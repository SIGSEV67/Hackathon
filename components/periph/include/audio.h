#pragma once

#include <stdbool.h>
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

// --- parlante ---
// Sin parlante (CONFIG_HACK_SPEAKER_ENABLE=n) todo devuelve ESP_ERR_NOT_SUPPORTED.
// Se puede llamar desde varios tasks: las escrituras se serializan.

bool audio_has_speaker(void);

// Manda PCM mono de 16 bits a AUDIO_SAMPLE_RATE_HZ al parlante, con el volumen
// de menuconfig aplicado. Bloquea hasta que entra en el buffer de DMA.
esp_err_t audio_speaker_write(const int16_t *samples, size_t count, uint32_t timeout_ms);

esp_err_t audio_play_tone(int freq_hz, int duration_ms);
