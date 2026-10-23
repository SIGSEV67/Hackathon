#pragma once

#include "messages.h"

// Store de mensajes: ring de metadatos + bloques de voz (en PSRAM si hay).
// Thread-safe. Privado del componente.

#define BLOCK_BYTES  (MESSAGES_VOICE_BLOCK_SAMPLES / 2)
#define STORE_MAX_MESSAGES 48

typedef struct {
    int16_t predictor;
    uint8_t index;
    uint8_t present;
    uint8_t data[BLOCK_BYTES];
} block_t;

esp_err_t store_init(size_t voice_budget_bytes);

// Agrega una alerta o texto (msg->id se ignora). Pisa al mas viejo si esta lleno.
esp_err_t store_add(const message_t *msg, uint32_t *out_id);

// Agrega una voz o archivo con msg->blocks bloques vacios. Si no hay memoria,
// libera los mas viejos que tengan bloques.
esp_err_t store_add_blocks(const message_t *msg, uint32_t *out_id);

// Busca un texto o voz por su identidad en la red (origin, ref).
bool store_find(mesh_node_id_t origin, uint16_t ref, uint32_t *out_id);

// Marca un privado como entregado / leido (leido implica entregado). Devuelven
// true si cambio.
bool store_set_delivered(uint32_t id);
bool store_set_read(uint32_t id);

void store_remove(uint32_t id);

// Guarda un bloque. *completed queda en true si con este se completo el mensaje.
// Bloques repetidos se ignoran (ESP_ERR_INVALID_STATE).
esp_err_t store_block_put(uint32_t id, uint16_t index, const block_t *block, bool *completed);
esp_err_t store_block_get(uint32_t id, uint16_t index, block_t *out);

// Recorta una voz propia al largo que realmente se grabo.
esp_err_t store_blocks_truncate(uint32_t id, uint16_t blocks);

// Ganancia de reproduccion cacheada (Q8, 0 = sin calcular).
uint16_t store_voice_gain(uint32_t id);
void store_voice_set_gain(uint32_t id, uint16_t gain_q8);

// out puede ser NULL para solo preguntar si sigue.
bool store_get(uint32_t id, message_t *out);

// Del mas nuevo al mas viejo, con id > after_id y que pasen `keep` (NULL = todos).
size_t store_list(message_t *out, size_t max, uint32_t after_id, bool (*keep)(const message_t *, void *ctx),
                  void *ctx);
size_t store_count(bool (*keep)(const message_t *));

// Ids de los mensajes que pasan `keep`, del mas nuevo al mas viejo. Para
// recorrer el store sin copiar mensajes enteros.
size_t store_ids(uint32_t *out, size_t max, bool (*keep)(const message_t *));

typedef struct {
    uint16_t start;
    uint16_t count;
} block_range_t;

// Rangos de bloques que faltan de un mensaje. Devuelve cuantos rangos copio.
size_t store_missing(uint32_t id, block_range_t *out, size_t max);

// Actualiza los datos de un archivo (llegan aparte de los bloques).
bool store_set_file_info(uint32_t id, uint32_t size, const char *name, const char *mime);
