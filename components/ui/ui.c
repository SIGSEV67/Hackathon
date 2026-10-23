#include "board.h"
#include "buttons.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "presence.h"

#include "ui.h"
#include "ui_internal.h"

static const char *TAG = "ui";

#define FRAME_MS        100
#define STATUS_LOG_MS   10000
#define ALERT_TOAST_MS  15000
#define SENT_TOAST_MS   2000
#define INCOMING_TOAST_MS 4000

// La vista la modifican los handlers de eventos (task del event loop) y la lee
// el task de dibujo: se copia bajo el lock y se dibuja la copia.
static ui_view_t s_view;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

const char *g_key_labels[4];

static void show_toast(toast_kind_t kind, const message_t *msg, uint32_t duration_ms)
{
    taskENTER_CRITICAL(&s_lock);
    // Una alerta no se tapa con avisos menores.
    if (s_view.toast != TOAST_ALERT || kind == TOAST_ALERT) {
        s_view.toast = kind;
        s_view.toast_msg = *msg;
        s_view.toast_until_ms = mesh_now_ms() + duration_ms;
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void on_message(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    const uint32_t id = *(const uint32_t *)data;
    message_t msg;
    if (!id || !messages_get(id, &msg)) {
        return;
    }
    const bool own = msg.hops == MESSAGE_HOPS_SELF;

    if (event == MESSAGES_EVENT_NEW) {
        if (own) {
            show_toast(TOAST_SENT, &msg, SENT_TOAST_MS);
        } else if (msg.kind == MESSAGE_ALERT) {
            show_toast(TOAST_ALERT, &msg, ALERT_TOAST_MS);
        } else if (msg.kind == MESSAGE_TEXT) {
            show_toast(TOAST_INCOMING, &msg, INCOMING_TOAST_MS);
        }
    } else if (event == MESSAGES_EVENT_UPDATED && msg.kind == MESSAGE_VOICE && !own) {
        show_toast(TOAST_INCOMING, &msg, INCOMING_TOAST_MS);  // la voz se avisa cuando termino de llegar
    } else if (event == MESSAGES_EVENT_DELIVERED && own) {
        show_toast(TOAST_DELIVERED, &msg, INCOMING_TOAST_MS);
    }
}

static void on_button(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    if (event != BUTTONS_EVENT_PRESSED) {
        return;
    }
    const button_id_t btn = *(const button_id_t *)data;
    if (btn != BTN_UP && btn != BTN_DOWN && btn != BTN_LEFT && btn != BTN_RIGHT) {
        return;  // KEY1-4 son de la aplicacion
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_view.toast == TOAST_ALERT) {
        s_view.toast = TOAST_NONE;  // el joystick primero cierra la alerta
    } else if (btn == BTN_LEFT || btn == BTN_RIGHT) {
        const int step = btn == BTN_RIGHT ? 1 : SCREEN_COUNT - 1;
        s_view.screen = (s_view.screen + step) % SCREEN_COUNT;
        s_view.scroll = 0;
    } else {
        s_view.scroll += btn == BTN_DOWN ? 1 : -1;  // el task de dibujo lo acota
    }
    taskEXIT_CRITICAL(&s_lock);
}

static void draw_task(void *arg)
{
    while (1) {
        taskENTER_CRITICAL(&s_lock);
        if (s_view.toast != TOAST_NONE && (int32_t)(mesh_now_ms() - s_view.toast_until_ms) > 0) {
            s_view.toast = TOAST_NONE;
        }
        ui_view_t view = s_view;
        taskEXIT_CRITICAL(&s_lock);

        int max_scroll = screens_rows(view.screen) - 1;
        if (max_scroll < 0) {
            max_scroll = 0;
        }
        const int clamped = view.scroll < 0 ? 0 : (view.scroll > max_scroll ? max_scroll : view.scroll);
        if (clamped != view.scroll) {
            view.scroll = clamped;
            taskENTER_CRITICAL(&s_lock);
            s_view.scroll = clamped;
            taskEXIT_CRITICAL(&s_lock);
        }

        const ui_pick_t pick = screens_draw(&view);
        taskENTER_CRITICAL(&s_lock);
        s_view.selected = pick.node;
        s_view.selected_msg = pick.message;
        taskEXIT_CRITICAL(&s_lock);
        display_flush();
        vTaskDelay(pdMS_TO_TICKS(FRAME_MS));
    }
}

// Sin display: un resumen periodico para seguir la red por el monitor serie.
static void status_log_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_LOG_MS));
        presence_node_t self;
        presence_self(&self);
        static presence_node_t nodes[24];
        const size_t n = presence_nodes(nodes, sizeof(nodes) / sizeof(nodes[0]));
        ESP_LOGI(TAG, "%s: %u nodes in mesh, %u direct neighbors, noise %u", self.name, (unsigned)n + 1,
                 self.n_links, self.noise);
    }
}

mesh_node_id_t ui_selected_node(void)
{
    taskENTER_CRITICAL(&s_lock);
    const mesh_node_id_t id = s_view.screen == SCREEN_NETWORK ? s_view.selected : MESSAGES_TO_ALL;
    taskEXIT_CRITICAL(&s_lock);
    return id;
}

uint32_t ui_selected_message(void)
{
    taskENTER_CRITICAL(&s_lock);
    const uint32_t id = s_view.screen == SCREEN_MESSAGES ? s_view.selected_msg : 0;
    taskEXIT_CRITICAL(&s_lock);
    return id;
}

esp_err_t ui_init(const ui_config_t *cfg)
{
    for (int i = 0; i < 4; i++) {
        g_key_labels[i] = cfg->key_labels[i] ? cfg->key_labels[i] : "";
    }
    if (!BOARD_HAS_DISPLAY) {
        ESP_RETURN_ON_FALSE(xTaskCreate(status_log_task, "ui_log", 3072, NULL, 2, NULL) == pdPASS, ESP_ERR_NO_MEM,
                            TAG, "task create failed");
        ESP_LOGI(TAG, "no display in this build: status goes to the log");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(display_init(), TAG, "display init failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(MESSAGES_EVENT, ESP_EVENT_ANY_ID, on_message, NULL), TAG,
                        "register messages failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(BUTTONS_EVENT, ESP_EVENT_ANY_ID, on_button, NULL), TAG,
                        "register buttons failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(draw_task, "ui", 4096, NULL, 3, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    return ESP_OK;
}
