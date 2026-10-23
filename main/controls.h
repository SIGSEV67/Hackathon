#pragma once

#include "esp_err.h"

// Botones de la placa -> acciones de la aplicacion:
//   KEY1             mantener = grabar voz (push-to-talk), soltar = enviar
//   KEY2 / KEY3 / KEY4  mantener 1 s = alerta SOS / MEDICO / PELIGRO
// El joystick lo usa la UI para navegar.
esp_err_t controls_init(void);
