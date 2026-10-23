#pragma once

#include <stdbool.h>

#include "mesh.h"

// Recuerda los ultimos (origen, seq) vistos para reenviar cada paquete una sola
// vez. Solo lo usa el task de rx de la mesh: no es thread-safe.

void dedup_reset(void);

// true si el paquete es nuevo (y lo registra), false si ya lo habiamos visto.
bool dedup_check_and_add(mesh_node_id_t origin, uint16_t seq);
