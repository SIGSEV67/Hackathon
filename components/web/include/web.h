#pragma once

#include "esp_err.h"

// Portal para telefonos: DNS cautivo (cualquier dominio resuelve al nodo, asi el
// telefono abre la pagina solo al conectarse) + servidor HTTP con la app web y
// una API JSON sobre presence y messages.
//
// Requiere el WiFi AP arriba (radio_init).
esp_err_t web_init(void);
