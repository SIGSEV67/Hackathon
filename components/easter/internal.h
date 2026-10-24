#pragma once

#include "esp_err.h"

// Detector del codigo en los eventos de botones (konami.c).
esp_err_t konami_init(void (*on_code)(void));

// Comandos por el USB serie (serial.c). Hoy uno solo: "konami".
esp_err_t serial_cmd_init(void (*on_konami)(void));
