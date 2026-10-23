#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 16000

// Recibe bloques de PCM mono de 16 bits a AUDIO_SAMPLE_RATE_HZ. Corre en el task
// del mic: tiene que ser rapido y no bloquear.
typedef void (*audio_mic_sink_t)(const int16_t *samples, size_t count, void *ctx);

esp_err_t audio_init(void);

// Registra un consumidor del mic. Los sinks son permanentes; si un consumidor
// solo quiere audio a veces (ej. grabar), filtra adentro de su callback.
esp_err_t audio_mic_add_sink(audio_mic_sink_t fn, void *ctx);

// Sin parlante (CONFIG_HACK_SPEAKER_ENABLE=n) devuelve ESP_ERR_NOT_SUPPORTED.
esp_err_t audio_play_tone(int freq_hz, int duration_ms);
