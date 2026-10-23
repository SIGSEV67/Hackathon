#include "esp_check.h"
#include "esp_random.h"
#include "sdkconfig.h"

#include "internal.h"
#include "presence.h"
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
    ESP_RETURN_ON_ERROR(sender_init(), TAG, "sender init failed");
    ESP_RETURN_ON_ERROR(files_init(), TAG, "files init failed");
    ESP_RETURN_ON_ERROR(repair_init(), TAG, "repair init failed");
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

bool messages_for_board(const message_t *m)
{
    return messages_for_client(m, mesh_self_id());
}

bool messages_for_client(const message_t *m, mesh_node_id_t client)
{
    return m->dest == MESSAGES_TO_ALL || m->dest == client || m->origin == client;
}

bool messages_dest_is_here(const message_t *m)
{
    return m->dest != MESSAGES_TO_ALL && (m->dest == mesh_self_id() || presence_is_local_client(m->dest));
}

bool messages_is_local(const message_t *m)
{
    return messages_for_board(m) || messages_dest_is_here(m) || presence_is_local_client(m->origin);
}

bool messages_in_transit(const message_t *m)
{
    return !messages_is_local(m);
}

bool messages_valid_sender(mesh_node_id_t from)
{
    return from == mesh_self_id() || presence_is_local_client(from);
}

static bool keep_for_client(const message_t *m, void *ctx)
{
    return messages_for_client(m, *(const mesh_node_id_t *)ctx);
}

size_t messages_list(message_t *out, size_t max, uint32_t after_id)
{
    return messages_list_for(mesh_self_id(), out, max, after_id);
}

size_t messages_list_for(mesh_node_id_t client, message_t *out, size_t max, uint32_t after_id)
{
    return store_list(out, max, after_id, keep_for_client, &client);
}

size_t messages_transit_count(void)
{
    return store_count(messages_in_transit);
}

void messages_mark_read(uint32_t id)
{
    message_t m;
    if (store_get(id, &m) && m.dest != MESSAGES_TO_ALL && !m.read && store_set_read(id)) {
        courier_read(&m);
    }
}

bool messages_get(uint32_t id, message_t *out)
{
    return store_get(id, out);
}
