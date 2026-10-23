#pragma once

#include "mesh.h"

// Vecinos directos (a un salto de radio), con su RSSI. Thread-safe.

esp_err_t neighbors_init(void);

void neighbors_heard(mesh_node_id_t id, const uint8_t addr[6], int8_t rssi);

size_t neighbors_copy(mesh_neighbor_t *out, size_t max);
