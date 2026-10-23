#include "esp_check.h"
#include "esp_random.h"
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
    ESP_RETURN_ON_ERROR(courier_init(), TAG, "courier init failed");
    ESP_RETURN_ON_ERROR(player_init(), TAG, "player init failed");
    return ESP_OK;
}

uint16_t messages_next_ref(void)
{
    // Arranca al azar para que un reinicio no repita refs viejas.
    static uint16_t next;
    static bool seeded;
    if (!seeded) {
        next = (uint16_t)esp_random();
        seeded = true;
    }
    return next++;
}

void messages_notify(messages_event_id_t event, uint32_t id)
{
    esp_event_post(MESSAGES_EVENT, event, &id, sizeof(id), pdMS_TO_TICKS(10));
}

const char *messages_alert_label(alert_kind_t kind)
{
    return kind < ALERT_COUNT ? s_alert_labels[kind] : "?";
}

bool messages_is_visible(const message_t *m)
{
    const mesh_node_id_t self = mesh_self_id();
    return m->dest == MESSAGES_TO_ALL || m->dest == self || m->origin == self;
}

bool messages_in_transit(const message_t *m)
{
    return !messages_is_visible(m);
}

size_t messages_list(message_t *out, size_t max, uint32_t after_id)
{
    return store_list(out, max, after_id, messages_is_visible);
}

size_t messages_transit_count(void)
{
    return store_count(messages_in_transit);
}

bool messages_get(uint32_t id, message_t *out)
{
    return store_get(id, out);
}
