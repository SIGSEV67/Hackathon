#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "store.h"

static const char *TAG = "store";

#define MAX_MESSAGES STORE_MAX_MESSAGES

// Los bloques de una voz van en segmentos chicos que se reservan
// cuando llega el primer bloque de cada uno: una grabacion ocupa lo grabado
// (no el largo maximo) y sin PSRAM no hace falta un bloque contiguo grande.
#define SEG_BLOCKS 32
#define SEG_BYTES  (SEG_BLOCKS * sizeof(block_t))

typedef struct {
    bool used;
    message_t msg;
    uint16_t gain_q8;
    block_t **segs;  // ceil(msg.blocks / SEG_BLOCKS) punteros (NULL = segmento sin bloques)
    uint16_t n_segs;
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

static void free_segs_from(slot_t *slot, uint16_t first)
{
    for (uint16_t i = first; i < slot->n_segs; i++) {
        if (slot->segs[i]) {
            heap_caps_free(slot->segs[i]);
            slot->segs[i] = NULL;
            s_blocks_used -= SEG_BYTES;
        }
    }
}

static void release(slot_t *slot)
{
    if (slot->segs) {
        free_segs_from(slot, 0);
        free(slot->segs);
    }
    memset(slot, 0, sizeof(*slot));
}

static const block_t *block_at(const slot_t *slot, uint16_t index)
{
    const block_t *seg = slot->segs[index / SEG_BLOCKS];
    return seg ? &seg[index % SEG_BLOCKS] : NULL;
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

// Libera la voz mas vieja, salvo `keep` (el que se esta llenando).
static bool evict_oldest_blocks(const slot_t *keep)
{
    for (int i = 0; i < MAX_MESSAGES; i++) {
        slot_t *slot = &s_slots[(s_head + i) % MAX_MESSAGES];
        if (slot->used && slot->segs && slot != keep) {
            ESP_LOGW(TAG, "block store full, dropping message %" PRIu32, slot->msg.id);
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
    ESP_RETURN_ON_FALSE(msg->blocks > 0 && msg->blocks * sizeof(block_t) <= s_blocks_budget, ESP_ERR_INVALID_SIZE,
                        TAG, "message of %u blocks does not fit", msg->blocks);
    const uint16_t n_segs = (msg->blocks + SEG_BLOCKS - 1) / SEG_BLOCKS;
    block_t **segs = calloc(n_segs, sizeof(block_t *));
    ESP_RETURN_ON_FALSE(segs, ESP_ERR_NO_MEM, TAG, "no mem");
    LOCK();
    slot_t *slot = push(msg);
    slot->msg.blocks_received = 0;
    slot->segs = segs;
    slot->n_segs = n_segs;
    *out_id = slot->msg.id;
    UNLOCK();
    return ESP_OK;
}

// Con el lock tomado. Reserva el segmento del bloque si hace falta, liberando
// mensajes viejos si se paso del presupuesto o no hay memoria.
static block_t *block_for_write(slot_t *slot, uint16_t index)
{
    block_t **seg = &slot->segs[index / SEG_BLOCKS];
    while (!*seg) {
        if (s_blocks_used + SEG_BYTES <= s_blocks_budget) {
            *seg = heap_caps_calloc_prefer(SEG_BLOCKS, sizeof(block_t), 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
            if (*seg) {
                s_blocks_used += SEG_BYTES;
                break;
            }
        }
        if (!evict_oldest_blocks(slot)) {
            ESP_LOGE(TAG, "no mem for blocks of message %" PRIu32, slot->msg.id);
            return NULL;
        }
    }
    return &(*seg)[index % SEG_BLOCKS];
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

bool store_set_read(uint32_t id)
{
    bool changed = false;
    LOCK();
    slot_t *slot = find(id);
    if (slot && !slot->msg.read) {
        slot->msg.read = true;
        slot->msg.delivered = true;
        changed = true;
    }
    UNLOCK();
    return changed;
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
    const block_t *existing = slot && slot->segs && index < slot->msg.blocks ? block_at(slot, index) : NULL;
    block_t *dst = NULL;
    if (!slot || !slot->segs || index >= slot->msg.blocks) {
        err = ESP_ERR_NOT_FOUND;
    } else if (existing && existing->present) {
        err = ESP_ERR_INVALID_STATE;
    } else if (!(dst = block_for_write(slot, index)) || !(slot = find(id))) {
        err = ESP_ERR_NO_MEM;  // (o el propio mensaje se libero buscando lugar)
    } else {
        *dst = *block;
        dst->present = 1;
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
    if (!slot || !slot->segs || index >= slot->msg.blocks) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        const block_t *b = block_at(slot, index);
        if (b) {
            *out = *b;
        } else {
            memset(out, 0, sizeof(*out));  // segmento sin bloques: todos ausentes
        }
    }
    UNLOCK();
    return err;
}

esp_err_t store_blocks_truncate(uint32_t id, uint16_t blocks)
{
    esp_err_t err = ESP_OK;
    LOCK();
    slot_t *slot = find(id);
    if (!slot || !slot->segs || blocks == 0 || blocks > slot->msg.blocks) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        free_segs_from(slot, (blocks + SEG_BLOCKS - 1) / SEG_BLOCKS);
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

size_t store_list(message_t *out, size_t max, uint32_t after_id, bool (*keep)(const message_t *, void *ctx),
                  void *ctx)
{
    size_t n = 0;
    LOCK();
    for (int i = 1; i <= MAX_MESSAGES && n < max; i++) {
        const slot_t *slot = &s_slots[(s_head - i + MAX_MESSAGES) % MAX_MESSAGES];
        if (slot->used && slot->msg.id > after_id && (!keep || keep(&slot->msg, ctx))) {
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
    if (slot && slot->segs) {
        for (uint16_t i = 0; i < slot->msg.blocks && n < max; i++) {
            const block_t *b = block_at(slot, i);
            if (b && b->present) {
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
