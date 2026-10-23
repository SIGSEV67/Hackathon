#pragma once

#include "esp_err.h"

// Levanta el WiFi como access point abierto en CONFIG_SIGSEV_RADIO_CHANNEL.
// El mismo radio lo usan los telefonos (AP) y la mesh (ESP-NOW sobre la
// interfaz AP), por eso no hay modo station: conectarse a un router cambiaria
// el canal y partiria la mesh. Con CONFIG_SIGSEV_RADIO_FTM la station existe
// pero nunca se conecta: solo inicia mediciones de distancia (FTM).
//
// Requiere NVS y el loop por defecto de esp_event inicializados.
esp_err_t radio_init(const char *ssid);
