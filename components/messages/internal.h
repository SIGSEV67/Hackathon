#pragma once

#include "messages.h"

// Piezas internas del componente messages.

esp_err_t alerts_init(void);
esp_err_t voice_init(void);

// Publica un evento de MESSAGES_EVENT con el id del mensaje.
void messages_notify(messages_event_id_t event, uint32_t id);
