#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "json_out.h"

static void flush(json_out_t *out)
{
    if (out->len && out->err == ESP_OK) {
        out->err = httpd_resp_send_chunk(out->req, out->buf, out->len);
    }
    out->len = 0;
}

static void put(json_out_t *out, const char *s, size_t n)
{
    while (n) {
        if (out->len == sizeof(out->buf)) {
            flush(out);
        }
        const size_t room = sizeof(out->buf) - out->len;
        const size_t take = n < room ? n : room;
        memcpy(out->buf + out->len, s, take);
        out->len += take;
        s += take;
        n -= take;
    }
}

void json_begin(json_out_t *out, httpd_req_t *req)
{
    out->req = req;
    out->len = 0;
    out->err = ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

void json_raw(json_out_t *out, const char *s)
{
    put(out, s, strlen(s));
}

void json_fmt(json_out_t *out, const char *fmt, ...)
{
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) {
        put(out, tmp, (size_t)n < sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
    }
}

void json_str(json_out_t *out, const char *s)
{
    put(out, "\"", 1);
    for (; *s; s++) {
        const unsigned char c = *s;
        if (c == '"' || c == '\\') {
            const char esc[2] = {'\\', c};
            put(out, esc, 2);
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof(esc), "\\u%04x", c);
            put(out, esc, 6);
        } else {
            put(out, (const char *)&c, 1);
        }
    }
    put(out, "\"", 1);
}

esp_err_t json_end(json_out_t *out)
{
    flush(out);
    if (out->err == ESP_OK) {
        out->err = httpd_resp_send_chunk(out->req, NULL, 0);
    }
    return out->err;
}
