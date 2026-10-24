#pragma once

#include "esp_err.h"

// Easter egg. El codigo Konami en la placa (arriba arriba abajo abajo izquierda
// derecha izquierda derecha KEY2 KEY2), o "konami" por el USB serie
// (tools/konami.py), hace que el kiwi baile el "6 7" y explote. Al explotar
// avisa por la mesh: todas las placas hacen lo mismo y despues se reinician.
// Se apaga con CONFIG_SIGSEV_EASTER_EGG.

// Despues de la UI y la mesh.
esp_err_t easter_init(void);

// Arranca la animacion en esta placa; al explotar la propaga. No hace nada si
// ya esta corriendo.
void easter_trigger(void);
