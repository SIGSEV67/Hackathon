#pragma once

#include "buttons.h"
#include "esp_err.h"

// Botones de la placa -> acciones de la aplicacion:
//   KEY1 / KEY2 / KEY3  mantener 1 s = alerta SOS / MEDICO / PELIGRO (los tres de abajo)
//   KEY4                mantener = grabar voz (push-to-talk), soltar = enviar (el apartado)
// El joystick lo usa la UI para navegar.
esp_err_t controls_init(void);

// Etiqueta corta (3 letras) de lo que hace la tecla, para dibujarla en pantalla.
const char *controls_key_label(button_id_t key);
