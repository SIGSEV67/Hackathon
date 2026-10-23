#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

typedef enum {
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_CENTER,
    BTN_KEY1,
    BTN_KEY2,
    BTN_KEY3,
    BTN_KEY4,
    BTN_COUNT,
} button_id_t;

// Eventos en el loop por defecto de esp_event. event_data es un button_id_t.
ESP_EVENT_DECLARE_BASE(BUTTONS_EVENT);

typedef enum {
    BUTTONS_EVENT_PRESSED,
    BUTTONS_EVENT_RELEASED,
} buttons_event_id_t;

// Requiere el loop por defecto creado (esp_event_loop_create_default).
esp_err_t buttons_init(void);

uint32_t buttons_read(void);

const char *buttons_name(button_id_t id);
