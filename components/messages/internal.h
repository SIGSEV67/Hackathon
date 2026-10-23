#pragma once

#include "messages.h"

// Piezas internas del componente messages:
//   alerts.c   alertas y texto (envio, recepcion, reenvio de texto)
//   voice.c    grabacion, envio, recepcion y reproduccion de voz
//   courier.c  custodia de privados: reenvio al destino y ACKs
//   store.c    donde queda todo

esp_err_t alerts_init(void);
esp_err_t voice_init(void);
esp_err_t courier_init(void);

// Publica un evento de MESSAGES_EVENT con el id del mensaje.
void messages_notify(messages_event_id_t event, uint32_t id);

// Identidad nueva para un texto o voz propio (estable entre reenvios).
uint16_t messages_next_ref(void);

// Vuelven a mandar un mensaje guardado, con su identidad original. Para voz se
// mandan los bloques que tengamos (un custodio puede tener una copia parcial).
esp_err_t alerts_resend_text(const message_t *msg);
esp_err_t voice_resend(const message_t *msg);

// Avisos al courier:
//   llego un privado para nosotros (o un repetido): confirmar al origen
void courier_ack(const message_t *msg);
//   otro nodo reenvio un mensaje que tenemos: no hace falta que lo hagamos
void courier_heard(uint32_t id);
//   lo acabamos de mandar nosotros
void courier_sent(uint32_t id);
