#pragma once

#include "messages.h"
#include "store.h"

// Piezas internas del componente messages:
//   alerts.c   alertas y texto (envio, recepcion, reenvio de texto)
//   voice.c    grabacion, envio, recepcion y reproduccion de voz
//   courier.c  custodia de privados: reenvio al destino y ACKs
//   player.c   reproduccion en el parlante
//   sender.c   task que manda rangos de bloques de una voz
//   repair.c   pedido de bloques perdidos (NACK) y respuesta
//   store.c    donde queda todo

esp_err_t alerts_init(void);
esp_err_t voice_init(void);
esp_err_t courier_init(void);
esp_err_t player_init(void);
esp_err_t sender_init(void);
esp_err_t repair_init(void);

// Si el destino de un privado esta aca (la placa o uno de sus celus): este
// nodo es el que lo entrega y confirma.
bool messages_dest_is_here(const message_t *m);
// Si le interesa a alguien de aca (placa o celus): para completarlo si llega roto.
bool messages_is_local(const message_t *m);
// Si `from` puede mandar desde esta placa: la placa o un celu conectado.
bool messages_valid_sender(mesh_node_id_t from);

// Publica un evento de MESSAGES_EVENT con el id del mensaje.
void messages_notify(messages_event_id_t event, uint32_t id);

// Identidad nueva para un texto o voz propio (estable entre reenvios).
uint16_t messages_next_ref(void);

// Vuelven a mandar un mensaje guardado, con su identidad original. Para voz se
// mandan los bloques que tengamos (un custodio puede tener una copia parcial).
esp_err_t alerts_resend_text(const message_t *msg);
esp_err_t voice_resend(const message_t *msg);

// Un bloque de voz en el formato del aire.
esp_err_t voice_send_block(const message_t *msg, uint16_t index, const block_t *block);

// Encola el envio de los bloques [start, start + count) que tengamos de una
// voz. No bloquea: si la cola esta llena, se descarta (el que pidio vuelve a pedir).
void sender_post(uint32_t id, uint16_t start, uint16_t count);

// Avisos al courier:
//   llego un privado para nosotros (o un repetido): confirmar al origen
void courier_ack(const message_t *msg);
//   el celu destino lo vio
void courier_read(const message_t *msg);
//   otro nodo reenvio un mensaje que tenemos: no hace falta que lo hagamos
void courier_heard(uint32_t id);
//   lo acabamos de mandar nosotros
void courier_sent(uint32_t id);
