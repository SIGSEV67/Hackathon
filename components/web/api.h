#pragma once

#include "esp_http_server.h"

// Endpoints JSON bajo /api/ (ver api.c).
esp_err_t api_register(httpd_handle_t server);
