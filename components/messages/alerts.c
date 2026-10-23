#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "internal.h"
#include "store.h"

static const char *TAG = "alerts";

typedef struct __attribute__((packed)) {
    uint8_t kind;
    uint32_t origin;  // la placa o el celu que la mando
} wire_alert_t;

// origin va en el payload: un custodio reenvia con su propio header de mesh.
typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
    uint32_t dest;  // MESSAGES_TO_ALL o el destino de un privado
    uint8_t len;
    char text[MESSAGES_TEXT_MAX];
} wire_text_t;

#define TEXT_HEADER_LEN offsetof(wire_text_t, text)

_Static_assert(sizeof(wire_text_t) <= MESH_PAYLOAD_MAX, "text does not fit in a mesh packet");

static void save(message_t *msg)
{
    if (store_add(msg, &msg->id) == ESP_OK && messages_is_local(msg)) {
        messages_notify(MESSAGES_EVENT_NEW, msg->id);
    }
}

static void on_alert(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_alert_t w = {0};
    if (len < 1) {
        return;
    }
    memcpy(&w, payload, len < sizeof(w) ? len : sizeof(w));
    if (w.kind >= ALERT_COUNT) {
        return;
    }
    const mesh_node_id_t origin = len >= sizeof(w) && w.origin ? w.origin : meta->origin;
    ESP_LOGW(TAG, "%s from %08" PRIx32 " (%u hops)", messages_alert_label(w.kind), origin, meta->hops);
    save(&(message_t){
        .kind = MESSAGE_ALERT,
        .origin = origin,
        .hops = meta->hops,
        .received_ms = mesh_now_ms(),
        .alert = w.kind,
    });
}

static void on_text(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_text_t w;
    if (len < TEXT_HEADER_LEN || len > sizeof(w)) {
        return;
    }
    memcpy(&w, payload, len);
    if (w.len == 0 || w.len > MESSAGES_TEXT_MAX || len < TEXT_HEADER_LEN + w.len) {
        return;
    }

    // Un privado puede llegar varias veces (reenvios de los custodios).
    uint32_t id;
    if (store_find(w.origin, w.ref, &id)) {
        courier_heard(id);
        message_t known;
        if (store_get(id, &known) && messages_dest_is_here(&known)) {
            courier_ack(&known);  // nuestro ACK se pudo haber perdido
        }
        return;
    }

    message_t msg = {
        .kind = MESSAGE_TEXT,
        .origin = w.origin,
        .ref = w.ref,
        .dest = w.dest,
        .hops = meta->hops,
        .received_ms = mesh_now_ms(),
    };
    memcpy(msg.text, w.text, w.len);
    msg.text[w.len] = '\0';

    if (messages_in_transit(&msg)) {
        ESP_LOGI(TAG, "holding private text %08" PRIx32 "/%u for %08" PRIx32, msg.origin, msg.ref, msg.dest);
    } else {
        ESP_LOGI(TAG, "%s from %08" PRIx32 ": %s", msg.dest ? "private text" : "text", msg.origin, msg.text);
    }
    save(&msg);
    if (msg.id && messages_dest_is_here(&msg)) {
        courier_ack(&msg);
    }
}

static esp_err_t send_text_wire(const message_t *msg)
{
    const size_t len = strlen(msg->text);
    wire_text_t w = {.origin = msg->origin, .ref = msg->ref, .dest = msg->dest, .len = len};
    memcpy(w.text, msg->text, len);
    return mesh_send(MESH_TYPE_TEXT, &w, TEXT_HEADER_LEN + len, pdMS_TO_TICKS(100));
}

esp_err_t alerts_init(void)
{
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_ALERT, on_alert, NULL), TAG, "register alert failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_TEXT, on_text, NULL), TAG, "register text failed");
    return ESP_OK;
}

esp_err_t alerts_resend_text(const message_t *msg)
{
    return send_text_wire(msg);
}

esp_err_t messages_send_alert(mesh_node_id_t from, alert_kind_t kind)
{
    ESP_RETURN_ON_FALSE(kind < ALERT_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad alert kind");
    ESP_RETURN_ON_FALSE(messages_valid_sender(from), ESP_ERR_INVALID_ARG, TAG, "unknown sender");
    const wire_alert_t w = {.kind = kind, .origin = from};
    ESP_RETURN_ON_ERROR(mesh_send(MESH_TYPE_ALERT, &w, sizeof(w), pdMS_TO_TICKS(100)), TAG, "send failed");
    save(&(message_t){
        .kind = MESSAGE_ALERT,
        .origin = from,
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
        .alert = kind,
    });
    return ESP_OK;
}

esp_err_t messages_send_text(mesh_node_id_t from, mesh_node_id_t dest, const char *text)
{
    const size_t len = strlen(text);
    ESP_RETURN_ON_FALSE(len > 0 && len <= MESSAGES_TEXT_MAX, ESP_ERR_INVALID_SIZE, TAG, "bad text length");
    ESP_RETURN_ON_FALSE(messages_valid_sender(from), ESP_ERR_INVALID_ARG, TAG, "unknown sender");
    ESP_RETURN_ON_FALSE(dest != from, ESP_ERR_INVALID_ARG, TAG, "can't send to self");

    message_t msg = {
        .kind = MESSAGE_TEXT,
        .origin = from,
        .ref = messages_next_ref(),
        .dest = dest,
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
    };
    strlcpy(msg.text, text, sizeof(msg.text));
    // Un privado se guarda aunque el envio falle: el courier lo reintenta.
    const esp_err_t err = send_text_wire(&msg);
    ESP_RETURN_ON_FALSE(err == ESP_OK || dest != MESSAGES_TO_ALL, err, TAG, "send failed");

    uint32_t id;
    ESP_RETURN_ON_ERROR(store_add(&msg, &id), TAG, "store failed");
    messages_notify(MESSAGES_EVENT_NEW, id);
    if (err == ESP_OK) {
        courier_sent(id);
    }
    return ESP_OK;
}
