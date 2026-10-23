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

#define MESSAGES_TEXT_MAX            160
#define MESSAGES_VOICE_SAMPLE_RATE   8000
#define MESSAGES_VOICE_BLOCK_SAMPLES 440  // 55 ms: un paquete de la mesh

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
    uint8_t hops;            // 0 = vecino directo; 0xff = propio
    uint32_t received_ms;    // mesh_now_ms() local
    alert_kind_t alert;      // MESSAGE_ALERT
    char text[MESSAGES_TEXT_MAX + 1];  // MESSAGE_TEXT
    uint16_t voice_blocks;   // MESSAGE_VOICE: bloques totales
    uint16_t voice_received; //   bloques que llegaron
} message_t;

#define MESSAGE_HOPS_SELF 0xff

ESP_EVENT_DECLARE_BASE(MESSAGES_EVENT);
typedef enum {
    MESSAGES_EVENT_NEW,         // data: uint32_t id. Mensaje nuevo (propio o recibido)
    MESSAGES_EVENT_UPDATED,     // data: uint32_t id. Ej. termino de llegar una voz
    MESSAGES_EVENT_REC_STARTED,
    MESSAGES_EVENT_REC_STOPPED, // data: uint32_t id del mensaje grabado (o 0 si fue muy corto)
} messages_event_id_t;

// Requiere mesh_init y audio_init.
esp_err_t messages_init(void);

const char *messages_alert_label(alert_kind_t kind);

esp_err_t messages_send_alert(alert_kind_t kind);
esp_err_t messages_send_text(const char *text);

// Grabacion push-to-talk: start al apretar, stop al soltar. Al parar se manda
// por la mesh en segundo plano. Corta sola a CONFIG_SIGSEV_VOICE_MAX_S.
esp_err_t messages_voice_start(void);
esp_err_t messages_voice_stop(void);
bool messages_voice_recording(uint32_t *elapsed_ms);

// Mensajes del mas nuevo al mas viejo, solo los de id > after_id.
size_t messages_list(message_t *out, size_t max, uint32_t after_id);
bool messages_get(uint32_t id, message_t *out);

// Decodifica un bloque de voz a PCM (MESSAGES_VOICE_BLOCK_SAMPLES muestras a
// MESSAGES_VOICE_SAMPLE_RATE), normalizado. Bloques perdidos salen en silencio.
esp_err_t messages_voice_decode(uint32_t id, uint16_t block, int16_t *out);
