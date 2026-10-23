#include "audio.h"
#include "buttons.h"
#include "call.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "messages.h"
#include "presence.h"
#include "ui.h"

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
    if (messages_send_alert(mesh_self_id(), alert) != ESP_OK) {
        ESP_LOGE(TAG, "could not send %s", messages_alert_label(alert));
    }
}

// KEY4: sobre una voz en Mensajes la reproduce (o la corta); si no, graba
// mientras se mantiene (en Red, privada al nodo marcado).
static void on_voice_key(bool pressed)
{
    static bool recording;
    call_info_t call;
    call_get(&call);
    if (call.state == CALL_ACTIVE) {
        call_talk(pressed);  // en llamada, KEY4 es la tecla para hablar
        return;
    }
    if (!pressed) {
        if (recording) {
            messages_voice_stop();
            recording = false;
        }
        return;
    }
    message_t msg;
    const uint32_t picked = ui_selected_message();
    if (picked && audio_has_speaker() && messages_get(picked, &msg) && msg.kind == MESSAGE_VOICE) {
        uint32_t playing;
        if (messages_voice_playing(&playing, NULL) && playing == picked) {
            messages_voice_play_stop();
        } else {
            messages_voice_play(picked);
        }
        return;
    }
    recording = messages_voice_start(ui_selected_node()) == ESP_OK;
}

// Toque corto (soltar antes de que salte la alerta) de KEY1/KEY2: llamadas.
// KEY1 llama al nodo marcado en Red, atiende o corta; KEY2 rechaza.
static void on_tap(button_id_t btn)
{
    call_info_t call;
    call_get(&call);
    if (btn == BTN_KEY2 && call.state == CALL_INCOMING) {
        call_hangup();
    } else if (btn != BTN_KEY1) {
        return;
    } else if (call.state == CALL_INCOMING) {
        messages_voice_play_stop();
        call_answer();
    } else if (call.state != CALL_IDLE) {
        call_hangup();
    } else if (ui_selected_node() != MESSAGES_TO_ALL && !presence_is_client(ui_selected_node())) {
        // A un celu no se lo llama en vivo: se le manda voz (KEY4).
        messages_voice_play_stop();
        call_dial(ui_selected_node());
    }
}

static void on_button(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    const button_id_t btn = *(const button_id_t *)data;
    const bool pressed = event == BUTTONS_EVENT_PRESSED;

    if (btn == PTT_KEY) {
        on_voice_key(pressed);
        return;
    }
    for (size_t i = 0; i < ALERT_KEYS; i++) {
        if (s_alert_keys[i].key == btn) {
            const bool tap = !pressed && esp_timer_is_active(s_hold_timers[i]);
            esp_timer_stop(s_hold_timers[i]);  // no importa si no estaba corriendo
            if (pressed) {
                esp_timer_start_once(s_hold_timers[i], ALERT_HOLD_US);
            } else if (tap) {
                on_tap(btn);
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
