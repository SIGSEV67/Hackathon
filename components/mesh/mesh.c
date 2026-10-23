#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "dedup.h"
#include "mesh.h"
#include "neighbors.h"

static const char *TAG = "mesh";

#define WIRE_MAGIC    0x53  // 'S'
#define WIRE_VERSION  1
#define FRAME_MAX     (MESH_HEADER_LEN + MESH_PAYLOAD_MAX)
#define RX_QUEUE_LEN  32
#define TX_QUEUE_LEN  24

// Formato en el aire. Todas las placas son ESP32-S3 (little endian), asi que el
// struct se manda tal cual.
typedef struct __attribute__((packed)) {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t ttl;
    uint8_t hops;
    uint8_t reserved;
    uint16_t seq;
    uint32_t origin;
} wire_header_t;

_Static_assert(sizeof(wire_header_t) == MESH_HEADER_LEN, "wire header size mismatch");

typedef struct {
    uint8_t len;
    int8_t rssi;
    bool forward;
    uint8_t src[LINK_ADDR_LEN];
    uint8_t data[FRAME_MAX];
} frame_t;

static const link_t *s_link;
static mesh_node_id_t s_self;
static uint16_t s_seq;
static QueueHandle_t s_rx_queue;
static QueueHandle_t s_tx_queue;
static mesh_stats_t s_stats;

static struct {
    mesh_handler_t fn;
    void *ctx;
} s_handlers[MESH_TYPE_MAX];

#define STAT_INC(field) __atomic_fetch_add(&s_stats.field, 1, __ATOMIC_RELAXED)

static mesh_node_id_t id_from_addr(const uint8_t addr[LINK_ADDR_LEN])
{
    return ((uint32_t)addr[2] << 24) | ((uint32_t)addr[3] << 16) | ((uint32_t)addr[4] << 8) | addr[5];
}

// Contexto del driver del link: solo copiar y encolar.
static void on_link_rx(const link_rx_info_t *info, const uint8_t *data, size_t len, void *ctx)
{
    if (len < MESH_HEADER_LEN || len > FRAME_MAX) {
        return;
    }
    frame_t f = {.len = len, .rssi = info->rssi};
    memcpy(f.src, info->src, LINK_ADDR_LEN);
    memcpy(f.data, data, len);
    if (xQueueSend(s_rx_queue, &f, 0) != pdTRUE) {
        STAT_INC(dropped);
    }
}

static void handle_frame(frame_t *f)
{
    wire_header_t h;
    memcpy(&h, f->data, sizeof(h));
    if (h.magic != WIRE_MAGIC || h.version != WIRE_VERSION) {
        return;
    }

    const mesh_node_id_t via = id_from_addr(f->src);
    neighbors_heard(via, f->rssi);

    if (h.origin == s_self) {
        return;  // un vecino nos devolvio un paquete propio
    }
    if (!dedup_check_and_add(h.origin, h.seq)) {
        STAT_INC(dup);
        return;
    }
    STAT_INC(rx);

    // Reenviar antes de procesar: el procesamiento local no tiene que sumar
    // latencia al resto de la red.
    if (h.ttl > 1) {
        frame_t fwd = *f;
        wire_header_t fh = h;
        fh.ttl--;
        fh.hops++;
        memcpy(fwd.data, &fh, sizeof(fh));
        fwd.forward = true;
        if (xQueueSend(s_tx_queue, &fwd, 0) == pdTRUE) {
            STAT_INC(fwd);
        } else {
            STAT_INC(dropped);
        }
    }

    if (h.type < MESH_TYPE_MAX && s_handlers[h.type].fn) {
        const mesh_rx_meta_t meta = {
            .origin = h.origin,
            .seq = h.seq,
            .hops = h.hops,
            .via = via,
            .rssi = f->rssi,
        };
        s_handlers[h.type].fn(&meta, f->data + MESH_HEADER_LEN, f->len - MESH_HEADER_LEN, s_handlers[h.type].ctx);
    }
}

static void rx_task(void *arg)
{
    static frame_t f;
    while (1) {
        xQueueReceive(s_rx_queue, &f, portMAX_DELAY);
        handle_frame(&f);
    }
}

static void tx_task(void *arg)
{
    static frame_t f;
    while (1) {
        xQueueReceive(s_tx_queue, &f, portMAX_DELAY);
        if (f.forward && CONFIG_SIGSEV_MESH_FORWARD_JITTER_MS > 0) {
            vTaskDelay(pdMS_TO_TICKS(esp_random() % (CONFIG_SIGSEV_MESH_FORWARD_JITTER_MS + 1)));
        }
        if (s_link->broadcast(f.data, f.len) != ESP_OK) {
            STAT_INC(dropped);
        }
    }
}

esp_err_t mesh_init(const link_t *link)
{
    ESP_RETURN_ON_FALSE(link && link->mtu >= FRAME_MAX, ESP_ERR_INVALID_ARG, TAG, "link mtu too small");
    s_link = link;

    uint8_t addr[LINK_ADDR_LEN];
    link->self_addr(addr);
    s_self = id_from_addr(addr);
    s_seq = (uint16_t)esp_random();  // asi un reinicio no choca con los seq viejos en el dedup de los vecinos
    dedup_reset();
    ESP_RETURN_ON_ERROR(neighbors_init(), TAG, "neighbors init failed");

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(frame_t));
    s_tx_queue = xQueueCreate(TX_QUEUE_LEN, sizeof(frame_t));
    ESP_RETURN_ON_FALSE(s_rx_queue && s_tx_queue, ESP_ERR_NO_MEM, TAG, "no mem for queues");

    ESP_RETURN_ON_FALSE(xTaskCreate(rx_task, "mesh_rx", 4096, NULL, 8, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "rx task failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(tx_task, "mesh_tx", 3072, NULL, 9, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "tx task failed");
    return ESP_OK;
}

esp_err_t mesh_start(void)
{
    ESP_RETURN_ON_FALSE(s_link, ESP_ERR_INVALID_STATE, TAG, "mesh_init first");
    ESP_RETURN_ON_ERROR(s_link->start(on_link_rx, NULL), TAG, "link start failed");

    char name[16];
    mesh_node_name(s_self, name, sizeof(name));
    ESP_LOGI(TAG, "node %s (%08" PRIx32 ") on link %s, ttl %d", name, s_self, s_link->name, CONFIG_SIGSEV_MESH_TTL);
    return ESP_OK;
}

mesh_node_id_t mesh_self_id(void)
{
    return s_self;
}

void mesh_node_name(mesh_node_id_t id, char *out, size_t len)
{
    snprintf(out, len, "SIGSEV-%04" PRIX32, id & 0xffff);
}

esp_err_t mesh_register_handler(mesh_type_t type, mesh_handler_t fn, void *ctx)
{
    ESP_RETURN_ON_FALSE(type > 0 && type < MESH_TYPE_MAX, ESP_ERR_INVALID_ARG, TAG, "bad type %d", type);
    ESP_RETURN_ON_FALSE(!s_handlers[type].fn, ESP_ERR_INVALID_STATE, TAG, "type %d already has a handler", type);
    s_handlers[type].fn = fn;
    s_handlers[type].ctx = ctx;
    return ESP_OK;
}

esp_err_t mesh_send(mesh_type_t type, const void *payload, size_t len, TickType_t wait)
{
    ESP_RETURN_ON_FALSE(len <= MESH_PAYLOAD_MAX, ESP_ERR_INVALID_SIZE, TAG, "payload too big (%u)", (unsigned)len);

    frame_t frame = {.len = MESH_HEADER_LEN + len, .forward = false};
    const wire_header_t h = {
        .magic = WIRE_MAGIC,
        .version = WIRE_VERSION,
        .type = type,
        .ttl = CONFIG_SIGSEV_MESH_TTL,
        .hops = 0,
        .seq = __atomic_fetch_add(&s_seq, 1, __ATOMIC_RELAXED),
        .origin = s_self,
    };
    memcpy(frame.data, &h, sizeof(h));
    memcpy(frame.data + MESH_HEADER_LEN, payload, len);

    if (xQueueSend(s_tx_queue, &frame, wait) != pdTRUE) {
        STAT_INC(dropped);
        return ESP_ERR_TIMEOUT;
    }
    STAT_INC(tx);
    return ESP_OK;
}

size_t mesh_neighbors(mesh_neighbor_t *out, size_t max)
{
    return neighbors_copy(out, max);
}

void mesh_get_stats(mesh_stats_t *out)
{
    *out = s_stats;
}

uint32_t mesh_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}
