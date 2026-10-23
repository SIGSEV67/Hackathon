#include <string.h>

#include "adpcm.h"
#include "audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "presence.h"

#include "call.h"

static const char *TAG = "call";

ESP_EVENT_DEFINE_BASE(CALL_EVENT);

#define FRAME_SAMPLES   440  // 55 ms a 8 kHz, igual que los mensajes de voz
#define FRAME_BYTES     (FRAME_SAMPLES / 2)
#define TICK_MS         20
#define INVITE_EVERY_MS 1000
#define RING_EVERY_MS   2000
#define RING_TIMEOUT_MS 30000
#define PING_EVERY_MS   2000
#define LOST_AFTER_MS   10000
#define SILENT_AFTER_MS 300   // sin audio esto: el otro solto la tecla
#define PREBUFFER       2     // bloques antes de empezar a sonar (~110 ms)
#define JITTER_BLOCKS   8

typedef enum {
    OP_INVITE,
    OP_ACCEPT,
    OP_HANGUP,
    OP_BUSY,
    OP_PING,
    OP_AUDIO,
} op_t;

typedef struct __attribute__((packed)) {
    uint8_t op;
    uint16_t call_id;
    uint32_t to;
} wire_hdr_t;

typedef struct __attribute__((packed)) {
    wire_hdr_t hdr;
    uint16_t seq;
    int16_t predictor;
    uint8_t step_index;
    uint8_t data[FRAME_BYTES];
} wire_audio_t;

_Static_assert(sizeof(wire_audio_t) <= MESH_PAYLOAD_MAX, "call audio does not fit in a mesh packet");

typedef struct {
    uint16_t seq;
    adpcm_state_t state;
    uint8_t data[FRAME_BYTES];
} frame_t;

// Estado de la llamada: lo tocan la API, el handler de la mesh y el task de la
// llamada, siempre bajo s_lock. El mic solo lee `talking`.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static call_info_t s_info;
static uint16_t s_call_id;
static uint32_t s_last_heard_ms;
static uint32_t s_last_audio_ms;
static uint32_t s_last_tx_ms;  // ultimo invite o ping

static volatile bool s_talking;
static QueueHandle_t s_tx;      // frames del mic para mandar
static QueueHandle_t s_rx;      // frames recibidos para sonar (jitter buffer)
static uint16_t s_tx_seq;
static uint16_t s_rx_last_seq;

static int16_t s_mic_frame[FRAME_SAMPLES];
static audio_framer_t s_framer = {.frame = s_mic_frame, .len = FRAME_SAMPLES};
static adpcm_state_t s_enc;

#define LOCK()   taskENTER_CRITICAL(&s_lock)
#define UNLOCK() taskEXIT_CRITICAL(&s_lock)

// --- estado ---------------------------------------------------------------------

static void publish(void)
{
    call_info_t info;
    LOCK();
    info = s_info;
    UNLOCK();
    esp_event_post(CALL_EVENT, CALL_EVENT_CHANGED, &info, sizeof(info), pdMS_TO_TICKS(10));
}

// Con el lock tomado.
static void set_state(call_state_t state, mesh_node_id_t peer, call_end_t end)
{
    s_info.state = state;
    s_info.peer = peer;
    s_info.talking = false;
    s_info.peer_talking = false;
    s_info.since_ms = mesh_now_ms();
    if (state == CALL_IDLE) {
        s_info.last_end = end;
    }
    s_talking = false;
    s_last_heard_ms = s_info.since_ms;
}

static void send_op(op_t op, uint16_t call_id, mesh_node_id_t to)
{
    const wire_hdr_t w = {.op = op, .call_id = call_id, .to = to};
    mesh_send(MESH_TYPE_CALL, &w, sizeof(w), pdMS_TO_TICKS(50));
}

// --- mic (task del mic) -------------------------------------------------------------

static void on_frame(const int16_t *pcm, size_t len, void *ctx)
{
    frame_t f = {.seq = s_tx_seq++, .state = s_enc};
    adpcm_encode(&s_enc, pcm, FRAME_SAMPLES, f.data);
    xQueueSend(s_tx, &f, 0);  // si el task se atrasa, se pierde un bloque
}

static void on_mic(const int16_t *samples, size_t count, void *ctx)
{
    if (s_talking) {
        audio_framer_push(&s_framer, samples, count, on_frame, NULL);
    }
}

// --- recepcion (task de la mesh) ----------------------------------------------------

static void on_call(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_hdr_t h;
    if (len < sizeof(h)) {
        return;
    }
    memcpy(&h, payload, sizeof(h));
    if (h.to != mesh_self_id()) {
        return;
    }
    const uint32_t now = mesh_now_ms();
    bool changed = false;
    bool reply_busy = false;

    LOCK();
    const bool ours = s_info.state != CALL_IDLE && meta->origin == s_info.peer && h.call_id == s_call_id;
    if (ours) {
        s_last_heard_ms = now;
    }
    switch ((op_t)h.op) {
    case OP_INVITE:
        if (s_info.state == CALL_IDLE) {
            s_call_id = h.call_id;
            set_state(CALL_INCOMING, meta->origin, CALL_END_NONE);
            changed = true;
        } else if (!ours) {
            reply_busy = true;
        }
        break;
    case OP_ACCEPT:
        if (ours && s_info.state == CALL_OUTGOING) {
            set_state(CALL_ACTIVE, s_info.peer, CALL_END_NONE);
            changed = true;
        }
        break;
    case OP_HANGUP:
    case OP_BUSY:
        if (ours) {
            set_state(CALL_IDLE, 0, h.op == OP_BUSY ? CALL_END_BUSY : CALL_END_HANGUP);
            changed = true;
        }
        break;
    case OP_PING:
        break;
    case OP_AUDIO:
        if (ours && s_info.state == CALL_ACTIVE && !s_info.talking && len >= sizeof(wire_audio_t)) {
            s_last_audio_ms = now;
            if (!s_info.peer_talking) {
                s_info.peer_talking = true;
                changed = true;
            }
            wire_audio_t w;
            memcpy(&w, payload, sizeof(w));
            frame_t f = {.seq = w.seq, .state = {.predictor = w.predictor, .index = w.step_index}};
            memcpy(f.data, w.data, sizeof(f.data));
            UNLOCK();
            xQueueSend(s_rx, &f, 0);  // buffer lleno: el bloque se pierde
            LOCK();
        }
        break;
    }
    UNLOCK();

    if (reply_busy) {
        send_op(OP_BUSY, h.call_id, meta->origin);
    }
    if (changed) {
        ESP_LOGI(TAG, "op %u from %08" PRIx32 " -> state %d", h.op, meta->origin, s_info.state);
        publish();
    }
}

// --- task: senalizacion, envio de audio y tonos -------------------------------------

static void send_audio(const frame_t *f, uint16_t call_id, mesh_node_id_t to)
{
    wire_audio_t w = {
        .hdr = {.op = OP_AUDIO, .call_id = call_id, .to = to},
        .seq = f->seq,
        .predictor = f->state.predictor,
        .step_index = f->state.index,
    };
    memcpy(w.data, f->data, sizeof(w.data));
    mesh_send(MESH_TYPE_CALL, &w, sizeof(w), pdMS_TO_TICKS(20));
}

static void call_task(void *arg)
{
    uint32_t last_ring = 0;
    while (1) {
        frame_t f;
        const bool have = xQueueReceive(s_tx, &f, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
        const uint32_t now = mesh_now_ms();

        LOCK();
        const call_info_t info = s_info;
        const uint16_t call_id = s_call_id;
        const uint32_t heard = s_last_heard_ms;
        bool changed = false;
        op_t op = OP_PING;
        bool send = false;
        if (info.state == CALL_ACTIVE && info.peer_talking && now - s_last_audio_ms > SILENT_AFTER_MS) {
            s_info.peer_talking = false;
            changed = true;
        }
        if (info.state == CALL_OUTGOING && now - info.since_ms > RING_TIMEOUT_MS) {
            set_state(CALL_IDLE, 0, CALL_END_NO_ANSWER);
            op = OP_HANGUP;
            send = changed = true;
        } else if (info.state == CALL_INCOMING && now - heard > LOST_AFTER_MS / 2) {
            set_state(CALL_IDLE, 0, CALL_END_NO_ANSWER);  // el que llamaba corto
            changed = true;
        } else if (info.state == CALL_ACTIVE && now - heard > LOST_AFTER_MS) {
            set_state(CALL_IDLE, 0, CALL_END_LOST);
            changed = true;
        } else if (info.state == CALL_OUTGOING && now - s_last_tx_ms > INVITE_EVERY_MS) {
            op = OP_INVITE;
            send = true;
            s_last_tx_ms = now;
        } else if (info.state == CALL_ACTIVE && now - s_last_tx_ms > PING_EVERY_MS) {
            send = true;
            s_last_tx_ms = now;
        }
        UNLOCK();

        if (have && info.state == CALL_ACTIVE && info.talking) {
            send_audio(&f, call_id, info.peer);
        }
        if (send) {
            send_op(op, call_id, info.peer);
        }
        if (changed) {
            publish();
        }
        // Timbre: el que recibe suena; el que llama escucha un tono mas suave.
        if ((info.state == CALL_INCOMING || info.state == CALL_OUTGOING) && now - last_ring > RING_EVERY_MS) {
            last_ring = now;
            if (info.state == CALL_INCOMING) {
                audio_play_tone(880, 150);
                audio_play_tone(660, 150);
            } else {
                audio_play_tone(440, 300);
            }
        }
    }
}

// --- task: audio del otro al parlante -------------------------------------------------

static void play_task(void *arg)
{
    static int16_t pcm[FRAME_SAMPLES];
    int16_t prev = 0;
    while (1) {
        // Esperar un par de bloques antes de arrancar: absorbe el jitter de la mesh.
        while (uxQueueMessagesWaiting(s_rx) < PREBUFFER) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        frame_t f;
        while (xQueueReceive(s_rx, &f, pdMS_TO_TICKS(150)) == pdTRUE) {
            if ((int16_t)(f.seq - s_rx_last_seq) <= 0 && s_rx_last_seq != 0) {
                continue;  // llego tarde o repetido
            }
            s_rx_last_seq = f.seq;
            adpcm_state_t st = f.state;
            adpcm_decode(&st, f.data, FRAME_SAMPLES, pcm);
            audio_speaker_write_8k(pcm, FRAME_SAMPLES, &prev, 500);
        }
        s_rx_last_seq = 0;  // se corto el audio: la proxima rafaga empieza de cero
    }
}

// --- API -------------------------------------------------------------------------

esp_err_t call_init(void)
{
    s_tx = xQueueCreate(4, sizeof(frame_t));
    s_rx = xQueueCreate(JITTER_BLOCKS, sizeof(frame_t));
    ESP_RETURN_ON_FALSE(s_tx && s_rx, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_CALL, on_call, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(audio_mic_add_sink(on_mic, NULL), TAG, "mic sink failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(call_task, "call", 3584, NULL, 6, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    if (audio_has_speaker()) {
        ESP_RETURN_ON_FALSE(xTaskCreate(play_task, "call_play", 3072, NULL, 7, NULL) == pdPASS, ESP_ERR_NO_MEM,
                            TAG, "task create failed");
    }
    return ESP_OK;
}

esp_err_t call_dial(mesh_node_id_t peer)
{
    ESP_RETURN_ON_FALSE(peer && peer != mesh_self_id(), ESP_ERR_INVALID_ARG, TAG, "bad peer");
    LOCK();
    const bool idle = s_info.state == CALL_IDLE;
    if (idle) {
        s_call_id = (uint16_t)esp_random();
        set_state(CALL_OUTGOING, peer, CALL_END_NONE);
        s_last_tx_ms = 0;  // el task manda el primer invite ya
    }
    UNLOCK();
    ESP_RETURN_ON_FALSE(idle, ESP_ERR_INVALID_STATE, TAG, "already in a call");
    ESP_LOGI(TAG, "calling %08" PRIx32, peer);
    publish();
    return ESP_OK;
}

esp_err_t call_answer(void)
{
    LOCK();
    const bool ringing = s_info.state == CALL_INCOMING;
    const mesh_node_id_t peer = s_info.peer;
    const uint16_t call_id = s_call_id;
    if (ringing) {
        set_state(CALL_ACTIVE, peer, CALL_END_NONE);
        s_last_tx_ms = mesh_now_ms();
    }
    UNLOCK();
    ESP_RETURN_ON_FALSE(ringing, ESP_ERR_INVALID_STATE, TAG, "nobody is calling");
    send_op(OP_ACCEPT, call_id, peer);
    publish();
    return ESP_OK;
}

esp_err_t call_hangup(void)
{
    LOCK();
    const bool in_call = s_info.state != CALL_IDLE;
    const mesh_node_id_t peer = s_info.peer;
    const uint16_t call_id = s_call_id;
    if (in_call) {
        set_state(CALL_IDLE, 0, CALL_END_HANGUP);
    }
    UNLOCK();
    ESP_RETURN_ON_FALSE(in_call, ESP_ERR_INVALID_STATE, TAG, "not in a call");
    send_op(OP_HANGUP, call_id, peer);
    publish();
    return ESP_OK;
}

esp_err_t call_talk(bool on)
{
    LOCK();
    const bool active = s_info.state == CALL_ACTIVE;
    if (active && s_info.talking != on) {
        s_info.talking = on;
        if (on) {
            s_info.peer_talking = false;  // half-duplex: mientras hablamos no escuchamos
            audio_framer_reset(&s_framer);
            s_enc = (adpcm_state_t){0};
        }
        s_talking = on;
    }
    UNLOCK();
    ESP_RETURN_ON_FALSE(active, ESP_ERR_INVALID_STATE, TAG, "no active call");
    publish();
    return ESP_OK;
}

void call_get(call_info_t *out)
{
    LOCK();
    *out = s_info;
    UNLOCK();
}
