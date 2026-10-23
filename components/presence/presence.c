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
#define MAX_NODES         24
#define NVS_NAMESPACE     "presence"
#define NVS_KEY_NAME      "name"

typedef struct __attribute__((packed)) {
    uint32_t id;
    int8_t rssi;
} wire_link_t;

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

typedef struct {
    presence_node_t node;
    uint32_t last_seen_ms;
} entry_t;

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

static void fill_links(presence_node_t *node)
{
    mesh_neighbor_t neighbors[PRESENCE_MAX_LINKS];
    node->n_links = mesh_neighbors(neighbors, PRESENCE_MAX_LINKS);
    for (int i = 0; i < node->n_links; i++) {
        node->links[i] = (presence_link_t){.id = neighbors[i].id, .rssi = neighbors[i].rssi};
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
            b.links[i] = (wire_link_t){.id = self.links[i].id, .rssi = self.links[i].rssi};
        }
        // Solo se mandan los links usados: el beacon es de largo variable.
        const size_t len = offsetof(wire_beacon_t, links) + self.n_links * sizeof(wire_link_t);
        mesh_send(MESH_TYPE_PRESENCE, &b, len, 0);
    }
}

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

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool added;
    entry_t *e = find_or_add(meta->origin, &added);
    presence_node_t *n = &e->node;
    // Se avisa como nuevo si no lo teniamos o si se habia caido.
    const bool was_alive = !added && mesh_now_ms() - e->last_seen_ms < NODE_TIMEOUT_MS;
    n->id = meta->origin;
    memcpy(n->name, b.name, PRESENCE_NAME_MAX);
    n->name[PRESENCE_NAME_MAX] = '\0';
    n->flags = b.flags;
    n->noise = b.noise;
    n->noise_peak = b.noise_peak;
    n->hops = meta->hops;
    n->uptime_s = b.uptime_s;
    n->n_links = n_links;
    for (int i = 0; i < n_links; i++) {
        n->links[i] = (presence_link_t){.id = b.links[i].id, .rssi = b.links[i].rssi};
    }
    e->last_seen_ms = mesh_now_ms();
    xSemaphoreGive(s_lock);

    if (!was_alive) {
        ESP_LOGI(TAG, "node %s joined (%u hops)", n->name, meta->hops);
        mesh_node_id_t id = meta->origin;
        esp_event_post(PRESENCE_EVENT, PRESENCE_EVENT_NODE_JOINED, &id, sizeof(id), 0);
    }
}

esp_err_t presence_init(const presence_config_t *cfg)
{
    s_cfg = *cfg;
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mem for lock");
    load_name();
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_PRESENCE, on_beacon, NULL), TAG, "register failed");
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
        const uint32_t age = now - s_nodes[i].last_seen_ms;
        if (age < NODE_TIMEOUT_MS) {
            out[n] = s_nodes[i].node;
            out[n].age_ms = age;
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}
