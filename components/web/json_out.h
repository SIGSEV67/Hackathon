#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

// Escritor de JSON en streaming sobre una respuesta HTTP chunked, con buffer
// chico: no arma el documento entero en RAM. Los errores quedan pegados en
// `err` y se reportan al final.

typedef struct {
    httpd_req_t *req;
    char buf[512];
    size_t len;
    esp_err_t err;
} json_out_t;

void json_begin(json_out_t *out, httpd_req_t *req);
void json_raw(json_out_t *out, const char *s);
void json_fmt(json_out_t *out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// String entre comillas, escapado.
void json_str(json_out_t *out, const char *s);
esp_err_t json_end(json_out_t *out);
