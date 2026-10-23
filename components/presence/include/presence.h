#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "mesh.h"

// Quien esta en la red. Cada nodo manda un beacon periodico (por toda la mesh)
// con su nombre, capacidades, nivel de ruido y vecinos directos; con eso cada
// nodo arma el directorio y la topologia completa (el "mapa de ruido").

#define PRESENCE_NAME_MAX  20
#define PRESENCE_MAX_LINKS 12

typedef enum {
    PRESENCE_FLAG_DISPLAY = 1 << 0,
    PRESENCE_FLAG_PSRAM = 1 << 1,
} presence_flags_t;

typedef struct {
    mesh_node_id_t id;
    int8_t rssi;
    uint16_t dist_dm;  // distancia estimada en decimetros (0 = no se sabe)
    bool dist_ftm;     // medida por tiempo de vuelo (si no, estimada por RSSI)
} presence_link_t;

typedef struct {
    mesh_node_id_t id;
    char name[PRESENCE_NAME_MAX + 1];
    uint8_t flags;       // presence_flags_t
    uint8_t noise;       // 0-100
    uint8_t noise_peak;  // pico desde el beacon anterior
    uint8_t hops;        // 0 = vecino directo
    uint32_t uptime_s;
    uint32_t age_ms;     // hace cuanto llego su ultimo beacon
    uint8_t n_links;
    presence_link_t links[PRESENCE_MAX_LINKS];
} presence_node_t;

// De donde sale lo que anuncia este nodo. Se inyecta para que presence no
// dependa del mic ni del hardware.
typedef struct {
    uint8_t flags;
    uint8_t (*noise_level)(void);      // opcional
    uint8_t (*noise_take_peak)(void);  // opcional
    // Distancia a un vecino directo (opcional): false si no se sabe.
    bool (*link_distance)(mesh_node_id_t id, uint16_t *dist_dm, bool *ftm);
} presence_config_t;

// event_data: mesh_node_id_t del nodo. NODE_JOINED sale cuando un nodo aparece
// (nuevo, de vuelta despues de caerse, o del historial por primera vez).
ESP_EVENT_DECLARE_BASE(PRESENCE_EVENT);
typedef enum {
    PRESENCE_EVENT_NODE_JOINED,
} presence_event_id_t;

// Requiere NVS y mesh_init. Los beacons empiezan a salir cuando arranca la mesh.
esp_err_t presence_init(const presence_config_t *cfg);

void presence_self_name(char *out, size_t len);

// Cambia el nombre de este nodo (ej. "Puesto medico") y lo guarda en NVS.
esp_err_t presence_set_name(const char *name);

// Devuelve el nombre anunciado por el nodo, o el nombre por defecto si todavia
// no recibimos su beacon.
void presence_name_of(mesh_node_id_t id, char *out, size_t len);

// Snapshot del nodo propio (como lo anunciaria el proximo beacon).
void presence_self(presence_node_t *out);

// Nodos remotos vivos. Devuelve cuantos copio.
size_t presence_nodes(presence_node_t *out, size_t max);

// Historial: todos los nodos que conocemos (vivos o no), con su ultimo nombre.
// Se guarda en NVS, asi que sobrevive a un reinicio.
typedef struct {
    mesh_node_id_t id;
    char name[PRESENCE_NAME_MAX + 1];
    bool alive;      // escuchamos su beacon hace poco
    bool seen;       // lo escuchamos en este arranque (si no, solo esta en el historial)
    uint32_t age_ms; // hace cuanto (si seen)
} presence_known_t;

#define PRESENCE_MAX_KNOWN 32

size_t presence_known(presence_known_t *out, size_t max);

bool presence_is_alive(mesh_node_id_t id);
