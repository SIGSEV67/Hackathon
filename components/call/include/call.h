#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "mesh.h"

// Llamadas entre placas, tipo walkie-talkie: una vez atendida, cada lado habla
// mientras mantiene la tecla y el otro lo escucha en vivo por el parlante. Un
// lado a la vez: con mic y parlante pegados, full-duplex acoplaria.
//
// La senalizacion y el audio viajan por la mesh como cualquier paquete (pueden
// dar varios saltos). El audio es el mismo ADPCM de 8 kHz que los mensajes de
// voz, en bloques de 55 ms que se decodifican solos.

typedef enum {
    CALL_IDLE,
    CALL_OUTGOING,  // llamando, esperando que atiendan
    CALL_INCOMING,  // nos estan llamando
    CALL_ACTIVE,
} call_state_t;

typedef enum {
    CALL_END_NONE,
    CALL_END_HANGUP,    // colgo alguno de los dos
    CALL_END_NO_ANSWER,
    CALL_END_BUSY,      // el otro estaba en otra llamada
    CALL_END_LOST,      // se dejo de escuchar al otro
} call_end_t;

typedef struct {
    call_state_t state;
    mesh_node_id_t peer;
    bool talking;         // estamos transmitiendo
    bool peer_talking;    // estamos recibiendo audio
    uint32_t since_ms;    // mesh_now_ms() del ultimo cambio de estado
    call_end_t last_end;  // por que termino la ultima llamada
} call_info_t;

ESP_EVENT_DECLARE_BASE(CALL_EVENT);
typedef enum {
    CALL_EVENT_CHANGED,  // data: call_info_t
} call_event_id_t;

// Requiere mesh_init y audio_init.
esp_err_t call_init(void);

esp_err_t call_dial(mesh_node_id_t peer);
esp_err_t call_answer(void);
esp_err_t call_hangup(void);  // cuelga, rechaza o cancela, segun el estado

// Push-to-talk durante una llamada activa.
esp_err_t call_talk(bool on);

void call_get(call_info_t *out);
