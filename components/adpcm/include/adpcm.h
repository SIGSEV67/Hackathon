#pragma once

#include <stddef.h>
#include <stdint.h>

// IMA ADPCM: 4 bits por muestra (4:1 sobre PCM de 16 bits). C puro, sin
// dependencias de ESP-IDF, asi se puede testear en la PC.
//
// Cada bloque arranca guardando el estado, asi se decodifica solo: si se pierde
// un paquete de la mesh, se pierde ese pedacito y nada mas.

typedef struct {
    int16_t predictor;
    uint8_t index;
} adpcm_state_t;

// Codifica `count` muestras (par) en count/2 bytes. Nibble bajo primero.
void adpcm_encode(adpcm_state_t *state, const int16_t *in, size_t count, uint8_t *out);

// Decodifica `count` muestras (par) desde count/2 bytes.
void adpcm_decode(adpcm_state_t *state, const uint8_t *in, size_t count, int16_t *out);
