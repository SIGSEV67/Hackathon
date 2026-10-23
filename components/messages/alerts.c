#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "internal.h"
#include "store.h"

static const char *TAG = "alerts";

typedef struct __attribute__((packed)) {
    uint8_t kind;
} wire_alert_t;

typedef struct __attribute__((packed)) {
    uint8_t len;
    char text[MESSAGES_TEXT_MAX];
} wire_text_t;

_Static_assert(sizeof(wire_text_t) <= MESH_PAYLOAD_MAX, "text does not fit in a mesh packet");

static void save(const message_t *msg)
{
    uint32_t id;
    if (store_add(msg, &id) == ESP_OK) {
        messages_notify(MESSAGES_EVENT_NEW, id);
    }
}

static void on_alert(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    const wire_alert_t *w = payload;
    if (len < sizeof(*w) || w->kind >= ALERT_COUNT) {
        return;
    }
    ESP_LOGW(TAG, "%s from %08" PRIx32 " (%u hops)", messages_alert_label(w->kind), meta->origin, meta->hops);
    save(&(message_t){
        .kind = MESSAGE_ALERT,
        .origin = meta->origin,
        .hops = meta->hops,
        .received_ms = mesh_now_ms(),
        .alert = w->kind,
    });
}

static void on_text(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    const wire_text_t *w = payload;
    if (len < 1 || w->len > MESSAGES_TEXT_MAX || len < 1 + (size_t)w->len) {
        return;
    }
    message_t msg = {
        .kind = MESSAGE_TEXT,
        .origin = meta->origin,
        .hops = meta->hops,
        .received_ms = mesh_now_ms(),
    };
    memcpy(msg.text, w->text, w->len);
    msg.text[w->len] = '\0';
    ESP_LOGI(TAG, "text from %08" PRIx32 ": %s", meta->origin, msg.text);
    save(&msg);
}

esp_err_t alerts_init(void)
{
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_ALERT, on_alert, NULL), TAG, "register alert failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_TEXT, on_text, NULL), TAG, "register text failed");
    return ESP_OK;
}

esp_err_t messages_send_alert(alert_kind_t kind)
{
    ESP_RETURN_ON_FALSE(kind < ALERT_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad alert kind");
    const wire_alert_t w = {.kind = kind};
    ESP_RETURN_ON_ERROR(mesh_send(MESH_TYPE_ALERT, &w, sizeof(w), pdMS_TO_TICKS(100)), TAG, "send failed");
    save(&(message_t){
        .kind = MESSAGE_ALERT,
        .origin = mesh_self_id(),
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
        .alert = kind,
    });
    return ESP_OK;
}

esp_err_t messages_send_text(const char *text)
{
    const size_t len = strlen(text);
    ESP_RETURN_ON_FALSE(len > 0 && len <= MESSAGES_TEXT_MAX, ESP_ERR_INVALID_SIZE, TAG, "bad text length");
    wire_text_t w = {.len = len};
    memcpy(w.text, text, len);
    ESP_RETURN_ON_ERROR(mesh_send(MESH_TYPE_TEXT, &w, 1 + len, pdMS_TO_TICKS(100)), TAG, "send failed");

    message_t msg = {
        .kind = MESSAGE_TEXT,
        .origin = mesh_self_id(),
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
    };
    strlcpy(msg.text, text, sizeof(msg.text));
    save(&msg);
    return ESP_OK;
}
