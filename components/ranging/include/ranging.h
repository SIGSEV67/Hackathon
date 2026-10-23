#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "mesh.h"

// Distancia a los vecinos directos. Con FTM (802.11mc, tiempo de vuelo del
// WiFi) se mide al metro o dos en campo abierto; si el vecino no contesta FTM
// se estima por RSSI, que es mucho mas grueso (el doble o la mitad es normal).

typedef enum {
    RANGING_NONE,
    RANGING_RSSI,
    RANGING_FTM,
} ranging_method_t;

typedef struct {
    uint16_t dist_dm;  // decimetros
    ranging_method_t method;
} ranging_t;

// Requiere radio_init y mesh_start.
esp_err_t ranging_init(void);

// Distancia a un vecino directo. false si no es vecino.
bool ranging_get(mesh_node_id_t id, ranging_t *out);
