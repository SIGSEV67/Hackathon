#include <stdlib.h>
#include <string.h>

#include "adpcm.h"
#include "audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
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

typedef struct __attribute__((packed)) {
    uint16_t voice_ref;
    uint16_t index;
    uint16_t count;
    int16_t predictor;
    uint8_t step_index;
    uint8_t reserved;
    uint8_t data[VOICE_BLOCK_BYTES];
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
    uint16_t voice_ref;
    volatile uint16_t blocks;
    uint32_t start_ms;
    adpcm_state_t enc;
    int32_t dc_q8;     // estimacion del offset DC del mic, en Q8
    int32_t pair_sum;  // decimacion 2:1
    bool pair_half;
    int16_t pcm[MESSAGES_VOICE_BLOCK_SAMPLES];
    int pcm_len;
} s_rec;

static uint16_t s_next_ref;
static TaskHandle_t s_task;

// --- grabacion (task del mic) -------------------------------------------------

static void flush_block(void)
{
    voice_block_t block = {.predictor = s_rec.enc.predictor, .index = s_rec.enc.index};
    adpcm_encode(&s_rec.enc, s_rec.pcm, MESSAGES_VOICE_BLOCK_SAMPLES, block.data);
    if (store_voice_put(s_rec.msg_id, s_rec.blocks, &block, NULL) != ESP_OK) {
        s_rec.active = false;  // el store desalojo el mensaje: no hay donde seguir
        xTaskNotifyGive(s_task);
        return;
    }
    s_rec.blocks++;
    s_rec.pcm_len = 0;
    if (s_rec.blocks >= MAX_BLOCKS) {
        s_rec.active = false;
        xTaskNotifyGive(s_task);
    }
}

static void on_mic(const int16_t *samples, size_t count, void *ctx)
{
    if (!s_rec.active) {
        return;
    }
    for (size_t i = 0; i < count && s_rec.active; i++) {
        // Pasa-altos de un polo: saca el offset DC del PDM.
        const int32_t x = samples[i];
        s_rec.dc_q8 += ((x << 8) - s_rec.dc_q8) >> 7;
        const int32_t y = x - (s_rec.dc_q8 >> 8);

        // 16 kHz -> 8 kHz promediando de a pares (filtro anti-alias minimo).
        s_rec.pair_sum += y;
        s_rec.pair_half = !s_rec.pair_half;
        if (s_rec.pair_half) {
            continue;
        }
        int32_t v = s_rec.pair_sum / 2;
        s_rec.pair_sum = 0;
        v = v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v);
        s_rec.pcm[s_rec.pcm_len++] = (int16_t)v;
        if (s_rec.pcm_len == MESSAGES_VOICE_BLOCK_SAMPLES) {
            flush_block();
        }
    }
}

// --- envio y cierre (task propio) ----------------------------------------------

static void send_voice(uint32_t id, uint16_t ref, uint16_t blocks)
{
    for (uint16_t i = 0; i < blocks; i++) {
        voice_block_t block;
        if (store_voice_get(id, i, &block) != ESP_OK) {
            ESP_LOGW(TAG, "message %" PRIu32 " gone while sending", id);
            return;
        }
        wire_voice_t w = {
            .voice_ref = ref,
            .index = i,
            .count = blocks,
            .predictor = block.predictor,
            .step_index = block.index,
        };
        memcpy(w.data, block.data, sizeof(w.data));
        if (mesh_send(MESH_TYPE_VOICE, &w, sizeof(w), pdMS_TO_TICKS(SEND_WAIT_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "block %u/%u not sent", i, blocks);
        }
        vTaskDelay(pdMS_TO_TICKS(SEND_SPACING_MS));
    }
    ESP_LOGI(TAG, "sent voice %" PRIu32 " (%u blocks, %u ms)", id, blocks, blocks * BLOCK_MS);
}

static void finish_recording(void)
{
    vTaskDelay(pdMS_TO_TICKS(STOP_SETTLE_MS));
    const uint32_t id = s_rec.msg_id;
    const uint16_t ref = s_rec.voice_ref;
    const uint16_t blocks = s_rec.blocks;
    s_rec.busy = false;

    if (blocks < MIN_BLOCKS) {
        ESP_LOGI(TAG, "recording too short (%u ms), discarded", blocks * BLOCK_MS);
        store_remove(id);
        messages_notify(MESSAGES_EVENT_REC_STOPPED, 0);
        return;
    }
    store_voice_truncate(id, blocks);
    messages_notify(MESSAGES_EVENT_REC_STOPPED, id);
    messages_notify(MESSAGES_EVENT_NEW, id);
    send_voice(id, ref, blocks);
}

static void voice_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        finish_recording();
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
    if (!store_find_voice(meta->origin, w.voice_ref, &id)) {
        const message_t msg = {
            .kind = MESSAGE_VOICE,
            .origin = meta->origin,
            .hops = meta->hops,
            .received_ms = mesh_now_ms(),
            .voice_blocks = w.count,
        };
        if (store_add_voice(&msg, w.voice_ref, &id) != ESP_OK) {
            return;
        }
        ESP_LOGI(TAG, "incoming voice from %08" PRIx32 " (%u ms)", meta->origin, w.count * BLOCK_MS);
        messages_notify(MESSAGES_EVENT_NEW, id);
    }

    voice_block_t block = {.predictor = w.predictor, .index = w.step_index};
    memcpy(block.data, w.data, sizeof(block.data));
    bool completed;
    if (store_voice_put(id, w.index, &block, &completed) == ESP_OK && completed) {
        ESP_LOGI(TAG, "voice %" PRIu32 " complete", id);
        messages_notify(MESSAGES_EVENT_UPDATED, id);
    }
}

// --- API -----------------------------------------------------------------------

esp_err_t voice_init(void)
{
    s_next_ref = (uint16_t)esp_random();
    ESP_RETURN_ON_FALSE(xTaskCreate(voice_task, "voice", 4096, NULL, 5, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    ESP_RETURN_ON_ERROR(audio_mic_add_sink(on_mic, NULL), TAG, "mic sink failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_VOICE, on_voice, NULL), TAG, "register failed");
    return ESP_OK;
}

esp_err_t messages_voice_start(void)
{
    ESP_RETURN_ON_FALSE(!s_rec.busy, ESP_ERR_INVALID_STATE, TAG, "already recording");

    const uint16_t ref = s_next_ref++;
    const message_t msg = {
        .kind = MESSAGE_VOICE,
        .origin = mesh_self_id(),
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
        .voice_blocks = MAX_BLOCKS,
    };
    uint32_t id;
    ESP_RETURN_ON_ERROR(store_add_voice(&msg, ref, &id), TAG, "no room to record");

    s_rec.msg_id = id;
    s_rec.voice_ref = ref;
    s_rec.blocks = 0;
    s_rec.start_ms = mesh_now_ms();
    s_rec.enc = (adpcm_state_t){0};
    s_rec.pair_sum = 0;
    s_rec.pair_half = false;
    s_rec.pcm_len = 0;
    s_rec.busy = true;
    s_rec.active = true;

    messages_notify(MESSAGES_EVENT_REC_STARTED, id);
    return ESP_OK;
}

esp_err_t messages_voice_stop(void)
{
    if (!s_rec.active) {
        return ESP_ERR_INVALID_STATE;  // ya corto solo por largo maximo
    }
    s_rec.active = false;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

bool messages_voice_recording(uint32_t *elapsed_ms)
{
    if (elapsed_ms) {
        *elapsed_ms = s_rec.active ? mesh_now_ms() - s_rec.start_ms : 0;
    }
    return s_rec.active;
}

// --- reproduccion ----------------------------------------------------------------

static void decode_block(const voice_block_t *block, int16_t *out)
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
        voice_block_t block;
        if (store_voice_get(id, i, &block) != ESP_OK) {
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
    ESP_RETURN_ON_FALSE(index < msg.voice_blocks, ESP_ERR_INVALID_ARG, TAG, "block out of range");

    voice_block_t block;
    ESP_RETURN_ON_ERROR(store_voice_get(id, index, &block), TAG, "block read failed");
    decode_block(&block, out);

    const int32_t gain = gain_for(id, msg.voice_blocks);
    for (int i = 0; i < MESSAGES_VOICE_BLOCK_SAMPLES; i++) {
        const int32_t v = (out[i] * gain) >> 8;
        out[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
    }
    return ESP_OK;
}
