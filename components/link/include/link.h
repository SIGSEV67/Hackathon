#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Transporte de un salto: manda frames en broadcast a los vecinos que tengan
// alcance de radio. La mesh solo conoce esta interfaz, asi que se puede cambiar
// el backend (ej. I2C entre dos placas cableadas) sin tocarla.

#define LINK_ADDR_LEN 6

typedef struct {
    uint8_t src[LINK_ADDR_LEN];  // direccion del vecino que transmitio el frame
    int8_t rssi;                 // dBm
} link_rx_info_t;

// Se llama desde el contexto del driver: copiar el frame y volver rapido.
typedef void (*link_rx_cb_t)(const link_rx_info_t *info, const uint8_t *data, size_t len, void *ctx);

typedef struct link {
    const char *name;
    size_t mtu;
    esp_err_t (*start)(link_rx_cb_t on_rx, void *ctx);
    // Bloquea hasta que el frame salio al aire (o timeout). No es thread-safe:
    // llamarlo desde un solo task.
    esp_err_t (*broadcast)(const uint8_t *data, size_t len);
    // Direccion propia, la misma que ven los vecinos en link_rx_info_t.src.
    void (*self_addr)(uint8_t out[LINK_ADDR_LEN]);
} link_t;

// Backend ESP-NOW sobre la interfaz softAP. Requiere el WiFi arrancado (radio).
const link_t *link_espnow(void);
