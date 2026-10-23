#include <stddef.h>
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
//
// El destino puede ser un celu: "entregado" es que llego a su placa, y
// "leido" que el celu lo vio (segundo ACK). Si el celu se pasa a otra placa
// antes de verlo, se le vuelve a mandar ahi; y si la placa nueva ya lo tenia en
// custodia, lo entrega directamente.

static const char *TAG = "courier";

#define TICK_MS          250
#define BACKOFF_MIN_MS   500
#define BACKOFF_MAX_MS   3000
#define RETRY_MS         20000  // reintento si el destino sigue vivo y no confirmo
#define MAX_ATTEMPTS     5      // por aparicion del destino
#define ACK_THROTTLE_MS  3000
#define MAX_AGE_MS       (2 * 60 * 60 * 1000)  // despues de 2 h dejamos de insistir

typedef enum {
    ACK_DELIVERED = 0,
    ACK_READ = 1,
} ack_kind_t;

typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
    uint8_t kind;  // ack_kind_t
} wire_ack_t;

typedef struct {
    uint32_t id;  // 0 = libre
    uint32_t last_tx_ms;
    uint32_t last_heard_ms;
    uint32_t due_ms;        // reenvio agendado (0 = ninguno)
    uint32_t scheduled_ms;  // cuando se agendo, para saber si alguien se adelanto
    uint32_t last_ack_ms;
    uint8_t last_ack_kind;
    uint8_t attempts;
} track_t;

static track_t s_tracks[STORE_MAX_MESSAGES];
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_joined;
static uint32_t s_scan[STORE_MAX_MESSAGES];  // ids, solo el task del courier

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

static bool is_private(const message_t *m)
{
    return m->kind != MESSAGE_ALERT && m->dest != MESSAGES_TO_ALL;
}

// Privado cuyo destino no esta aca y que todavia no llego a su placa:
// reintentar mientras el destino este vivo.
static bool needs_courier(const message_t *m)
{
    return is_private(m) && !messages_dest_is_here(m) && !m->delivered;
}

// Privado que todavia puede tener que salir de aca: sin entregar, o entregado
// a un celu que no lo vio (si se cambia de placa, hay que llevarselo).
static bool pending(const message_t *m)
{
    return is_private(m) && !messages_dest_is_here(m) && !m->read;
}

static void send_ack(const message_t *msg, ack_kind_t kind)
{
    const uint32_t now = mesh_now_ms();
    LOCK();
    track_t *t = track_of(msg->id);
    const bool throttled = t->last_ack_ms && t->last_ack_kind == kind && now - t->last_ack_ms < ACK_THROTTLE_MS;
    if (!throttled) {
        t->last_ack_ms = now;
        t->last_ack_kind = kind;
    }
    UNLOCK();
    if (throttled) {
        return;
    }
    const wire_ack_t w = {.origin = msg->origin, .ref = msg->ref, .kind = kind};
    if (mesh_send(MESH_TYPE_ACK, &w, sizeof(w), pdMS_TO_TICKS(50)) == ESP_OK) {
        ESP_LOGI(TAG, "%s %08" PRIx32 "/%u", kind == ACK_READ ? "read" : "ack", msg->origin, msg->ref);
    }
}

// La confirmacion que corresponde a lo que sabemos del mensaje.
static ack_kind_t ack_kind_of(const message_t *m)
{
    return m->read ? ACK_READ : ACK_DELIVERED;
}

static void schedule(track_t *t, uint32_t now)
{
    t->attempts = 0;
    t->due_ms = now + random_between(BACKOFF_MIN_MS, BACKOFF_MAX_MS);
    t->scheduled_ms = now;
}

static void resend(const message_t *msg)
{
    static const char *const names[] = {[MESSAGE_TEXT] = "text", [MESSAGE_VOICE] = "voice", [MESSAGE_FILE] = "file"};
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    switch (msg->kind) {
    case MESSAGE_TEXT:
        err = alerts_resend_text(msg);
        break;
    case MESSAGE_VOICE:
        err = voice_resend(msg);
        break;
    case MESSAGE_FILE:
        err = files_resend(msg);
        break;
    case MESSAGE_ALERT:
        break;
    }
    ESP_LOGI(TAG, "resend %s %08" PRIx32 "/%u to %08" PRIx32 ": %s", names[msg->kind], msg->origin, msg->ref,
             msg->dest, esp_err_to_name(err));
}

// Aparecio un nodo o un celu (o un celu cambio de placa): agendar los privados
// para el, entregar los que ya teniamos si ahora cuelga de aca, y reenviarle
// las confirmaciones de los suyos (las originales se le pudieron perder).
static void on_node_joined(mesh_node_id_t node)
{
    const uint32_t now = mesh_now_ms();
    const size_t n = store_ids(s_scan, STORE_MAX_MESSAGES, NULL);
    for (size_t i = 0; i < n; i++) {
        message_t msg;
        if (!store_get(s_scan[i], &msg)) {
            continue;
        }
        const message_t *m = &msg;
        if (!is_private(m)) {
            continue;
        }
        if (m->dest == node && messages_dest_is_here(m) && !m->delivered) {
            ESP_LOGI(TAG, "%08" PRIx32 " moved here, delivering %08" PRIx32 "/%u", node, m->origin, m->ref);
            courier_ack(m);
        } else if (m->dest == node && pending(m)) {
            LOCK();
            schedule(track_of(m->id), now);
            UNLOCK();
            ESP_LOGI(TAG, "%08" PRIx32 " is here, scheduling %08" PRIx32 "/%u", node, m->origin, m->ref);
        } else if (m->delivered && m->origin == node) {
            send_ack(m, ack_kind_of(m));
        }
    }
}

static void tick(void)
{
    const uint32_t now = mesh_now_ms();
    const size_t n = store_ids(s_scan, STORE_MAX_MESSAGES, pending);
    for (size_t i = 0; i < n; i++) {
        message_t msg;
        if (!store_get(s_scan[i], &msg)) {
            continue;
        }
        const message_t *m = &msg;
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
        } else if (needs_courier(m) && t->attempts < MAX_ATTEMPTS && presence_is_alive(m->dest)) {
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
    wire_ack_t w = {0};
    if (len < offsetof(wire_ack_t, kind)) {
        return;
    }
    memcpy(&w, payload, len < sizeof(w) ? len : sizeof(w));
    uint32_t id;
    if (!store_find(w.origin, w.ref, &id)) {
        return;
    }
    const bool read = w.kind == ACK_READ;
    if (read ? store_set_read(id) : store_set_delivered(id)) {
        ESP_LOGI(TAG, "%08" PRIx32 "/%u %s (ack from %08" PRIx32 ")", w.origin, w.ref, read ? "read" : "delivered",
                 meta->origin);
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
    // La placa ve un privado en cuanto le llega; un celu, cuando lo abre.
    if (msg->dest == mesh_self_id()) {
        store_set_read(msg->id);
    } else {
        store_set_delivered(msg->id);
    }
    message_t now;
    send_ack(msg, store_get(msg->id, &now) ? ack_kind_of(&now) : ACK_DELIVERED);
}

void courier_read(const message_t *msg)
{
    send_ack(msg, ACK_READ);
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
