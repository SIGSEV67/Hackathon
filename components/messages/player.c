#include <string.h>

#include "audio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "internal.h"
#include "store.h"

// Reproduce voces guardadas en el parlante de la placa. Un task propio decodifica
// bloque a bloque y escribe al I2S, que marca el ritmo.

static const char *TAG = "player";

#define BLOCK_MS (MESSAGES_VOICE_BLOCK_SAMPLES * 1000 / MESSAGES_VOICE_SAMPLE_RATE)

static TaskHandle_t s_task;
static volatile uint32_t s_request;   // id pedido por la API (0 = nada)
static volatile uint32_t s_playing;   // id que esta sonando (0 = nada)
static volatile uint16_t s_block;     // bloque en curso
static volatile bool s_stop;

static void play(uint32_t id)
{
    message_t msg;
    if (!store_get(id, &msg) || msg.kind != MESSAGE_VOICE) {
        return;
    }
    static int16_t pcm[MESSAGES_VOICE_BLOCK_SAMPLES];
    int16_t prev = 0;

    ESP_LOGI(TAG, "playing voice %" PRIu32 " (%u ms)", id, msg.blocks * BLOCK_MS);
    s_playing = id;
    for (uint16_t i = 0; i < msg.blocks && !s_stop; i++) {
        s_block = i;
        if (messages_voice_decode(id, i, pcm) != ESP_OK) {
            break;  // se borro mientras sonaba
        }
        if (audio_speaker_write_8k(pcm, MESSAGES_VOICE_BLOCK_SAMPLES, &prev, 1000) != ESP_OK) {
            break;
        }
    }
    // Un poco de silencio para vaciar el DMA sin cortar la ultima palabra.
    memset(pcm, 0, sizeof(pcm));
    audio_speaker_write_8k(pcm, MESSAGES_VOICE_BLOCK_SAMPLES, &prev, 1000);
    s_playing = 0;
    messages_notify(MESSAGES_EVENT_PLAY_STOPPED, id);
}

static void player_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const uint32_t id = s_request;
        s_request = 0;
        s_stop = false;
        if (id) {
            play(id);
        }
    }
}

esp_err_t player_init(void)
{
    if (!audio_has_speaker()) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(xTaskCreate(player_task, "player", 3072, NULL, 6, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    return ESP_OK;
}

esp_err_t messages_voice_play(uint32_t id)
{
    ESP_RETURN_ON_FALSE(s_task, ESP_ERR_NOT_SUPPORTED, TAG, "no speaker");
    message_t msg;
    ESP_RETURN_ON_FALSE(store_get(id, &msg) && msg.kind == MESSAGE_VOICE, ESP_ERR_NOT_FOUND, TAG, "no voice %" PRIu32,
                        id);
    s_stop = s_playing != 0;  // corta la que este sonando; el task arranca la nueva al terminar
    s_request = id;
    xTaskNotifyGive(s_task);
    return ESP_OK;
}

void messages_voice_play_stop(void)
{
    s_request = 0;
    s_stop = true;
}

bool messages_voice_playing(uint32_t *id, uint32_t *pos_ms)
{
    const uint32_t playing = s_playing;
    if (id) {
        *id = playing;
    }
    if (pos_ms) {
        *pos_ms = playing ? s_block * BLOCK_MS : 0;
    }
    return playing != 0;
}
