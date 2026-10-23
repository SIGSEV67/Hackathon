#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "store.h"

static const char *TAG = "store";

#define MAX_MESSAGES STORE_MAX_MESSAGES

typedef struct {
    bool used;
    message_t msg;
    uint16_t gain_q8;
    block_t *blocks;  // msg.blocks bloques, solo para voz
} slot_t;

// Ring en orden de llegada: s_head es el proximo slot a escribir (y el mas viejo).
static slot_t s_slots[MAX_MESSAGES];
static int s_head;
static uint32_t s_next_id = 1;
static size_t s_blocks_used;
static size_t s_blocks_budget;
static SemaphoreHandle_t s_lock;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static void release(slot_t *slot)
{
    if (slot->blocks) {
        s_blocks_used -= slot->msg.blocks * sizeof(block_t);
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

static bool evict_oldest_blocks(void)
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
    s_blocks_budget = voice_budget_bytes;
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

esp_err_t store_add_blocks(const message_t *msg, uint32_t *out_id)
{
    const size_t bytes = msg->blocks * sizeof(block_t);
    ESP_RETURN_ON_FALSE(msg->blocks > 0 && bytes <= s_blocks_budget, ESP_ERR_INVALID_SIZE, TAG,
                        "voice of %u blocks does not fit", msg->blocks);
    LOCK();
    while (s_blocks_used + bytes > s_blocks_budget && evict_oldest_blocks()) {
    }
    block_t *blocks = heap_caps_calloc_prefer(msg->blocks, sizeof(block_t), 2,
                                                    MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (!blocks) {
        UNLOCK();
        ESP_LOGE(TAG, "no mem for %u voice blocks", msg->blocks);
        return ESP_ERR_NO_MEM;
    }
    slot_t *slot = push(msg);
    slot->msg.blocks_received = 0;
    slot->blocks = blocks;
    s_blocks_used += bytes;
    *out_id = slot->msg.id;
    UNLOCK();
    return ESP_OK;
}

bool store_find(mesh_node_id_t origin, uint16_t ref, uint32_t *out_id)
{
    bool found = false;
    LOCK();
    for (int i = 0; i < MAX_MESSAGES && !found; i++) {
        const message_t *m = &s_slots[i].msg;
        // Las alertas no tienen ref (no se reenvian), no participan.
        if (s_slots[i].used && m->kind != MESSAGE_ALERT && m->origin == origin && m->ref == ref) {
            *out_id = m->id;
            found = true;
        }
    }
    UNLOCK();
    return found;
}

bool store_set_delivered(uint32_t id)
{
    bool changed = false;
    LOCK();
    slot_t *slot = find(id);
    if (slot && !slot->msg.delivered) {
        slot->msg.delivered = true;
        changed = true;
    }
    UNLOCK();
    return changed;
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

esp_err_t store_block_put(uint32_t id, uint16_t index, const block_t *block, bool *completed)
{
    esp_err_t err = ESP_OK;
    bool done = false;
    LOCK();
    slot_t *slot = find(id);
    if (!slot || !slot->blocks || index >= slot->msg.blocks) {
        err = ESP_ERR_NOT_FOUND;
    } else if (slot->blocks[index].present) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        slot->blocks[index] = *block;
        slot->blocks[index].present = 1;
        slot->msg.blocks_received++;
        slot->gain_q8 = 0;  // cambio el audio: recalcular la normalizacion
        done = slot->msg.blocks_received == slot->msg.blocks;
    }
    UNLOCK();
    if (completed) {
        *completed = done;
    }
    return err;
}

esp_err_t store_block_get(uint32_t id, uint16_t index, block_t *out)
{
    esp_err_t err = ESP_OK;
    LOCK();
    const slot_t *slot = find(id);
    if (!slot || !slot->blocks || index >= slot->msg.blocks) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        *out = slot->blocks[index];
    }
    UNLOCK();
    return err;
}

esp_err_t store_blocks_truncate(uint32_t id, uint16_t blocks)
{
    esp_err_t err = ESP_OK;
    LOCK();
    slot_t *slot = find(id);
    if (!slot || !slot->blocks || blocks == 0 || blocks > slot->msg.blocks) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        // Se devuelve lo que sobro: una grabacion reserva el largo maximo y sin
        // PSRAM no entra otra hasta liberarlo.
        const uint32_t caps = esp_ptr_external_ram(slot->blocks) ? MALLOC_CAP_SPIRAM : MALLOC_CAP_DEFAULT;
        block_t *shrunk = heap_caps_realloc(slot->blocks, blocks * sizeof(block_t), caps);
        if (shrunk) {
            slot->blocks = shrunk;
        }
        s_blocks_used -= (slot->msg.blocks - blocks) * sizeof(block_t);
        slot->msg.blocks = blocks;
        if (slot->msg.blocks_received > blocks) {
            slot->msg.blocks_received = blocks;
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
    if (slot && out) {
        *out = slot->msg;
    }
    UNLOCK();
    return slot != NULL;
}

size_t store_list(message_t *out, size_t max, uint32_t after_id, bool (*keep)(const message_t *))
{
    size_t n = 0;
    LOCK();
    for (int i = 1; i <= MAX_MESSAGES && n < max; i++) {
        const slot_t *slot = &s_slots[(s_head - i + MAX_MESSAGES) % MAX_MESSAGES];
        if (slot->used && slot->msg.id > after_id && (!keep || keep(&slot->msg))) {
            out[n++] = slot->msg;
        }
    }
    UNLOCK();
    return n;
}

size_t store_count(bool (*keep)(const message_t *))
{
    size_t n = 0;
    LOCK();
    for (int i = 0; i < MAX_MESSAGES; i++) {
        if (s_slots[i].used && keep(&s_slots[i].msg)) {
            n++;
        }
    }
    UNLOCK();
    return n;
}

size_t store_ids(uint32_t *out, size_t max, bool (*keep)(const message_t *))
{
    size_t n = 0;
    LOCK();
    for (int i = 1; i <= MAX_MESSAGES && n < max; i++) {
        const slot_t *slot = &s_slots[(s_head - i + MAX_MESSAGES) % MAX_MESSAGES];
        if (slot->used && (!keep || keep(&slot->msg))) {
            out[n++] = slot->msg.id;
        }
    }
    UNLOCK();
    return n;
}

size_t store_missing(uint32_t id, block_range_t *out, size_t max)
{
    size_t n = 0;
    LOCK();
    const slot_t *slot = find(id);
    if (slot && slot->blocks) {
        for (uint16_t i = 0; i < slot->msg.blocks && n < max; i++) {
            if (slot->blocks[i].present) {
                continue;
            }
            if (n > 0 && out[n - 1].start + out[n - 1].count == i) {
                out[n - 1].count++;
            } else {
                out[n++] = (block_range_t){.start = i, .count = 1};
            }
        }
    }
    UNLOCK();
    return n;
}

bool store_set_file_info(uint32_t id, uint32_t size, const char *name, const char *mime)
{
    LOCK();
    slot_t *slot = find(id);
    if (slot) {
        slot->msg.file_size = size;
        strlcpy(slot->msg.text, name, sizeof(slot->msg.text));
        strlcpy(slot->msg.mime, mime, sizeof(slot->msg.mime));
    }
    UNLOCK();
    return slot != NULL;
}
