#include "buttons.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "messages.h"

#include "controls.h"

static const char *TAG = "controls";

#define ALERT_HOLD_US (1000 * 1000)
#define PTT_KEY       BTN_KEY4

// Tecla -> alerta que dispara si se la mantiene ALERT_HOLD_US.
static const struct {
    button_id_t key;
    alert_kind_t alert;
    const char *label;
} s_alert_keys[] = {
    {BTN_KEY1, ALERT_SOS, "SOS"},
    {BTN_KEY2, ALERT_MEDIC, "MED"},
    {BTN_KEY3, ALERT_DANGER, "PEL"},
};

#define ALERT_KEYS (sizeof(s_alert_keys) / sizeof(s_alert_keys[0]))

static esp_timer_handle_t s_hold_timers[ALERT_KEYS];

// Se dispara si la tecla sigue apretada: un toque accidental no manda nada.
static void on_hold(void *arg)
{
    const alert_kind_t alert = (alert_kind_t)(uintptr_t)arg;
    if (messages_send_alert(alert) != ESP_OK) {
        ESP_LOGE(TAG, "could not send %s", messages_alert_label(alert));
    }
}

static void on_button(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    const button_id_t btn = *(const button_id_t *)data;
    const bool pressed = event == BUTTONS_EVENT_PRESSED;

    if (btn == PTT_KEY) {
        if (pressed) {
            messages_voice_start(MESSAGES_TO_ALL);
        } else {
            messages_voice_stop();
        }
        return;
    }
    for (size_t i = 0; i < ALERT_KEYS; i++) {
        if (s_alert_keys[i].key == btn) {
            esp_timer_stop(s_hold_timers[i]);  // no importa si no estaba corriendo
            if (pressed) {
                esp_timer_start_once(s_hold_timers[i], ALERT_HOLD_US);
            }
            return;
        }
    }
}

esp_err_t controls_init(void)
{
    for (size_t i = 0; i < ALERT_KEYS; i++) {
        const esp_timer_create_args_t args = {
            .callback = on_hold,
            .arg = (void *)(uintptr_t)s_alert_keys[i].alert,
            .name = "alert_hold",
        };
        ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_hold_timers[i]), TAG, "timer create failed");
    }
    return esp_event_handler_register(BUTTONS_EVENT, ESP_EVENT_ANY_ID, on_button, NULL);
}

const char *controls_key_label(button_id_t key)
{
    if (key == PTT_KEY) {
        return "VOZ";
    }
    for (size_t i = 0; i < ALERT_KEYS; i++) {
        if (s_alert_keys[i].key == key) {
            return s_alert_keys[i].label;
        }
    }
    return "";
}
