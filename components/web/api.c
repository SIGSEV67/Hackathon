// API JSON para la app web:
//   GET  /api/state             nodo propio, nodos de la red, estadisticas
//   GET  /api/messages?after=N  mensajes con id > N, del mas nuevo al mas viejo
//   GET  /api/voice?id=N        mensaje de voz como WAV (PCM 16 bits, 8 kHz)
//   POST /api/alert?kind=N      manda una alerta (alert_kind_t)
//   POST /api/text              manda el body como texto
//   POST /api/name              renombra este nodo con el body

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "api.h"
#include "json_out.h"
#include "messages.h"
#include "presence.h"

static const char *TAG = "api";

#define MAX_LIST 48

// --- helpers ---------------------------------------------------------------------

static esp_err_t reply_error(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[96];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return httpd_resp_sendstr(req, body);
}

static esp_err_t reply_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static bool query_int(httpd_req_t *req, const char *key, long *out)
{
    char query[64], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    char *end;
    *out = strtol(val, &end, 10);
    return end != val && *end == '\0';
}

// Lee el body como string (sin espacios al principio ni al final).
static int read_body(httpd_req_t *req, char *buf, size_t size)
{
    if (req->content_len == 0 || req->content_len >= size) {
        return -1;
    }
    size_t got = 0;
    while (got < req->content_len) {
        const int n = httpd_req_recv(req, buf + got, req->content_len - got);
        if (n <= 0) {
            return -1;
        }
        got += n;
    }
    buf[got] = '\0';
    while (got && (buf[got - 1] == ' ' || buf[got - 1] == '\n' || buf[got - 1] == '\r')) {
        buf[--got] = '\0';
    }
    char *start = buf;
    while (*start == ' ') {
        start++;
    }
    memmove(buf, start, strlen(start) + 1);
    return strlen(buf);
}

static void json_id(json_out_t *out, mesh_node_id_t id)
{
    json_fmt(out, "\"%08" PRIx32 "\"", id);
}

static void json_node(json_out_t *out, const presence_node_t *n)
{
    json_raw(out, "{\"id\":");
    json_id(out, n->id);
    json_raw(out, ",\"name\":");
    json_str(out, n->name);
    json_fmt(out, ",\"flags\":%u,\"noise\":%u,\"peak\":%u,\"hops\":%u,\"age\":%" PRIu32 ",\"up\":%" PRIu32,
             n->flags, n->noise, n->noise_peak, n->hops, n->age_ms, n->uptime_s);
    json_raw(out, ",\"links\":[");
    for (int i = 0; i < n->n_links; i++) {
        json_raw(out, i ? ",{\"id\":" : "{\"id\":");
        json_id(out, n->links[i].id);
        json_fmt(out, ",\"rssi\":%d}", n->links[i].rssi);
    }
    json_raw(out, "]}");
}

// --- GET -------------------------------------------------------------------------

static esp_err_t get_state(httpd_req_t *req)
{
    presence_node_t *nodes = malloc(sizeof(presence_node_t) * (MAX_LIST + 1));
    if (!nodes) {
        return reply_error(req, "503 Service Unavailable", "no memory");
    }
    presence_self(&nodes[0]);
    const size_t count = presence_nodes(&nodes[1], MAX_LIST);

    mesh_stats_t stats;
    mesh_get_stats(&stats);
    uint32_t rec_ms;
    const bool recording = messages_voice_recording(&rec_ms);

    json_out_t out;
    json_begin(&out, req);
    json_fmt(&out, "{\"now\":%" PRIu32 ",\"self\":", mesh_now_ms());
    json_node(&out, &nodes[0]);
    json_raw(&out, ",\"nodes\":[");
    for (size_t i = 0; i < count; i++) {
        if (i) {
            json_raw(&out, ",");
        }
        json_node(&out, &nodes[1 + i]);
    }
    json_fmt(&out,
             "],\"stats\":{\"rx\":%" PRIu32 ",\"tx\":%" PRIu32 ",\"fwd\":%" PRIu32 ",\"dup\":%" PRIu32
             ",\"dropped\":%" PRIu32 "}",
             stats.rx, stats.tx, stats.fwd, stats.dup, stats.dropped);
    json_fmt(&out, ",\"rec\":{\"active\":%s,\"ms\":%" PRIu32 "}}", recording ? "true" : "false", rec_ms);
    free(nodes);
    return json_end(&out);
}

static const char *kind_name(message_kind_t kind)
{
    switch (kind) {
    case MESSAGE_ALERT:
        return "alert";
    case MESSAGE_TEXT:
        return "text";
    case MESSAGE_VOICE:
        return "voice";
    }
    return "?";
}

static esp_err_t get_messages(httpd_req_t *req)
{
    long after = 0;
    query_int(req, "after", &after);

    message_t *msgs = malloc(sizeof(message_t) * MAX_LIST);
    if (!msgs) {
        return reply_error(req, "503 Service Unavailable", "no memory");
    }
    const size_t count = messages_list(msgs, MAX_LIST, after > 0 ? after : 0);
    const uint32_t now = mesh_now_ms();

    json_out_t out;
    json_begin(&out, req);
    json_raw(&out, "{\"messages\":[");
    for (size_t i = 0; i < count; i++) {
        const message_t *m = &msgs[i];
        char from[PRESENCE_NAME_MAX + 1];
        presence_name_of(m->origin, from, sizeof(from));

        json_fmt(&out, "%s{\"id\":%" PRIu32 ",\"kind\":\"%s\",\"origin\":", i ? "," : "", m->id, kind_name(m->kind));
        json_id(&out, m->origin);
        json_raw(&out, ",\"from\":");
        json_str(&out, from);
        json_fmt(&out, ",\"hops\":%d,\"age\":%" PRIu32, m->hops == MESSAGE_HOPS_SELF ? -1 : m->hops,
                 (now - m->received_ms) / 1000);
        switch (m->kind) {
        case MESSAGE_ALERT:
            json_fmt(&out, ",\"alert\":%d,\"label\":", m->alert);
            json_str(&out, messages_alert_label(m->alert));
            break;
        case MESSAGE_TEXT:
            json_raw(&out, ",\"text\":");
            json_str(&out, m->text);
            break;
        case MESSAGE_VOICE:
            json_fmt(&out, ",\"blocks\":%u,\"got\":%u,\"ms\":%u", m->voice_blocks, m->voice_received,
                     m->voice_blocks * MESSAGES_VOICE_BLOCK_SAMPLES * 1000 / MESSAGES_VOICE_SAMPLE_RATE);
            break;
        }
        json_raw(&out, "}");
    }
    json_raw(&out, "]}");
    free(msgs);
    return json_end(&out);
}

#define FOURCC __attribute__((nonstring))

typedef struct __attribute__((packed)) {
    char riff[4] FOURCC;
    uint32_t riff_size;
    char wave[4] FOURCC;
    char fmt[4] FOURCC;
    uint32_t fmt_size;
    uint16_t format;
    uint16_t channels;
    uint32_t rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
    char data[4] FOURCC;
    uint32_t data_size;
} wav_header_t;

static esp_err_t get_voice(httpd_req_t *req)
{
    long id;
    message_t msg;
    if (!query_int(req, "id", &id) || !messages_get(id, &msg) || msg.kind != MESSAGE_VOICE) {
        return reply_error(req, "404 Not Found", "no such voice message");
    }

    const uint32_t samples = (uint32_t)msg.voice_blocks * MESSAGES_VOICE_BLOCK_SAMPLES;
    const wav_header_t h = {
        .riff = "RIFF",
        .riff_size = 36 + samples * 2,
        .wave = "WAVE",
        .fmt = "fmt ",
        .fmt_size = 16,
        .format = 1,
        .channels = 1,
        .rate = MESSAGES_VOICE_SAMPLE_RATE,
        .byte_rate = MESSAGES_VOICE_SAMPLE_RATE * 2,
        .block_align = 2,
        .bits = 16,
        .data = "data",
        .data_size = samples * 2,
    };
    httpd_resp_set_type(req, "audio/wav");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, (const char *)&h, sizeof(h)), TAG, "send failed");

    int16_t pcm[MESSAGES_VOICE_BLOCK_SAMPLES];
    for (uint16_t i = 0; i < msg.voice_blocks; i++) {
        if (messages_voice_decode(id, i, pcm) != ESP_OK) {
            memset(pcm, 0, sizeof(pcm));  // el mensaje se borro a mitad: completar con silencio
        }
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, (const char *)pcm, sizeof(pcm)), TAG, "send failed");
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// --- POST ------------------------------------------------------------------------

static esp_err_t post_alert(httpd_req_t *req)
{
    long kind;
    if (!query_int(req, "kind", &kind) || kind < 0 || kind >= ALERT_COUNT) {
        return reply_error(req, "400 Bad Request", "bad alert kind");
    }
    if (messages_send_alert(kind) != ESP_OK) {
        return reply_error(req, "503 Service Unavailable", "mesh busy");
    }
    return reply_ok(req);
}

static esp_err_t post_text(httpd_req_t *req)
{
    char text[MESSAGES_TEXT_MAX + 1];
    if (read_body(req, text, sizeof(text)) <= 0) {
        return reply_error(req, "400 Bad Request", "text must be 1-160 bytes");
    }
    if (messages_send_text(text) != ESP_OK) {
        return reply_error(req, "503 Service Unavailable", "mesh busy");
    }
    return reply_ok(req);
}

static esp_err_t post_name(httpd_req_t *req)
{
    char name[PRESENCE_NAME_MAX + 1];
    if (read_body(req, name, sizeof(name)) <= 0 || presence_set_name(name) != ESP_OK) {
        return reply_error(req, "400 Bad Request", "name must be 1-20 bytes");
    }
    return reply_ok(req);
}

esp_err_t api_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/state", .method = HTTP_GET, .handler = get_state},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = get_messages},
        {.uri = "/api/voice", .method = HTTP_GET, .handler = get_voice},
        {.uri = "/api/alert", .method = HTTP_POST, .handler = post_alert},
        {.uri = "/api/text", .method = HTTP_POST, .handler = post_text},
        {.uri = "/api/name", .method = HTTP_POST, .handler = post_name},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s failed", uris[i].uri);
    }
    return ESP_OK;
}
