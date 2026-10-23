#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "link.h"

// Mesh peer-to-peer por flooding controlado: cada nodo reenvia una vez cada
// paquete que no vio antes, hasta agotar el TTL. No hay coordinador ni rutas:
// si un nodo se cae, los paquetes siguen por los demas caminos.
//
// La mesh no interpreta el payload. Cada modulo registra un handler para su
// tipo de paquete (mesh_register_handler).

typedef uint32_t mesh_node_id_t;  // ultimos 4 bytes de la direccion del link

// Registro central de tipos de paquete, para que no se pisen entre modulos.
typedef enum {
    MESH_TYPE_PRESENCE = 1,
    MESH_TYPE_ALERT = 2,
    MESH_TYPE_TEXT = 3,
    MESH_TYPE_VOICE = 4,
    MESH_TYPE_ACK = 5,
    MESH_TYPE_FILE_INFO = 6,
    MESH_TYPE_FILE = 7,
    MESH_TYPE_NACK = 8,
    MESH_TYPE_MAX,
} mesh_type_t;

#define MESH_HEADER_LEN   12
#define MESH_PAYLOAD_MAX  (250 - MESH_HEADER_LEN)

typedef struct {
    mesh_node_id_t origin;  // quien creo el paquete
    uint16_t seq;           // secuencia del origen: (origin, seq) identifica el paquete
    uint8_t hops;           // saltos hasta llegar aca (0 = lo mando un vecino directo)
    mesh_node_id_t via;     // vecino que nos lo entrego
    int8_t rssi;            // del ultimo salto
} mesh_rx_meta_t;

// Se llama desde el task de la mesh, una sola vez por paquete. No bloquear.
typedef void (*mesh_handler_t)(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx);

typedef struct {
    mesh_node_id_t id;
    int8_t rssi;           // promedio movil
    uint32_t last_seen_ms;
} mesh_neighbor_t;

typedef struct {
    uint32_t rx;        // paquetes nuevos recibidos
    uint32_t dup;       // duplicados descartados
    uint32_t tx;        // paquetes propios enviados
    uint32_t fwd;       // paquetes reenviados
    uint32_t dropped;   // descartados por colas llenas o errores de envio
} mesh_stats_t;

// Prepara la identidad del nodo, colas y tasks. No toca la radio: se puede
// llamar antes de levantar el WiFi (ej. para armar el SSID con el nombre).
esp_err_t mesh_init(const link_t *link);

// Arranca el link y empieza a recibir/reenviar. Requiere el link listo (para
// ESP-NOW, el WiFi arrancado).
esp_err_t mesh_start(void);

mesh_node_id_t mesh_self_id(void);

// Nombre por defecto de un nodo, ej. "SIGSEV-36D8".
void mesh_node_name(mesh_node_id_t id, char *out, size_t len);

// Registrar antes de que lleguen paquetes (en el init de cada modulo).
esp_err_t mesh_register_handler(mesh_type_t type, mesh_handler_t fn, void *ctx);

// Manda un paquete propio a toda la mesh. Espera hasta `wait` si la cola de
// salida esta llena (util para mandar rafagas, ej. un mensaje de voz).
// Antes de mesh_start devuelve ESP_ERR_INVALID_STATE.
esp_err_t mesh_send(mesh_type_t type, const void *payload, size_t len, TickType_t wait);

// Copia los vecinos directos vivos a `out`. Devuelve cuantos copio.
size_t mesh_neighbors(mesh_neighbor_t *out, size_t max);

void mesh_get_stats(mesh_stats_t *out);

uint32_t mesh_now_ms(void);
