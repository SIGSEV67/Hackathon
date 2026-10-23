#pragma once

#include <stdint.h>

#include "esp_err.h"

// Medidor de ruido ambiente sobre el mic. Escala 0-100 (~0-60 dB sobre el piso
// de ruido del mic, no son dB SPL calibrados).

esp_err_t noise_init(void);

// Nivel suavizado: sube rapido y baja lento.
uint8_t noise_level(void);

// Pico desde la ultima llamada a noise_take_peak(); lo resetea al nivel actual.
uint8_t noise_take_peak(void);
