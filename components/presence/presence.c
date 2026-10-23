#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "presence.h"

static const char *TAG = "presence";

ESP_EVENT_DEFINE_BASE(PRESENCE_EVENT);

#define BEACON_PERIOD_MS  3000
#define BEACON_JITTER_MS  500
#define NODE_TIMEOUT_MS   (4 * BEACON_PERIOD_MS)
#define LOCAL_CLIENT_MS   30000  // la web consulta cada ~2 s; con la pantalla apagada deja de hacerlo
#define MAX_CLIENTS_WIRE  8
#define MAX_NODES         PRESENCE_MAX_KNOWN
#define NVS_NAMESPACE     "presence"
#define NVS_KEY_NAME      "name"
#define NVS_KEY_KNOWN     "known2"  // "known" era el formato sin clientes

typedef struct __attribute__((packed)) {
    uint32_t id;
    int8_t rssi;
    uint16_t dist_dm;
    uint8_t flags;  // bit 0: distancia por FTM
} wire_link_t;

#define LINK_FLAG_FTM 1

typedef struct __attribute__((packed)) {
    char name[PRESENCE_NAME_MAX];  // sin terminador si ocupa los 20
    uint8_t flags;
    uint8_t noise;
    uint8_t noise_peak;
    uint8_t n_links;
    uint32_t uptime_s;
    wire_link_t links[PRESENCE_MAX_LINKS];
} wire_beacon_t;

_Static_assert(sizeof(wire_beacon_t) <= MESH_PAYLOAD_MAX, "beacon does not fit in a mesh packet");

// Clientes de una placa: va aparte del beacon porque no entra en el mismo paquete.
typedef struct __attribute__((packed)) {
    uint32_t id;
    char name[PRESENCE_NAME_MAX];
} wire_client_t;

typedef struct __attribute__((packed)) {
    uint8_t n_clients;
    wire_client_t clients[MAX_CLIENTS_WIRE];
} wire_clients_t;

_Static_assert(sizeof(wire_clients_t) <= MESH_PAYLOAD_MAX, "client list does not fit in a mesh packet");

typedef struct {
    presence_node_t node;  // de un cliente solo se usan id y name
    presence_kind_t kind;
    mesh_node_id_t parent;
    uint32_t last_seen_ms;
    bool seen;  // lo escuchamos en este arranque (si no, viene del historial en NVS)
} entry_t;

// Lo que se guarda del historial: quien es, que es y como se llama.
typedef struct __attribute__((packed)) {
    uint32_t id;
    uint8_t kind;
    uint32_t parent;
    char name[PRESENCE_NAME_MAX];
} known_record_t;

static presence_config_t s_cfg;
static char s_name[PRESENCE_NAME_MAX + 1];
static entry_t s_nodes[MAX_NODES];
static int s_count;
static SemaphoreHandle_t s_lock;

static void load_name(void)
{
    mesh_node_name(mesh_self_id(), s_name, sizeof(s_name));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_name);
        if (nvs_get_str(nvs, NVS_KEY_NAME, s_name, &len) != ESP_OK) {
            mesh_node_name(mesh_self_id(), s_name, sizeof(s_name));
        }
        nvs_close(nvs);
    }
}

static bool is_local(const entry_t *e)
{
    return e->kind == PRESENCE_KIND_CLIENT && e->parent == mesh_self_id();
}

static bool is_alive(const entry_t *e, uint32_t now)
{
    return e->seen && now - e->last_seen_ms < (is_local(e) ? LOCAL_CLIENT_MS : NODE_TIMEOUT_MS);
}

// Historial de nodos conocidos en NVS, para poder escribirles aunque esten
// fuera de alcance o despues de un reinicio.
static void load_known(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    static known_record_t records[MAX_NODES];
    size_t len = sizeof(records);
    if (nvs_get_blob(nvs, NVS_KEY_KNOWN, records, &len) == ESP_OK) {
        s_count = len / sizeof(known_record_t);
        for (int i = 0; i < s_count; i++) {
            memset(&s_nodes[i], 0, sizeof(s_nodes[i]));
            s_nodes[i].node.id = records[i].id;
            s_nodes[i].kind = records[i].kind;
            s_nodes[i].parent = records[i].parent;
            memcpy(s_nodes[i].node.name, records[i].name, PRESENCE_NAME_MAX);
            s_nodes[i].node.name[PRESENCE_NAME_MAX] = '\0';
        }
    }
    nvs_close(nvs);
}

static void save_known(void)
{
    static known_record_t records[MAX_NODES];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int count = s_count;
    for (int i = 0; i < count; i++) {
        records[i].id = s_nodes[i].node.id;
        records[i].kind = s_nodes[i].kind;
        records[i].parent = s_nodes[i].parent;
        memcpy(records[i].name, s_nodes[i].node.name, PRESENCE_NAME_MAX);
    }
    xSemaphoreGive(s_lock);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_set_blob(nvs, NVS_KEY_KNOWN, records, count * sizeof(known_record_t)) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

static void fill_links(presence_node_t *node)
{
    mesh_neighbor_t neighbors[PRESENCE_MAX_LINKS];
    node->n_links = mesh_neighbors(neighbors, PRESENCE_MAX_LINKS);
    for (int i = 0; i < node->n_links; i++) {
        presence_link_t *l = &node->links[i];
        *l = (presence_link_t){.id = neighbors[i].id, .rssi = neighbors[i].rssi};
        if (s_cfg.link_distance && !s_cfg.link_distance(l->id, &l->dist_dm, &l->dist_ftm)) {
            l->dist_dm = 0;
            l->dist_ftm = false;
        }
    }
}

static void build_self(presence_node_t *out, bool take_peak)
{
    memset(out, 0, sizeof(*out));
    out->id = mesh_self_id();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out->name, s_name, sizeof(out->name));
    xSemaphoreGive(s_lock);
    out->flags = s_cfg.flags;
    out->noise = s_cfg.noise_level ? s_cfg.noise_level() : 0;
    if (s_cfg.noise_take_peak) {
        out->noise_peak = take_peak ? s_cfg.noise_take_peak() : out->noise;
    }
    out->uptime_s = mesh_now_ms() / 1000;
    fill_links(out);
}

static void send_clients(void)
{
    wire_clients_t w = {0};
    const uint32_t now = mesh_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && w.n_clients < MAX_CLIENTS_WIRE; i++) {
        if (is_local(&s_nodes[i]) && is_alive(&s_nodes[i], now)) {
            wire_client_t *c = &w.clients[w.n_clients++];
            c->id = s_nodes[i].node.id;
            strncpy(c->name, s_nodes[i].node.name, sizeof(c->name));
        }
    }
    xSemaphoreGive(s_lock);
    if (w.n_clients) {
        mesh_send(MESH_TYPE_CLIENTS, &w, offsetof(wire_clients_t, clients) + w.n_clients * sizeof(wire_client_t), 0);
    }
}

static void beacon_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(BEACON_PERIOD_MS - BEACON_JITTER_MS + esp_random() % (2 * BEACON_JITTER_MS)));

        presence_node_t self;
        build_self(&self, true);

        wire_beacon_t b = {
            .flags = self.flags,
            .noise = self.noise,
            .noise_peak = self.noise_peak,
            .n_links = self.n_links,
            .uptime_s = self.uptime_s,
        };
        strncpy(b.name, self.name, sizeof(b.name));
        for (int i = 0; i < self.n_links; i++) {
            const presence_link_t *l = &self.links[i];
            b.links[i] = (wire_link_t){
                .id = l->id, .rssi = l->rssi, .dist_dm = l->dist_dm, .flags = l->dist_ftm ? LINK_FLAG_FTM : 0};
        }
        // Solo se mandan los links usados: el beacon es de largo variable.
        const size_t len = offsetof(wire_beacon_t, links) + self.n_links * sizeof(wire_link_t);
        mesh_send(MESH_TYPE_PRESENCE, &b, len, 0);
        send_clients();
    }
}

static void announce_joined(mesh_node_id_t id, const char *name, const char *what);

static entry_t *find_or_add(mesh_node_id_t id, bool *added)
{
    *added = false;
    for (int i = 0; i < s_count; i++) {
        if (s_nodes[i].node.id == id) {
            return &s_nodes[i];
        }
    }
    entry_t *slot;
    if (s_count < MAX_NODES) {
        slot = &s_nodes[s_count++];
    } else {
        // Pisa al que hace mas que no escuchamos (los del historial que no
        // aparecieron en este arranque tienen last_seen_ms = 0).
        slot = &s_nodes[0];
        for (int i = 1; i < s_count; i++) {
            if (s_nodes[i].last_seen_ms < slot->last_seen_ms) {
                slot = &s_nodes[i];
            }
        }
    }
    memset(slot, 0, sizeof(*slot));
    *added = true;
    return slot;
}

static void on_beacon(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_beacon_t b = {0};
    if (len < offsetof(wire_beacon_t, links) || len > sizeof(b)) {
        return;
    }
    memcpy(&b, payload, len);
    const int n_links = b.n_links;
    if (n_links > PRESENCE_MAX_LINKS || len < offsetof(wire_beacon_t, links) + n_links * sizeof(wire_link_t)) {
        return;
    }

    const uint32_t now = mesh_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool added;
    entry_t *e = find_or_add(meta->origin, &added);
    presence_node_t *n = &e->node;
    // Se avisa como nuevo si no lo teniamos, si se habia caido o si viene del
    // historial y es la primera vez que aparece en este arranque.
    const bool was_alive = !added && is_alive(e, now);
    const bool renamed = added || strncmp(n->name, b.name, PRESENCE_NAME_MAX) != 0;
    n->id = meta->origin;
    e->kind = PRESENCE_KIND_NODE;
    memcpy(n->name, b.name, PRESENCE_NAME_MAX);
    n->name[PRESENCE_NAME_MAX] = '\0';
    char name[PRESENCE_NAME_MAX + 1];
    strlcpy(name, n->name, sizeof(name));
    n->flags = b.flags;
    n->noise = b.noise;
    n->noise_peak = b.noise_peak;
    n->hops = meta->hops;
    n->uptime_s = b.uptime_s;
    n->n_links = n_links;
    for (int i = 0; i < n_links; i++) {
        n->links[i] = (presence_link_t){
            .id = b.links[i].id,
            .rssi = b.links[i].rssi,
            .dist_dm = b.links[i].dist_dm,
            .dist_ftm = b.links[i].flags & LINK_FLAG_FTM,
        };
    }
    e->last_seen_ms = now;
    e->seen = true;
    xSemaphoreGive(s_lock);

    if (renamed) {
        save_known();
    }
    if (!was_alive) {
        announce_joined(meta->origin, name, meta->hops ? "node (multi-hop)" : "node");
    }
}

// Con el lock tomado. Registra que el cliente esta colgado de `parent` y dice
// si hay que avisarlo como recien llegado (nuevo, vuelve o cambio de placa).
static bool client_heard(mesh_node_id_t id, const char *name, mesh_node_id_t parent, uint32_t now, bool *renamed)
{
    bool added;
    entry_t *e = find_or_add(id, &added);
    const bool joined = added || !is_alive(e, now) || e->parent != parent;
    *renamed = added || strncmp(e->node.name, name, PRESENCE_NAME_MAX) != 0 || e->parent != parent;
    e->node.id = id;
    e->kind = PRESENCE_KIND_CLIENT;
    e->parent = parent;
    strlcpy(e->node.name, name, sizeof(e->node.name));
    e->last_seen_ms = now;
    e->seen = true;
    return joined;
}

static void announce_joined(mesh_node_id_t id, const char *name, const char *what)
{
    ESP_LOGI(TAG, "%s %s joined", what, name);
    esp_event_post(PRESENCE_EVENT, PRESENCE_EVENT_NODE_JOINED, &id, sizeof(id), 0);
}

static void on_clients(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_clients_t w;
    if (len < offsetof(wire_clients_t, clients) || len > sizeof(w)) {
        return;
    }
    memcpy(&w, payload, len);
    if (w.n_clients > MAX_CLIENTS_WIRE || len < offsetof(wire_clients_t, clients) + w.n_clients * sizeof(wire_client_t)) {
        return;
    }
    const uint32_t now = mesh_now_ms();
    bool save = false;
    for (int i = 0; i < w.n_clients; i++) {
        char name[PRESENCE_NAME_MAX + 1];
        memcpy(name, w.clients[i].name, PRESENCE_NAME_MAX);
        name[PRESENCE_NAME_MAX] = '\0';
        const mesh_node_id_t id = w.clients[i].id;
        if (id == 0 || id == mesh_self_id()) {
            continue;
        }
        bool renamed;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool joined = client_heard(id, name, meta->origin, now, &renamed);
        xSemaphoreGive(s_lock);
        save |= renamed;
        if (joined) {
            announce_joined(id, name, "client");
        }
    }
    if (save) {
        save_known();
    }
}

esp_err_t presence_client_seen(mesh_node_id_t id, const char *name)
{
    ESP_RETURN_ON_FALSE(id && id != mesh_self_id(), ESP_ERR_INVALID_ARG, TAG, "bad client id");
    char clean[PRESENCE_NAME_MAX + 1];
    strlcpy(clean, name && name[0] ? name : "celu", sizeof(clean));
    const uint32_t now = mesh_now_ms();
    bool is_node = false, joined = false, renamed = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && !is_node; i++) {
        is_node = s_nodes[i].node.id == id && s_nodes[i].kind == PRESENCE_KIND_NODE && s_nodes[i].seen;
    }
    if (!is_node) {
        joined = client_heard(id, clean, mesh_self_id(), now, &renamed);
    }
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_FALSE(!is_node, ESP_ERR_INVALID_ARG, TAG, "id %08" PRIx32 " is a node", id);
    if (renamed) {
        save_known();
    }
    if (joined) {
        announce_joined(id, clean, "local client");
    }
    return ESP_OK;
}

bool presence_is_local_client(mesh_node_id_t id)
{
    const uint32_t now = mesh_now_ms();
    bool local = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_nodes[i].node.id == id) {
            local = is_local(&s_nodes[i]) && is_alive(&s_nodes[i], now);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return local;
}

esp_err_t presence_init(const presence_config_t *cfg)
{
    s_cfg = *cfg;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mem for lock");
    load_name();
    load_known();
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_PRESENCE, on_beacon, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_CLIENTS, on_clients, NULL), TAG, "register failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(beacon_task, "presence", 3072, NULL, 4, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    ESP_LOGI(TAG, "announcing as \"%s\"", s_name);
    return ESP_OK;
}

void presence_self_name(char *out, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_name, len);
    xSemaphoreGive(s_lock);
}

esp_err_t presence_set_name(const char *name)
{
    ESP_RETURN_ON_FALSE(name && name[0] && strlen(name) <= PRESENCE_NAME_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "bad name");
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "nvs open failed");
    esp_err_t err = nvs_set_str(nvs, NVS_KEY_NAME, name);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "nvs write failed");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_name, name, sizeof(s_name));
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "renamed to \"%s\"", name);
    return ESP_OK;
}

void presence_name_of(mesh_node_id_t id, char *out, size_t len)
{
    if (id == mesh_self_id()) {
        presence_self_name(out, len);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_nodes[i].node.id == id) {
            strlcpy(out, s_nodes[i].node.name, len);
            xSemaphoreGive(s_lock);
            return;
        }
    }
    xSemaphoreGive(s_lock);
    mesh_node_name(id, out, len);
}

void presence_self(presence_node_t *out)
{
    build_self(out, false);
}

size_t presence_nodes(presence_node_t *out, size_t max)
{
    const uint32_t now = mesh_now_ms();
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max; i++) {
        if (s_nodes[i].kind == PRESENCE_KIND_NODE && is_alive(&s_nodes[i], now)) {
            out[n] = s_nodes[i].node;
            out[n].age_ms = now - s_nodes[i].last_seen_ms;
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

size_t presence_known(presence_known_t *out, size_t max)
{
    const uint32_t now = mesh_now_ms();
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max; i++) {
        const entry_t *e = &s_nodes[i];
        out[n] = (presence_known_t){
            .id = e->node.id,
            .kind = e->kind,
            .parent = e->parent,
            .alive = is_alive(e, now),
            .seen = e->seen,
            .age_ms = e->seen ? now - e->last_seen_ms : 0,
        };
        strlcpy(out[n].name, e->node.name, sizeof(out[n].name));
        n++;
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool presence_is_alive(mesh_node_id_t id)
{
    const uint32_t now = mesh_now_ms();
    bool alive = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (s_nodes[i].node.id == id) {
            alive = is_alive(&s_nodes[i], now);
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return alive;
}
