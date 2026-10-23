#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "mesh.h"

// Mensajes que viajan por la mesh: alertas rapidas, texto y voz. Todo lo que
// llega (y lo propio) queda en un store local para mostrarlo en el display o
// servirlo al telefono.
//
// Texto y voz pueden ser privados (dest != MESSAGES_TO_ALL). Un privado viaja
// igual que los demas, pero solo lo muestran el destino y quien lo mando; los
// otros nodos lo guardan en custodia y se lo reenvian al destino cuando lo
// ven aparecer en la red (store-and-forward). El destino confirma con un ACK.
// Ojo: privado = dirigido, no cifrado.

#define MESSAGES_TEXT_MAX            160
#define MESSAGES_VOICE_SAMPLE_RATE   8000
#define MESSAGES_VOICE_BLOCK_SAMPLES 440  // 55 ms: un paquete de la mesh
#define MESSAGES_TO_ALL              0

typedef enum {
    MESSAGE_ALERT,
    MESSAGE_TEXT,
    MESSAGE_VOICE,
} message_kind_t;

typedef enum {
    ALERT_SOS,
    ALERT_MEDIC,
    ALERT_DANGER,
    ALERT_SAFE,
    ALERT_COUNT,
} alert_kind_t;

typedef struct {
    uint32_t id;             // id local, creciente: sirve para paginar desde el telefono
    message_kind_t kind;
    mesh_node_id_t origin;
    uint16_t ref;            // identifica el mensaje dentro de su origen, igual en todos los nodos
    mesh_node_id_t dest;     // MESSAGES_TO_ALL o el nodo destino de un privado
    bool delivered;          // privado: el destino confirmo que le llego
    uint8_t hops;            // 0 = vecino directo; MESSAGE_HOPS_SELF = propio
    uint32_t received_ms;    // mesh_now_ms() local
    alert_kind_t alert;      // MESSAGE_ALERT
    char text[MESSAGES_TEXT_MAX + 1];  // MESSAGE_TEXT
    uint16_t blocks;   // MESSAGE_VOICE: bloques totales
    uint16_t blocks_received; //   bloques que llegaron
} message_t;

#define MESSAGE_HOPS_SELF 0xff

ESP_EVENT_DECLARE_BASE(MESSAGES_EVENT);
typedef enum {
    MESSAGES_EVENT_NEW,         // data: uint32_t id. Mensaje nuevo visible (propio o recibido)
    MESSAGES_EVENT_UPDATED,     // data: uint32_t id. Ej. termino de llegar una voz
    MESSAGES_EVENT_DELIVERED,   // data: uint32_t id. El destino de un privado confirmo
    MESSAGES_EVENT_REC_STARTED,
    MESSAGES_EVENT_REC_STOPPED, // data: uint32_t id del mensaje grabado (o 0 si fue muy corto)
    MESSAGES_EVENT_PLAY_STOPPED,// data: uint32_t id. Termino (o se corto) una reproduccion
} messages_event_id_t;

// Requiere mesh_init, presence_init y audio_init.
esp_err_t messages_init(void);

const char *messages_alert_label(alert_kind_t kind);

// Si el mensaje se muestra en este nodo: es para todos, para nosotros o nuestro.
bool messages_is_visible(const message_t *m);
// Privado ajeno que este nodo guarda para reenviarlo.
bool messages_in_transit(const message_t *m);

esp_err_t messages_send_alert(alert_kind_t kind);
esp_err_t messages_send_text(mesh_node_id_t dest, const char *text);

// Grabacion push-to-talk: start al apretar, stop al soltar. Al parar se manda
// por la mesh en segundo plano. Corta sola a CONFIG_SIGSEV_VOICE_MAX_S.
esp_err_t messages_voice_start(mesh_node_id_t dest);
esp_err_t messages_voice_stop(void);
bool messages_voice_recording(uint32_t *elapsed_ms);

// Mensajes visibles, del mas nuevo al mas viejo, solo los de id > after_id.
size_t messages_list(message_t *out, size_t max, uint32_t after_id);
bool messages_get(uint32_t id, message_t *out);
// Cuantos privados ajenos tenemos en custodia.
size_t messages_transit_count(void);

// Voz que llega ya grabada (ej. subida desde el celular): PCM a
// MESSAGES_VOICE_SAMPLE_RATE, de a MESSAGES_VOICE_BLOCK_SAMPLES muestras. Al
// terminar se manda por la mesh en segundo plano, como una grabacion. Una a la vez.
esp_err_t messages_voice_import_begin(mesh_node_id_t dest, uint32_t samples, uint32_t *out_id);
esp_err_t messages_voice_import_block(uint32_t id, const int16_t *pcm);
esp_err_t messages_voice_import_end(uint32_t id, bool ok);  // ok = false descarta

// Reproduccion en el parlante de la placa (ESP_ERR_NOT_SUPPORTED sin parlante).
// Pedir otra voz corta la que suena. Grabar tambien la corta.
esp_err_t messages_voice_play(uint32_t id);
void messages_voice_play_stop(void);
bool messages_voice_playing(uint32_t *id, uint32_t *pos_ms);

// Decodifica un bloque de voz a PCM (MESSAGES_VOICE_BLOCK_SAMPLES muestras a
// MESSAGES_VOICE_SAMPLE_RATE), normalizado. Bloques perdidos salen en silencio.
esp_err_t messages_voice_decode(uint32_t id, uint16_t block, int16_t *out);
