#include <stdbool.h>
#include <string.h>

#include "buttons.h"
#include "esp_check.h"
#include "esp_log.h"
#include "mesh.h"

#include "internal.h"

// Mira los botones que se aprietan y avisa cuando los ultimos forman el codigo
// Konami. Se guardan las ultimas entradas y se comparan enteras con el codigo:
// asi "arriba arriba arriba abajo..." tambien vale (un automata que vuelve a
// cero al primer error lo perderia).

static const char *TAG = "konami";

static const button_id_t s_code[] = {
    BTN_UP, BTN_UP, BTN_DOWN, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_LEFT, BTN_RIGHT, BTN_KEY2, BTN_KEY2,
};

#define CODE_LEN    (sizeof(s_code) / sizeof(s_code[0]))
#define GAP_MS      3000  // una pausa mas larga empieza de cero
#define DIAGONAL_MS 60    // una diagonal llega como dos direcciones a la vez: vale la primera

static void (*s_on_code)(void);
static button_id_t s_last[CODE_LEN];  // las ultimas entradas, la mas nueva al final
static size_t s_count;
static uint32_t s_last_ms;
static uint32_t s_last_joy_ms;

static bool is_joystick(button_id_t b)
{
    return b == BTN_UP || b == BTN_DOWN || b == BTN_LEFT || b == BTN_RIGHT;
}

// Corre en el task de eventos.
static void on_button(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    const button_id_t btn = *(const button_id_t *)data;
    if (event != BUTTONS_EVENT_PRESSED) {
        return;
    }
    const uint32_t now = mesh_now_ms();
    if (now - s_last_ms > GAP_MS) {
        s_count = 0;
    }
    if (is_joystick(btn)) {
        if (s_last_joy_ms && now - s_last_joy_ms < DIAGONAL_MS) {
            return;
        }
        s_last_joy_ms = now;
    }
    s_last_ms = now;

    if (s_count == CODE_LEN) {
        memmove(s_last, s_last + 1, (CODE_LEN - 1) * sizeof(s_last[0]));
        s_count--;
    }
    s_last[s_count++] = btn;
    if (s_count == CODE_LEN && memcmp(s_last, s_code, sizeof(s_code)) == 0) {
        s_count = 0;
        ESP_LOGI(TAG, "konami code!");
        s_on_code();
    }
}

esp_err_t konami_init(void (*on_code)(void))
{
    s_on_code = on_code;
    return esp_event_handler_register(BUTTONS_EVENT, ESP_EVENT_ANY_ID, on_button, NULL);
}
