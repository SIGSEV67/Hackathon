#pragma once

#include "esp_err.h"
#include "mesh.h"

// Interfaz en la placa. Con display: pantallas en el OLED (Inicio, Mensajes,
// Red) que se recorren con el joystick (en Mensajes y Red, arriba/abajo mueve
// un cursor), y avisos encima (grabando, alerta
// recibida, enviado). Sin display (perfil psram): un resumen periodico por log.
//
// La UI solo muestra: que hace cada boton lo decide la aplicacion, que le pasa
// las etiquetas para dibujarlas.
typedef struct {
    const char *key_labels[4];  // KEY1..KEY4, ej. "SOS" (3 letras entran en el dibujo)
} ui_config_t;

esp_err_t ui_init(const ui_config_t *cfg);

// Destino elegido en la pantalla Red (el cursor del joystick), o
// MESSAGES_TO_ALL si no hay un nodo marcado o la pantalla es otra.
mesh_node_id_t ui_selected_node(void);

// Mensaje bajo el cursor en la pantalla Mensajes, o 0 si la pantalla es otra.
uint32_t ui_selected_message(void);
