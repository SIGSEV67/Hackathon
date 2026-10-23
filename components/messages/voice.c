#include <stdlib.h>
#include <string.h>

#include "adpcm.h"
#include "audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "internal.h"
#include "store.h"

static const char *TAG = "voice";

_Static_assert(AUDIO_SAMPLE_RATE_HZ == 2 * MESSAGES_VOICE_SAMPLE_RATE, "voice decimates the mic by 2");

#define BLOCK_MS          (MESSAGES_VOICE_BLOCK_SAMPLES * 1000 / MESSAGES_VOICE_SAMPLE_RATE)
#define MAX_BLOCKS        ((CONFIG_SIGSEV_VOICE_MAX_S * 1000 + BLOCK_MS - 1) / BLOCK_MS)
#define MIN_BLOCKS        5   // menos de ~250 ms es un toque sin querer
#define STOP_SETTLE_MS    40  // > un bloque del mic, para que termine el callback en curso
#define SEND_SPACING_MS   5   // aire libre entre bloques para los reenvios de los vecinos
#define SEND_WAIT_MS      500

// Igual que el texto, la identidad del mensaje va en el payload para que un
// custodio pueda reenviarlo tal cual.
typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
    uint32_t dest;
    uint16_t index;
    uint16_t count;
    int16_t predictor;
    uint8_t step_index;
    uint8_t reserved;
    uint8_t data[BLOCK_BYTES];
} wire_voice_t;

_Static_assert(sizeof(wire_voice_t) <= MESH_PAYLOAD_MAX, "voice block does not fit in a mesh packet");

// Estado de la grabacion. `active` lo escriben la API (start/stop) y el
// callback del mic (corte por largo maximo); el resto solo el callback mientras
// active == true. `busy` cubre desde start hasta que el task de voz termino de
// cerrar la grabacion, para que un start nuevo no pise el estado.
static struct {
    volatile bool busy;
    volatile bool active;
    uint32_t msg_id;
    volatile uint16_t blocks;
    uint32_t start_ms;
    adpcm_state_t enc;
    audio_framer_t framer;
    int16_t pcm[MESSAGES_VOICE_BLOCK_SAMPLES];
} s_rec = {.framer = {.frame = s_rec.pcm, .len = MESSAGES_VOICE_BLOCK_SAMPLES}};

// Trabajos del task de voz: REC_DONE cierra la grabacion de la placa; otro
// valor es el id de una voz importada que hay que mandar.
#define REC_DONE 0
static QueueHandle_t s_jobs;

// Voz que se esta importando (subida desde el celular). Solo una a la vez.
static struct {
    uint32_t msg_id;
    uint16_t blocks;
    adpcm_state_t enc;
} s_import;

static void post_job(uint32_t job)
{
    xQueueSend(s_jobs, &job, portMAX_DELAY);
}

// --- grabacion (task del mic) -------------------------------------------------

static void on_frame(const int16_t *pcm, size_t len, void *ctx)
{
    if (!s_rec.active) {
        return;  // se corto a mitad del chunk del mic
    }
    block_t block = {.predictor = s_rec.enc.predictor, .index = s_rec.enc.index};
    adpcm_encode(&s_rec.enc, pcm, MESSAGES_VOICE_BLOCK_SAMPLES, block.data);
    if (store_block_put(s_rec.msg_id, s_rec.blocks, &block, NULL) != ESP_OK) {
        s_rec.active = false;  // el store desalojo el mensaje: no hay donde seguir
        post_job(REC_DONE);
        return;
    }
    s_rec.blocks++;
    if (s_rec.blocks >= MAX_BLOCKS) {
        s_rec.active = false;
        post_job(REC_DONE);
    }
}

static void on_mic(const int16_t *samples, size_t count, void *ctx)
{
    if (!s_rec.active) {
        return;
    }
    audio_framer_push(&s_rec.framer, samples, count, on_frame, NULL);
}

// --- envio y cierre (task propio) ----------------------------------------------

esp_err_t voice_send_block(const message_t *msg, uint16_t index, const block_t *block)
{
    wire_voice_t w = {
        .origin = msg->origin,
        .ref = msg->ref,
        .dest = msg->dest,
        .index = index,
        .count = msg->blocks,
        .predictor = block->predictor,
        .step_index = block->index,
    };
    memcpy(w.data, block->data, sizeof(w.data));
    return mesh_send(MESH_TYPE_VOICE, &w, sizeof(w), pdMS_TO_TICKS(SEND_WAIT_MS));
}

// Manda los bloques que tengamos del mensaje (un custodio puede tener huecos).
static esp_err_t send_voice(const message_t *msg)
{
    const uint16_t blocks = msg->blocks;
    uint16_t sent = 0;
    for (uint16_t i = 0; i < blocks; i++) {
        block_t block;
        if (store_block_get(msg->id, i, &block) != ESP_OK) {
            ESP_LOGW(TAG, "message %" PRIu32 " gone while sending", msg->id);
            return ESP_ERR_NOT_FOUND;
        }
        if (!block.present) {
            continue;
        }
        if (voice_send_block(msg, i, &block) == ESP_OK) {
            sent++;
        } else {
            ESP_LOGW(TAG, "block %u/%u not sent", i, blocks);
        }
        vTaskDelay(pdMS_TO_TICKS(SEND_SPACING_MS));
    }
    ESP_LOGI(TAG, "sent voice %" PRIu32 " (%u/%u blocks, %u ms)", msg->id, sent, blocks, blocks * BLOCK_MS);
    return sent ? ESP_OK : ESP_FAIL;
}

static void finish_recording(void)
{
    vTaskDelay(pdMS_TO_TICKS(STOP_SETTLE_MS));
    const uint32_t id = s_rec.msg_id;
    const uint16_t blocks = s_rec.blocks;
    s_rec.busy = false;

    if (blocks < MIN_BLOCKS) {
        ESP_LOGI(TAG, "recording too short (%u ms), discarded", blocks * BLOCK_MS);
        store_remove(id);
        messages_notify(MESSAGES_EVENT_REC_STOPPED, 0);
        return;
    }
    store_blocks_truncate(id, blocks);
    messages_notify(MESSAGES_EVENT_REC_STOPPED, id);
    messages_notify(MESSAGES_EVENT_NEW, id);
    message_t msg;
    if (store_get(id, &msg) && send_voice(&msg) == ESP_OK) {
        courier_sent(id);
    }
}

static void voice_task(void *arg)
{
    while (1) {
        uint32_t job;
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        if (job == REC_DONE) {
            finish_recording();
            continue;
        }
        message_t msg;
        if (store_get(job, &msg) && send_voice(&msg) == ESP_OK) {
            courier_sent(job);
        }
    }
}

// --- recepcion (task de la mesh) -----------------------------------------------

static void on_voice(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    if (len < sizeof(wire_voice_t)) {
        return;
    }
    wire_voice_t w;
    memcpy(&w, payload, sizeof(w));
    if (w.count == 0 || w.count > MAX_BLOCKS || w.index >= w.count) {
        return;
    }

    uint32_t id;
    if (!store_find(w.origin, w.ref, &id)) {
        const message_t msg = {
            .kind = MESSAGE_VOICE,
            .origin = w.origin,
            .ref = w.ref,
            .dest = w.dest,
            .hops = meta->hops,
            .received_ms = mesh_now_ms(),
            .blocks = w.count,
        };
        if (store_add_blocks(&msg, &id) != ESP_OK) {
            return;
        }
        if (messages_is_visible(&msg)) {
            ESP_LOGI(TAG, "incoming %svoice from %08" PRIx32 " (%u ms)", msg.dest ? "private " : "", msg.origin,
                     w.count * BLOCK_MS);
            messages_notify(MESSAGES_EVENT_NEW, id);
        } else {
            ESP_LOGI(TAG, "holding private voice %08" PRIx32 "/%u for %08" PRIx32, msg.origin, msg.ref, msg.dest);
        }
    }

    block_t block = {.predictor = w.predictor, .index = w.step_index};
    memcpy(block.data, w.data, sizeof(block.data));
    bool completed = false;
    const esp_err_t err = store_block_put(id, w.index, &block, &completed);

    message_t msg;
    if (!store_get(id, &msg)) {
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        // Bloque repetido: alguien mas esta reenviando este mensaje.
        courier_heard(id);
        if (msg.dest == mesh_self_id() && msg.blocks_received == msg.blocks) {
            courier_ack(&msg);  // nuestro ACK se pudo haber perdido
        }
        return;
    }
    if (err == ESP_OK && completed) {
        ESP_LOGI(TAG, "voice %" PRIu32 " complete", id);
        if (messages_is_visible(&msg)) {
            messages_notify(MESSAGES_EVENT_UPDATED, id);
        }
        if (msg.dest == mesh_self_id()) {
            courier_ack(&msg);
        }
    }
}

// --- API -----------------------------------------------------------------------

esp_err_t voice_init(void)
{
    s_jobs = xQueueCreate(4, sizeof(uint32_t));
    ESP_RETURN_ON_FALSE(s_jobs, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_FALSE(xTaskCreate(voice_task, "voice", 4096, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    ESP_RETURN_ON_ERROR(audio_mic_add_sink(on_mic, NULL), TAG, "mic sink failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_VOICE, on_voice, NULL), TAG, "register failed");
    return ESP_OK;
}

esp_err_t voice_resend(const message_t *msg)
{
    return send_voice(msg);
}

esp_err_t messages_voice_start(mesh_node_id_t dest)
{
    ESP_RETURN_ON_FALSE(!s_rec.busy, ESP_ERR_INVALID_STATE, TAG, "already recording");
    ESP_RETURN_ON_FALSE(dest != mesh_self_id(), ESP_ERR_INVALID_ARG, TAG, "can't send to self");
    messages_voice_play_stop();  // el mic grabaria el parlante

    const message_t msg = {
        .kind = MESSAGE_VOICE,
        .origin = mesh_self_id(),
        .ref = messages_next_ref(),
        .dest = dest,
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
        .blocks = MAX_BLOCKS,
    };
    uint32_t id;
    ESP_RETURN_ON_ERROR(store_add_blocks(&msg, &id), TAG, "no room to record");

    s_rec.msg_id = id;
    s_rec.blocks = 0;
    s_rec.start_ms = mesh_now_ms();
    s_rec.enc = (adpcm_state_t){0};
    audio_framer_reset(&s_rec.framer);
    s_rec.busy = true;
    s_rec.active = true;

    ESP_LOGI(TAG, "recording%s...", dest ? " private" : "");
    messages_notify(MESSAGES_EVENT_REC_STARTED, id);
    return ESP_OK;
}

esp_err_t messages_voice_stop(void)
{
    if (!s_rec.active) {
        return ESP_ERR_INVALID_STATE;  // ya corto solo por largo maximo
    }
    s_rec.active = false;
    ESP_LOGI(TAG, "recording stopped after %" PRIu32 " ms", mesh_now_ms() - s_rec.start_ms);
    post_job(REC_DONE);
    return ESP_OK;
}

bool messages_voice_recording(uint32_t *elapsed_ms)
{
    if (elapsed_ms) {
        *elapsed_ms = s_rec.active ? mesh_now_ms() - s_rec.start_ms : 0;
    }
    return s_rec.active;
}

// --- importar (voz subida desde el celular) ------------------------------------------

esp_err_t messages_voice_import_begin(mesh_node_id_t dest, uint32_t samples, uint32_t *out_id)
{
    ESP_RETURN_ON_FALSE(dest != mesh_self_id(), ESP_ERR_INVALID_ARG, TAG, "can't send to self");
    ESP_RETURN_ON_FALSE(samples > 0, ESP_ERR_INVALID_SIZE, TAG, "empty voice");
    uint32_t blocks = (samples + MESSAGES_VOICE_BLOCK_SAMPLES - 1) / MESSAGES_VOICE_BLOCK_SAMPLES;
    blocks = blocks > MAX_BLOCKS ? MAX_BLOCKS : blocks;  // lo que sobre se corta

    const message_t msg = {
        .kind = MESSAGE_VOICE,
        .origin = mesh_self_id(),
        .ref = messages_next_ref(),
        .dest = dest,
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
        .blocks = blocks,
    };
    ESP_RETURN_ON_ERROR(store_add_blocks(&msg, &s_import.msg_id), TAG, "no room for voice");
    s_import.blocks = 0;
    s_import.enc = (adpcm_state_t){0};
    *out_id = s_import.msg_id;
    return ESP_OK;
}

esp_err_t messages_voice_import_block(uint32_t id, const int16_t *pcm)
{
    ESP_RETURN_ON_FALSE(id == s_import.msg_id, ESP_ERR_INVALID_STATE, TAG, "not importing %" PRIu32, id);
    block_t block = {.predictor = s_import.enc.predictor, .index = s_import.enc.index};
    adpcm_encode(&s_import.enc, pcm, MESSAGES_VOICE_BLOCK_SAMPLES, block.data);
    ESP_RETURN_ON_ERROR(store_block_put(id, s_import.blocks, &block, NULL), TAG, "voice evicted");
    s_import.blocks++;
    return ESP_OK;
}

esp_err_t messages_voice_import_end(uint32_t id, bool ok)
{
    ESP_RETURN_ON_FALSE(id == s_import.msg_id, ESP_ERR_INVALID_STATE, TAG, "not importing %" PRIu32, id);
    s_import.msg_id = 0;
    if (!ok || s_import.blocks < MIN_BLOCKS) {
        store_remove(id);
        return ok ? ESP_ERR_INVALID_SIZE : ESP_OK;
    }
    store_blocks_truncate(id, s_import.blocks);
    ESP_LOGI(TAG, "imported voice %" PRIu32 " (%u ms)", id, s_import.blocks * BLOCK_MS);
    messages_notify(MESSAGES_EVENT_NEW, id);
    post_job(id);
    return ESP_OK;
}

// --- reproduccion ----------------------------------------------------------------

static void decode_block(const block_t *block, int16_t *out)
{
    if (!block->present) {
        memset(out, 0, MESSAGES_VOICE_BLOCK_SAMPLES * sizeof(int16_t));
        return;
    }
    adpcm_state_t st = {.predictor = block->predictor, .index = block->index};
    adpcm_decode(&st, block->data, MESSAGES_VOICE_BLOCK_SAMPLES, out);
}

// Ganancia para llevar el pico del mensaje a ~full scale (los mics PDM graban
// bajo). Se cachea en el store y se invalida si llegan bloques nuevos.
static uint16_t gain_for(uint32_t id, uint16_t blocks)
{
    uint16_t gain = store_voice_gain(id);
    if (gain) {
        return gain;
    }
    int16_t pcm[MESSAGES_VOICE_BLOCK_SAMPLES];
    int peak = 1;
    for (uint16_t i = 0; i < blocks; i++) {
        block_t block;
        if (store_block_get(id, i, &block) != ESP_OK) {
            break;
        }
        decode_block(&block, pcm);
        for (int j = 0; j < MESSAGES_VOICE_BLOCK_SAMPLES; j++) {
            const int a = abs(pcm[j]);
            peak = a > peak ? a : peak;
        }
    }
    const uint32_t max_q8 = CONFIG_SIGSEV_VOICE_GAIN_MAX * 256;
    const uint32_t g = (uint32_t)30000 * 256 / peak;
    gain = (uint16_t)(g > max_q8 ? max_q8 : (g < 256 ? 256 : g));
    store_voice_set_gain(id, gain);
    return gain;
}

esp_err_t messages_voice_decode(uint32_t id, uint16_t index, int16_t *out)
{
    message_t msg;
    ESP_RETURN_ON_FALSE(store_get(id, &msg) && msg.kind == MESSAGE_VOICE, ESP_ERR_NOT_FOUND, TAG, "no voice %" PRIu32,
                        id);
    ESP_RETURN_ON_FALSE(index < msg.blocks, ESP_ERR_INVALID_ARG, TAG, "block out of range");

    block_t block;
    ESP_RETURN_ON_ERROR(store_block_get(id, index, &block), TAG, "block read failed");
    decode_block(&block, out);

    const int32_t gain = gain_for(id, msg.blocks);
    for (int i = 0; i < MESSAGES_VOICE_BLOCK_SAMPLES; i++) {
        const int32_t v = (out[i] * gain) >> 8;
        out[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
    }
    return ESP_OK;
}
