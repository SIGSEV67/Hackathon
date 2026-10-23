#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "presence.h"

#include "internal.h"

// Bloques perdidos. El flooding no tiene reintentos: a una voz le puede faltar
// algun bloque (se oye un hueco de 55 ms) y a un archivo no le puede faltar
// ninguno. Quien tiene un mensaje incompleto y deja de recibir pide los rangos
// que le faltan (NACK). Contesta el origen; si el origen ya no esta en la red,
// cualquiera que tenga esos bloques (puede haber algun duplicado: el store los
// descarta).

static const char *TAG = "repair";

#define TICK_MS        500
#define STALL_MS       1500  // sin bloques nuevos durante esto: pedir lo que falta
#define MAX_ROUNDS     6     // pedidos seguidos sin progreso antes de rendirse
#define MAX_RANGES     40
#define MAX_JOBS       8     // mas rangos que esto se piden como uno solo

typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
    uint8_t n_ranges;  // 0 = solo los datos del archivo
    block_range_t ranges[MAX_RANGES];
} wire_nack_t;

_Static_assert(sizeof(wire_nack_t) <= MESH_PAYLOAD_MAX, "nack does not fit in a mesh packet");

typedef struct {
    uint32_t id;
    uint16_t received;
    uint32_t changed_ms;
    uint8_t rounds;
} track_t;

static track_t s_tracks[STORE_MAX_MESSAGES];
static uint32_t s_ids[STORE_MAX_MESSAGES];  // solo el task de repair

// Voz o archivo a medio llegar que este nodo quiere completar: le interesa
// (es visible) y no es propio.
static bool wants_repair(const message_t *m)
{
    return (m->kind == MESSAGE_VOICE || m->kind == MESSAGE_FILE) && messages_is_visible(m) &&
           m->origin != mesh_self_id() &&
           (m->blocks_received < m->blocks || (m->kind == MESSAGE_FILE && m->file_size == 0));
}

static track_t *track_of(uint32_t id)
{
    track_t *slot = &s_tracks[0];
    for (int i = 0; i < STORE_MAX_MESSAGES; i++) {
        if (s_tracks[i].id == id) {
            return &s_tracks[i];
        }
        if (s_tracks[i].id < slot->id) {
            slot = &s_tracks[i];  // el mas viejo (o uno libre, id 0)
        }
    }
    *slot = (track_t){.id = id, .received = UINT16_MAX};
    return slot;
}

static void send_nack(const message_t *m)
{
    block_range_t ranges[MAX_RANGES];  // aparte: w es packed
    wire_nack_t w = {.origin = m->origin, .ref = m->ref};
    w.n_ranges = store_missing(m->id, ranges, MAX_RANGES);
    memcpy(w.ranges, ranges, w.n_ranges * sizeof(block_range_t));
    const size_t len = offsetof(wire_nack_t, ranges) + w.n_ranges * sizeof(block_range_t);
    if (mesh_send(MESH_TYPE_NACK, &w, len, pdMS_TO_TICKS(50)) == ESP_OK) {
        ESP_LOGI(TAG, "asked %08" PRIx32 "/%u for %u ranges (%u/%u blocks)", m->origin, m->ref, w.n_ranges,
                 m->blocks_received, m->blocks);
    }
}

static void tick(void)
{
    const uint32_t now = mesh_now_ms();
    const size_t n = store_ids(s_ids, STORE_MAX_MESSAGES, wants_repair);
    for (size_t i = 0; i < n; i++) {
        message_t m;
        if (!store_get(s_ids[i], &m)) {
            continue;
        }
        track_t *t = track_of(m.id);
        if (t->received != m.blocks_received) {
            t->received = m.blocks_received;
            t->changed_ms = now;
            t->rounds = 0;
            continue;
        }
        if (now - t->changed_ms >= STALL_MS && t->rounds < MAX_ROUNDS) {
            send_nack(&m);
            t->rounds++;
            t->changed_ms = now;  // darle tiempo a la respuesta
        }
    }
}

static void repair_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        tick();
    }
}

static void on_nack(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_nack_t w;
    if (len < offsetof(wire_nack_t, ranges) || len > sizeof(w)) {
        return;
    }
    memcpy(&w, payload, len);
    if (w.n_ranges > MAX_RANGES || len < offsetof(wire_nack_t, ranges) + w.n_ranges * sizeof(block_range_t)) {
        return;
    }
    uint32_t id;
    message_t m;
    if (!store_find(w.origin, w.ref, &id) || !store_get(id, &m)) {
        return;
    }
    const bool is_origin = w.origin == mesh_self_id();
    if (!is_origin && presence_is_alive(w.origin)) {
        return;  // contesta el origen
    }
    ESP_LOGI(TAG, "%08" PRIx32 " asked for %u ranges of %08" PRIx32 "/%u", meta->origin, w.n_ranges, w.origin, w.ref);
    if (w.n_ranges == 0) {
        sender_post(id, 0, 0, true);
        return;
    }
    if (w.n_ranges > MAX_JOBS) {
        // Muchos huecos: un solo envio de punta a punta (manda tambien lo que
        // ya tenia, pero no llena la cola del sender).
        const block_range_t last = w.ranges[w.n_ranges - 1];
        sender_post(id, w.ranges[0].start, last.start + last.count - w.ranges[0].start, true);
        return;
    }
    for (uint8_t i = 0; i < w.n_ranges; i++) {
        sender_post(id, w.ranges[i].start, w.ranges[i].count, i == 0);
    }
}

esp_err_t repair_init(void)
{
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_NACK, on_nack, NULL), TAG, "register failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(repair_task, "repair", 3072, NULL, 3, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    return ESP_OK;
}
