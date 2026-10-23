#pragma once

#include <stdint.h>

#include "esp_err.h"

// DNS cautivo: responde cualquier consulta A con `ip` (network byte order). Asi
// el chequeo de conectividad del telefono cae en nuestro servidor HTTP y abre
// el portal.
esp_err_t dns_start(uint32_t ip);
