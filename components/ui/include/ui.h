#pragma once

#include "esp_err.h"

// Interfaz en la placa. Con display: pantallas en el OLED (Inicio, Mensajes,
// Red) que se recorren con el joystick, y avisos encima (grabando, alerta
// recibida, enviado). Sin display (perfil psram): un resumen periodico por log.
//
// La UI solo muestra: que hace cada boton lo decide la aplicacion.
esp_err_t ui_init(void);
