#pragma once

#include "messages.h"

// Store de mensajes: ring de metadatos + bloques de voz (en PSRAM si hay).
// Thread-safe. Privado del componente.

#define VOICE_BLOCK_BYTES (MESSAGES_VOICE_BLOCK_SAMPLES / 2)

typedef struct {
    int16_t predictor;
    uint8_t index;
    uint8_t present;
    uint8_t data[VOICE_BLOCK_BYTES];
} voice_block_t;

esp_err_t store_init(size_t voice_budget_bytes);

// Agrega una alerta o texto (msg->id se ignora). Pisa al mas viejo si esta lleno.
esp_err_t store_add(const message_t *msg, uint32_t *out_id);

// Agrega una voz con msg->voice_blocks bloques vacios. Si no hay memoria, libera
// las voces mas viejas. voice_ref identifica la voz dentro de su origen.
esp_err_t store_add_voice(const message_t *msg, uint16_t voice_ref, uint32_t *out_id);
bool store_find_voice(mesh_node_id_t origin, uint16_t voice_ref, uint32_t *out_id);

void store_remove(uint32_t id);

// Guarda un bloque. *completed queda en true si con este se completo el mensaje.
// Bloques repetidos se ignoran (ESP_ERR_INVALID_STATE).
esp_err_t store_voice_put(uint32_t id, uint16_t index, const voice_block_t *block, bool *completed);
esp_err_t store_voice_get(uint32_t id, uint16_t index, voice_block_t *out);

// Recorta una voz propia al largo que realmente se grabo.
esp_err_t store_voice_truncate(uint32_t id, uint16_t blocks);

// Ganancia de reproduccion cacheada (Q8, 0 = sin calcular).
uint16_t store_voice_gain(uint32_t id);
void store_voice_set_gain(uint32_t id, uint16_t gain_q8);

bool store_get(uint32_t id, message_t *out);
size_t store_list(message_t *out, size_t max, uint32_t after_id);
