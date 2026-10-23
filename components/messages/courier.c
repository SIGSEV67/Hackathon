#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "presence.h"

#include "internal.h"
#include "store.h"

// Courier: hace llegar los privados (store-and-forward epidemico).
//
// Todo nodo que tiene un privado sin entregar (propio o en custodia) lo vuelve
// a mandar cuando ve aparecer al destino en la red, despues de una espera al
// azar: si en ese tiempo escucha que otro ya lo reenvio, no lo hace (asi no
// lo reenvian todos a la vez). Mientras el destino siga vivo y no confirme, se
// reintenta cada tanto, hasta un tope. El destino confirma con un ACK que se
// inunda por la red; quien lo escucha deja de reenviar.

static const char *TAG = "courier";

#define TICK_MS          250
#define BACKOFF_MIN_MS   500
#define BACKOFF_MAX_MS   3000
#define RETRY_MS         20000  // reintento si el destino sigue vivo y no confirmo
#define MAX_ATTEMPTS     5      // por aparicion del destino
#define ACK_THROTTLE_MS  3000
#define MAX_AGE_MS       (2 * 60 * 60 * 1000)  // despues de 2 h dejamos de insistir

typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
} wire_ack_t;

typedef struct {
    uint32_t id;  // 0 = libre
    uint32_t last_tx_ms;
    uint32_t last_heard_ms;
    uint32_t due_ms;        // reenvio agendado (0 = ninguno)
    uint32_t scheduled_ms;  // cuando se agendo, para saber si alguien se adelanto
    uint32_t last_ack_ms;
    uint8_t attempts;
} track_t;

static track_t s_tracks[STORE_MAX_MESSAGES];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_joined;
static message_t s_scan[STORE_MAX_MESSAGES];  // solo el task del courier

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static uint32_t random_between(uint32_t lo, uint32_t hi)
{
    return lo + esp_random() % (hi - lo + 1);
}

// Con el lock tomado. Crea el registro si no existe (pisando uno cuyo mensaje
// ya no esta en el store, o el mas viejo).
static track_t *track_of(uint32_t id)
{
    track_t *free_slot = NULL;
    track_t *oldest = &s_tracks[0];
    for (int i = 0; i < STORE_MAX_MESSAGES; i++) {
        track_t *t = &s_tracks[i];
        if (t->id == id) {
            return t;
        }
        if (!free_slot && (t->id == 0 || !store_get(t->id, NULL))) {
            free_slot = t;
        }
        if (t->id < oldest->id) {
            oldest = t;
        }
    }
    track_t *t = free_slot ? free_slot : oldest;
    *t = (track_t){.id = id};
    return t;
}

static bool needs_courier(const message_t *m)
{
    return m->kind != MESSAGE_ALERT && m->dest != MESSAGES_TO_ALL && m->dest != mesh_self_id() && !m->delivered;
}

static bool is_delivered_private(const message_t *m)
{
    return m->kind != MESSAGE_ALERT && m->dest != MESSAGES_TO_ALL && m->delivered;
}

static void send_ack(const message_t *msg)
{
    const uint32_t now = mesh_now_ms();
    LOCK();
    track_t *t = track_of(msg->id);
    const bool throttled = t->last_ack_ms && now - t->last_ack_ms < ACK_THROTTLE_MS;
    if (!throttled) {
        t->last_ack_ms = now;
    }
    UNLOCK();
    if (throttled) {
        return;
    }
    const wire_ack_t w = {.origin = msg->origin, .ref = msg->ref};
    if (mesh_send(MESH_TYPE_ACK, &w, sizeof(w), pdMS_TO_TICKS(50)) == ESP_OK) {
        ESP_LOGI(TAG, "ack %08" PRIx32 "/%u", msg->origin, msg->ref);
    }
}

static void resend(const message_t *msg)
{
    const esp_err_t err = msg->kind == MESSAGE_TEXT ? alerts_resend_text(msg) : voice_resend(msg);
    ESP_LOGI(TAG, "resend %s %08" PRIx32 "/%u to %08" PRIx32 ": %s", msg->kind == MESSAGE_TEXT ? "text" : "voice",
             msg->origin, msg->ref, msg->dest, esp_err_to_name(err));
}

// Aparecio un nodo: agendar los privados para el y reenviarle los ACKs de los
// suyos que ya sabemos entregados (el ACK original se lo pudo haber perdido).
static void on_node_joined(mesh_node_id_t node)
{
    const uint32_t now = mesh_now_ms();
    const size_t n = store_list(s_scan, STORE_MAX_MESSAGES, 0, NULL);
    for (size_t i = 0; i < n; i++) {
        const message_t *m = &s_scan[i];
        if (needs_courier(m) && m->dest == node) {
            LOCK();
            track_t *t = track_of(m->id);
            t->attempts = 0;
            t->due_ms = now + random_between(BACKOFF_MIN_MS, BACKOFF_MAX_MS);
            t->scheduled_ms = now;
            UNLOCK();
            ESP_LOGI(TAG, "%08" PRIx32 " is here, scheduling %08" PRIx32 "/%u", node, m->origin, m->ref);
        } else if (is_delivered_private(m) && m->origin == node) {
            send_ack(m);
        }
    }
}

static void tick(void)
{
    const uint32_t now = mesh_now_ms();
    const size_t n = store_list(s_scan, STORE_MAX_MESSAGES, 0, needs_courier);
    for (size_t i = 0; i < n; i++) {
        const message_t *m = &s_scan[i];
        if (now - m->received_ms > MAX_AGE_MS) {
            continue;
        }
        bool fire = false;
        LOCK();
        track_t *t = track_of(m->id);
        if (t->due_ms) {
            if ((int32_t)(now - t->due_ms) >= 0) {
                // Si otro lo reenvio mientras esperabamos, nos ahorramos el nuestro.
                fire = !(t->last_heard_ms && (int32_t)(t->last_heard_ms - t->scheduled_ms) > 0);
                t->due_ms = 0;
                t->attempts++;
                if (fire) {
                    t->last_tx_ms = now;
                }
            }
        } else if (t->attempts < MAX_ATTEMPTS && presence_is_alive(m->dest)) {
            const uint32_t last = (int32_t)(t->last_heard_ms - t->last_tx_ms) > 0 ? t->last_heard_ms : t->last_tx_ms;
            if (now - last > RETRY_MS) {
                t->due_ms = now + random_between(0, BACKOFF_MAX_MS);
                t->scheduled_ms = now;
            }
        }
        UNLOCK();
        if (fire) {
            resend(m);
        }
    }
}

static void courier_task(void *arg)
{
    while (1) {
        mesh_node_id_t node;
        while (xQueueReceive(s_joined, &node, 0) == pdTRUE) {
            on_node_joined(node);
        }
        tick();
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

static void on_presence(void *arg, esp_event_base_t base, int32_t event_id, void *data)
{
    xQueueSend(s_joined, data, 0);
}

static void on_ack(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_ack_t w;
    if (len < sizeof(w)) {
        return;
    }
    memcpy(&w, payload, sizeof(w));
    uint32_t id;
    if (store_find(w.origin, w.ref, &id) && store_set_delivered(id)) {
        ESP_LOGI(TAG, "%08" PRIx32 "/%u delivered (ack from %08" PRIx32 ")", w.origin, w.ref, meta->origin);
        messages_notify(MESSAGES_EVENT_DELIVERED, id);
    }
}

esp_err_t courier_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_joined = xQueueCreate(8, sizeof(mesh_node_id_t));
    ESP_RETURN_ON_FALSE(s_lock && s_joined, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_ACK, on_ack, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(PRESENCE_EVENT, PRESENCE_EVENT_NODE_JOINED, on_presence, NULL), TAG,
        "presence handler failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(courier_task, "courier", 4096, NULL, 4, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    return ESP_OK;
}

void courier_ack(const message_t *msg)
{
    store_set_delivered(msg->id);
    send_ack(msg);
}

void courier_heard(uint32_t id)
{
    LOCK();
    track_of(id)->last_heard_ms = mesh_now_ms();
    UNLOCK();
}

void courier_sent(uint32_t id)
{
    LOCK();
    track_of(id)->last_tx_ms = mesh_now_ms();
    UNLOCK();
}
