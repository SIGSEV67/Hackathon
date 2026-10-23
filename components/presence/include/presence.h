#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "mesh.h"

// Quien esta en la red. Cada nodo (placa) manda un beacon periodico (por toda
// la mesh) con su nombre, capacidades, nivel de ruido y vecinos directos; con
// eso cada nodo arma el directorio y la topologia completa (el "mapa de ruido").
//
// Ademas de las placas hay clientes: celulares (o compus) conectados al WiFi de
// una placa, que es su "padre". Cada placa anuncia sus clientes, asi toda la red
// sabe donde esta cada uno. Placas y clientes comparten el espacio de ids
// (mesh_node_id_t): a los dos se les puede mandar un mensaje.

#define PRESENCE_NAME_MAX  20
#define PRESENCE_MAX_LINKS 12

typedef enum {
    PRESENCE_KIND_NODE,    // una placa
    PRESENCE_KIND_CLIENT,  // un celu colgado de una placa
} presence_kind_t;

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

// event_data: mesh_node_id_t. NODE_JOINED sale cuando una placa o un cliente
// aparece (nuevo, de vuelta despues de caerse, del historial por primera vez, o
// un cliente que se paso a otra placa).
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

// Placas remotas vivas (sin clientes). Devuelve cuantas copio.
size_t presence_nodes(presence_node_t *out, size_t max);

// Un cliente le hablo a esta placa (la web lo llama en cada pedido): queda
// colgado de ella. ESP_ERR_INVALID_ARG si el id es de una placa.
esp_err_t presence_client_seen(mesh_node_id_t id, const char *name);

// Si el cliente esta conectado a esta placa ahora.
bool presence_is_local_client(mesh_node_id_t id);
// Si el id es de un cliente (conectado aca o en otra placa, vivo o no).
bool presence_is_client(mesh_node_id_t id);

// Historial: todas las placas y clientes que conocemos (vivos o no), con su
// ultimo nombre. Se guarda en NVS, asi que sobrevive a un reinicio.
typedef struct {
    mesh_node_id_t id;
    presence_kind_t kind;
    mesh_node_id_t parent;  // cliente: placa de la que cuelga (o colgaba)
    char name[PRESENCE_NAME_MAX + 1];
    bool alive;      // escuchamos su beacon hace poco
    bool seen;       // lo escuchamos en este arranque (si no, solo esta en el historial)
    uint32_t age_ms; // hace cuanto (si seen)
} presence_known_t;

#define PRESENCE_MAX_KNOWN 48

size_t presence_known(presence_known_t *out, size_t max);

bool presence_is_alive(mesh_node_id_t id);
