// API JSON para la app web:
//   GET  /api/state             nodo propio, nodos de la red, historial, estadisticas
//   GET  /api/messages?after=N  mensajes con id > N, del mas nuevo al mas viejo
//   GET  /api/voice?id=N        mensaje de voz como WAV (PCM 16 bits, 8 kHz)
//   POST /api/alert?kind=N      manda una alerta (alert_kind_t)
//   POST /api/text?to=ID        manda el body como texto (to = id de 8 hex: privado)
//   POST /api/name              renombra este nodo con el body
//   POST /api/voice?to=ID       manda el body (PCM 16 bits mono, 8 kHz) como voz
//   POST /api/play?id=N         reproduce una voz en el parlante de la placa
//   GET  /api/file?id=N         baja un archivo recibido
//   POST /api/file?to=ID        manda el body como archivo. Content-Type es su tipo y
//                               X-File-Name su nombre (URL-encoded)

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "api.h"
#include "audio.h"
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

static bool query_num(httpd_req_t *req, const char *key, int base, long long *out)
{
    char query[64], val[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, val, sizeof(val)) != ESP_OK) {
        return false;
    }
    char *end;
    *out = strtoll(val, &end, base);
    return end != val && *end == '\0';
}

static bool query_int(httpd_req_t *req, const char *key, long *out)
{
    long long v;
    if (!query_num(req, key, 10, &v)) {
        return false;
    }
    *out = (long)v;
    return true;
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
    json_fmt(&out, ",\"rec\":{\"active\":%s,\"ms\":%" PRIu32 "}", recording ? "true" : "false", rec_ms);
    free(nodes);

    // Historial de nodos conocidos: para elegir el destino de un privado.
    presence_known_t *known = malloc(sizeof(presence_known_t) * PRESENCE_MAX_KNOWN);
    const size_t n_known = known ? presence_known(known, PRESENCE_MAX_KNOWN) : 0;
    json_raw(&out, ",\"known\":[");
    for (size_t i = 0; i < n_known; i++) {
        json_raw(&out, i ? ",{\"id\":" : "{\"id\":");
        json_id(&out, known[i].id);
        json_raw(&out, ",\"name\":");
        json_str(&out, known[i].name);
        json_fmt(&out, ",\"alive\":%s,\"seen\":%s,\"age\":%" PRIu32 "}", known[i].alive ? "true" : "false",
                 known[i].seen ? "true" : "false", known[i].age_ms / 1000);
    }
    free(known);
    json_fmt(&out, "],\"transit\":%u,\"voice_max_s\":%d,\"file_max\":%u,\"speaker\":%s}",
             (unsigned)messages_transit_count(), CONFIG_SIGSEV_VOICE_MAX_S, (unsigned)messages_file_max_bytes(),
             audio_has_speaker() ? "true" : "false");
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
    case MESSAGE_FILE:
        return "file";
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
        if (m->dest != MESSAGES_TO_ALL) {
            char to[PRESENCE_NAME_MAX + 1];
            presence_name_of(m->dest, to, sizeof(to));
            json_raw(&out, ",\"to\":");
            json_id(&out, m->dest);
            json_raw(&out, ",\"to_name\":");
            json_str(&out, to);
            json_fmt(&out, ",\"delivered\":%s", m->delivered ? "true" : "false");
        }
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
            json_fmt(&out, ",\"blocks\":%u,\"got\":%u,\"ms\":%u", m->blocks, m->blocks_received,
                     m->blocks * MESSAGES_VOICE_BLOCK_SAMPLES * 1000 / MESSAGES_VOICE_SAMPLE_RATE);
            break;
        case MESSAGE_FILE:
            json_fmt(&out, ",\"blocks\":%u,\"got\":%u,\"size\":%" PRIu32 ",\"name\":", m->blocks,
                     m->blocks_received, m->file_size);
            json_str(&out, m->text);
            json_raw(&out, ",\"mime\":");
            json_str(&out, m->mime);
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

    const uint32_t samples = (uint32_t)msg.blocks * MESSAGES_VOICE_BLOCK_SAMPLES;
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
    for (uint16_t i = 0; i < msg.blocks; i++) {
        if (messages_voice_decode(id, i, pcm) != ESP_OK) {
            memset(pcm, 0, sizeof(pcm));  // el mensaje se borro a mitad: completar con silencio
        }
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, (const char *)pcm, sizeof(pcm)), TAG, "send failed");
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t get_file(httpd_req_t *req)
{
    long id;
    message_t msg;
    if (!query_int(req, "id", &id) || !messages_get(id, &msg) || msg.kind != MESSAGE_FILE) {
        return reply_error(req, "404 Not Found", "no such file");
    }
    if (msg.file_size == 0 || msg.blocks_received < msg.blocks) {
        return reply_error(req, "409 Conflict", "file still arriving");
    }
    httpd_resp_set_type(req, msg.mime[0] ? msg.mime : "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");  // un id no cambia de contenido
    uint8_t chunk[MESSAGES_FILE_CHUNK];
    for (uint16_t i = 0; i < msg.blocks; i++) {
        size_t len;
        if (messages_file_read(id, i, chunk, &len) != ESP_OK) {
            break;  // se borro a mitad: el archivo sale cortado
        }
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(req, (const char *)chunk, len), TAG, "send failed");
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

// --- POST ------------------------------------------------------------------------

// Lee exactamente len bytes del body.
static bool recv_exact(httpd_req_t *req, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        const int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    return true;
}

static bool query_dest(httpd_req_t *req, mesh_node_id_t *out)
{
    long long to = MESSAGES_TO_ALL;
    if (query_num(req, "to", 16, &to) && (to < 0 || to > UINT32_MAX || to == mesh_self_id())) {
        return false;
    }
    *out = (mesh_node_id_t)to;
    return true;
}

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
    mesh_node_id_t to;
    if (!query_dest(req, &to)) {
        return reply_error(req, "400 Bad Request", "bad destination");
    }
    char text[MESSAGES_TEXT_MAX + 1];
    if (read_body(req, text, sizeof(text)) <= 0) {
        return reply_error(req, "400 Bad Request", "text must be 1-160 bytes");
    }
    if (messages_send_text(to, text) != ESP_OK) {
        return reply_error(req, "503 Service Unavailable", "mesh busy");
    }
    return reply_ok(req);
}

static esp_err_t post_voice(httpd_req_t *req)
{
    mesh_node_id_t to;
    if (!query_dest(req, &to)) {
        return reply_error(req, "400 Bad Request", "bad destination");
    }
    const uint32_t samples = req->content_len / sizeof(int16_t);
    uint32_t id;
    if (samples == 0 || messages_voice_import_begin(to, samples, &id) != ESP_OK) {
        return reply_error(req, "503 Service Unavailable", "no room for voice");
    }

    // Lo que pase del largo maximo se lee y se descarta.
    static int16_t pcm[MESSAGES_VOICE_BLOCK_SAMPLES];  // un solo task de httpd
    bool ok = true;
    for (uint32_t left = samples; left > 0 && ok;) {
        const uint32_t n = left < MESSAGES_VOICE_BLOCK_SAMPLES ? left : MESSAGES_VOICE_BLOCK_SAMPLES;
        memset(pcm, 0, sizeof(pcm));
        ok = recv_exact(req, pcm, n * sizeof(int16_t));
        left -= n;
        if (ok && messages_voice_import_block(id, pcm) != ESP_OK) {
            // Ya no entra (largo maximo o memoria): drenar el resto del body.
            while (ok && left > 0) {
                const uint32_t m = left < MESSAGES_VOICE_BLOCK_SAMPLES ? left : MESSAGES_VOICE_BLOCK_SAMPLES;
                ok = recv_exact(req, pcm, m * sizeof(int16_t));
                left -= m;
            }
            break;
        }
    }
    const esp_err_t err = messages_voice_import_end(id, ok);
    if (!ok) {
        return reply_error(req, "400 Bad Request", "upload cut");
    }
    if (err != ESP_OK) {
        return reply_error(req, "400 Bad Request", "voice too short");
    }
    return reply_ok(req);
}

// Decodifica %XX en el lugar (lo que manda encodeURIComponent).
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        int hi, lo;
        if (*s == '%' && s[1] && s[2] && sscanf((char[]){s[1], s[2], 0}, "%1x%1x", &hi, &lo) == 2) {
            *out++ = (char)(hi << 4 | lo);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

static esp_err_t post_file(httpd_req_t *req)
{
    mesh_node_id_t to;
    if (!query_dest(req, &to)) {
        return reply_error(req, "400 Bad Request", "bad destination");
    }
    if (req->content_len == 0 || req->content_len > messages_file_max_bytes()) {
        return reply_error(req, "413 Payload Too Large", "file too big");
    }
    char mime[MESSAGES_MIME_MAX + 1] = "";
    char name[MESSAGES_FILE_NAME_MAX * 3 + 1] = "";  // URL-encoded: hasta 3 bytes por byte
    httpd_req_get_hdr_value_str(req, "Content-Type", mime, sizeof(mime));
    httpd_req_get_hdr_value_str(req, "X-File-Name", name, sizeof(name));
    url_decode(name);
    name[MESSAGES_FILE_NAME_MAX] = '\0';

    uint32_t id;
    if (messages_file_begin(to, name, mime, req->content_len, &id) != ESP_OK) {
        return reply_error(req, "503 Service Unavailable", "no room for file");
    }
    static uint8_t buf[1024];  // un solo task de httpd
    bool ok = true;
    for (size_t left = req->content_len; left > 0 && ok;) {
        const size_t n = left < sizeof(buf) ? left : sizeof(buf);
        ok = recv_exact(req, buf, n) && messages_file_write(id, buf, n) == ESP_OK;
        left -= n;
    }
    if (messages_file_end(id, ok) != ESP_OK) {
        return reply_error(req, "400 Bad Request", "upload failed");
    }
    return reply_ok(req);
}

static esp_err_t post_play(httpd_req_t *req)
{
    long id;
    if (!query_int(req, "id", &id)) {
        return reply_error(req, "400 Bad Request", "bad id");
    }
    const esp_err_t err = messages_voice_play(id);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return reply_error(req, "501 Not Implemented", "no speaker");
    }
    if (err != ESP_OK) {
        return reply_error(req, "404 Not Found", "no such voice");
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
        {.uri = "/api/voice", .method = HTTP_POST, .handler = post_voice},
        {.uri = "/api/file", .method = HTTP_GET, .handler = get_file},
        {.uri = "/api/file", .method = HTTP_POST, .handler = post_file},
        {.uri = "/api/play", .method = HTTP_POST, .handler = post_play},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &uris[i]), TAG, "register %s failed", uris[i].uri);
    }
    return ESP_OK;
}
