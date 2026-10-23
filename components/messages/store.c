#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "store.h"

static const char *TAG = "store";

#define MAX_MESSAGES 48

typedef struct {
    bool used;
    message_t msg;
    uint16_t voice_ref;
    uint16_t gain_q8;
    voice_block_t *blocks;  // msg.voice_blocks bloques, solo para voz
} slot_t;

// Ring en orden de llegada: s_head es el proximo slot a escribir (y el mas viejo).
static slot_t s_slots[MAX_MESSAGES];
static int s_head;
static uint32_t s_next_id = 1;
static size_t s_voice_used;
static size_t s_voice_budget;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void release(slot_t *slot)
{
    if (slot->blocks) {
        s_voice_used -= slot->msg.voice_blocks * sizeof(voice_block_t);
        heap_caps_free(slot->blocks);
    }
    memset(slot, 0, sizeof(*slot));
}

static slot_t *find(uint32_t id)
{
    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (s_slots[i].used && s_slots[i].msg.id == id) {
            return &s_slots[i];
        }
    }
    return NULL;
}

// Toma el proximo slot del ring (pisando el mas viejo) y le asigna id.
static slot_t *push(const message_t *msg)
{
    slot_t *slot = &s_slots[s_head];
    release(slot);
    s_head = (s_head + 1) % MAX_MESSAGES;
    slot->used = true;
    slot->msg = *msg;
    slot->msg.id = s_next_id++;
    return slot;
}

static bool evict_oldest_voice(void)
{
    for (int i = 0; i < MAX_MESSAGES; i++) {
        slot_t *slot = &s_slots[(s_head + i) % MAX_MESSAGES];
        if (slot->used && slot->blocks) {
            ESP_LOGW(TAG, "voice store full, dropping message %" PRIu32, slot->msg.id);
            release(slot);
            return true;
        }
    }
    return false;
}

esp_err_t store_init(size_t voice_budget_bytes)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mem for lock");
    s_voice_budget = voice_budget_bytes;
    ESP_LOGI(TAG, "%d messages, %u KB for voice (%s)", MAX_MESSAGES, (unsigned)(voice_budget_bytes / 1024),
             heap_caps_get_total_size(MALLOC_CAP_SPIRAM) ? "PSRAM" : "internal RAM");
    return ESP_OK;
}

esp_err_t store_add(const message_t *msg, uint32_t *out_id)
{
    LOCK();
    const slot_t *slot = push(msg);
    *out_id = slot->msg.id;
    UNLOCK();
    return ESP_OK;
}

esp_err_t store_add_voice(const message_t *msg, uint16_t voice_ref, uint32_t *out_id)
{
    const size_t bytes = msg->voice_blocks * sizeof(voice_block_t);
    ESP_RETURN_ON_FALSE(msg->voice_blocks > 0 && bytes <= s_voice_budget, ESP_ERR_INVALID_SIZE, TAG,
                        "voice of %u blocks does not fit", msg->voice_blocks);
    LOCK();
    while (s_voice_used + bytes > s_voice_budget && evict_oldest_voice()) {
    }
    voice_block_t *blocks = heap_caps_calloc_prefer(msg->voice_blocks, sizeof(voice_block_t), 2,
                                                    MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (!blocks) {
        UNLOCK();
        ESP_LOGE(TAG, "no mem for %u voice blocks", msg->voice_blocks);
        return ESP_ERR_NO_MEM;
    }
    slot_t *slot = push(msg);
    slot->msg.voice_received = 0;
    slot->voice_ref = voice_ref;
    slot->blocks = blocks;
    s_voice_used += bytes;
    *out_id = slot->msg.id;
    UNLOCK();
    return ESP_OK;
}

bool store_find_voice(mesh_node_id_t origin, uint16_t voice_ref, uint32_t *out_id)
{
    bool found = false;
    LOCK();
    for (int i = 0; i < MAX_MESSAGES && !found; i++) {
        const slot_t *slot = &s_slots[i];
        if (slot->used && slot->blocks && slot->msg.origin == origin && slot->voice_ref == voice_ref) {
            *out_id = slot->msg.id;
            found = true;
        }
    }
    UNLOCK();
    return found;
}

void store_remove(uint32_t id)
{
    LOCK();
    slot_t *slot = find(id);
    if (slot) {
        release(slot);
    }
    UNLOCK();
}

esp_err_t store_voice_put(uint32_t id, uint16_t index, const voice_block_t *block, bool *completed)
{
    esp_err_t err = ESP_OK;
    bool done = false;
    LOCK();
    slot_t *slot = find(id);
    if (!slot || !slot->blocks || index >= slot->msg.voice_blocks) {
        err = ESP_ERR_NOT_FOUND;
    } else if (slot->blocks[index].present) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        slot->blocks[index] = *block;
        slot->blocks[index].present = 1;
        slot->msg.voice_received++;
        slot->gain_q8 = 0;  // cambio el audio: recalcular la normalizacion
        done = slot->msg.voice_received == slot->msg.voice_blocks;
    }
    UNLOCK();
    if (completed) {
        *completed = done;
    }
    return err;
}

esp_err_t store_voice_get(uint32_t id, uint16_t index, voice_block_t *out)
{
    esp_err_t err = ESP_OK;
    LOCK();
    const slot_t *slot = find(id);
    if (!slot || !slot->blocks || index >= slot->msg.voice_blocks) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        *out = slot->blocks[index];
    }
    UNLOCK();
    return err;
}

esp_err_t store_voice_truncate(uint32_t id, uint16_t blocks)
{
    esp_err_t err = ESP_OK;
    LOCK();
    slot_t *slot = find(id);
    if (!slot || !slot->blocks || blocks == 0 || blocks > slot->msg.voice_blocks) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        // Se devuelve lo que sobro: una grabacion reserva el largo maximo y sin
        // PSRAM no entra otra hasta liberarlo.
        const uint32_t caps = esp_ptr_external_ram(slot->blocks) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT;
        voice_block_t *shrunk = heap_caps_realloc(slot->blocks, blocks * sizeof(voice_block_t), caps);
        if (shrunk) {
            slot->blocks = shrunk;
        }
        s_voice_used -= (slot->msg.voice_blocks - blocks) * sizeof(voice_block_t);
        slot->msg.voice_blocks = blocks;
        if (slot->msg.voice_received > blocks) {
            slot->msg.voice_received = blocks;
        }
    }
    UNLOCK();
    return err;
}

uint16_t store_voice_gain(uint32_t id)
{
    LOCK();
    const slot_t *slot = find(id);
    const uint16_t gain = slot ? slot->gain_q8 : 0;
    UNLOCK();
    return gain;
}

void store_voice_set_gain(uint32_t id, uint16_t gain_q8)
{
    LOCK();
    slot_t *slot = find(id);
    if (slot) {
        slot->gain_q8 = gain_q8;
    }
    UNLOCK();
}

bool store_get(uint32_t id, message_t *out)
{
    LOCK();
    const slot_t *slot = find(id);
    if (slot) {
        *out = slot->msg;
    }
    UNLOCK();
    return slot != NULL;
}

size_t store_list(message_t *out, size_t max, uint32_t after_id)
{
    size_t n = 0;
    LOCK();
    for (int i = 1; i <= MAX_MESSAGES && n < max; i++) {
        const slot_t *slot = &s_slots[(s_head - i + MAX_MESSAGES) % MAX_MESSAGES];
        if (slot->used && slot->msg.id > after_id) {
            out[n++] = slot->msg;
        }
    }
    UNLOCK();
    return n;
}
