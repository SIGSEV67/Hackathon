#include "esp_check.h"
#include "sdkconfig.h"

#include "internal.h"
#include "store.h"

static const char *TAG = "messages";

ESP_EVENT_DEFINE_BASE(MESSAGES_EVENT);

static const char *const s_alert_labels[ALERT_COUNT] = {
    [ALERT_SOS] = "SOS",
    [ALERT_MEDIC] = "MEDICO",
    [ALERT_DANGER] = "PELIGRO",
    [ALERT_SAFE] = "ESTOY BIEN",
};

esp_err_t messages_init(void)
{
    ESP_RETURN_ON_ERROR(store_init(CONFIG_SIGSEV_VOICE_STORE_KB * 1024), TAG, "store init failed");
    ESP_RETURN_ON_ERROR(alerts_init(), TAG, "alerts init failed");
    ESP_RETURN_ON_ERROR(voice_init(), TAG, "voice init failed");
    return ESP_OK;
}

void messages_notify(messages_event_id_t event, uint32_t id)
{
    esp_event_post(MESSAGES_EVENT, event, &id, sizeof(id), pdMS_TO_TICKS(10));
}

const char *messages_alert_label(alert_kind_t kind)
{
    return kind < ALERT_COUNT ? s_alert_labels[kind] : "?";
}

size_t messages_list(message_t *out, size_t max, uint32_t after_id)
{
    return store_list(out, max, after_id);
}

bool messages_get(uint32_t id, message_t *out)
{
    return store_get(id, out);
}
