#pragma once

#include "esp_err.h"

// Interfaz en la placa. Con display: pantallas en el OLED (Inicio, Mensajes,
// Red) que se recorren con el joystick, y avisos encima (grabando, alerta
// recibida, enviado). Sin display (perfil psram): un resumen periodico por log.
//
// La UI solo muestra: que hace cada boton lo decide la aplicacion, que le pasa
// las etiquetas para dibujarlas.
typedef struct {
    const char *key_labels[4];  // KEY1..KEY4, ej. "SOS" (3 letras entran en el dibujo)
} ui_config_t;

esp_err_t ui_init(const ui_config_t *cfg);
