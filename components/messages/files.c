#include <stddef.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "internal.h"

// Archivos (fotos, PDFs, lo que sea) que viajan por la mesh en pedazos de
// MESSAGES_FILE_CHUNK bytes. Los datos del archivo (nombre, tipo, largo) van en
// un paquete aparte que se repite al principio de cada envio. A diferencia de
// la voz, a un archivo no le puede faltar nada: lo que se pierde se pide de
// nuevo (repair.c).

static const char *TAG = "files";

_Static_assert(MESSAGES_FILE_CHUNK == BLOCK_BYTES, "a file chunk fills a store block");

#define MAX_BYTES  (CONFIG_SIGSEV_FILE_MAX_KB * 1024)
#define MAX_BLOCKS ((MAX_BYTES + MESSAGES_FILE_CHUNK - 1) / MESSAGES_FILE_CHUNK)

typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
    uint32_t dest;
    uint32_t size;
    uint16_t count;
    char mime[MESSAGES_MIME_MAX + 1];
    uint8_t name_len;
    char name[MESSAGES_FILE_NAME_MAX];
} wire_file_info_t;

typedef struct __attribute__((packed)) {
    uint32_t origin;
    uint16_t ref;
    uint32_t dest;
    uint16_t index;
    uint16_t count;
    uint8_t data[MESSAGES_FILE_CHUNK];
} wire_file_chunk_t;

_Static_assert(sizeof(wire_file_info_t) <= MESH_PAYLOAD_MAX, "file info does not fit in a mesh packet");
_Static_assert(sizeof(wire_file_chunk_t) <= MESH_PAYLOAD_MAX, "file chunk does not fit in a mesh packet");

// Archivo que se esta subiendo desde el telefono. Uno a la vez (un solo task de httpd).
static struct {
    uint32_t id;
    uint16_t index;
    size_t fill;
    block_t block;
} s_upload;

static uint16_t blocks_for(uint32_t size)
{
    return (size + MESSAGES_FILE_CHUNK - 1) / MESSAGES_FILE_CHUNK;
}

// Busca el archivo o lo crea con lo que sepamos hasta ahora.
static bool find_or_add(uint32_t origin, uint16_t ref, uint32_t dest, uint16_t count, uint8_t hops, uint32_t *id)
{
    if (store_find(origin, ref, id)) {
        return true;
    }
    if (count == 0 || count > MAX_BLOCKS) {
        return false;
    }
    const message_t msg = {
        .kind = MESSAGE_FILE,
        .origin = origin,
        .ref = ref,
        .dest = dest,
        .hops = hops,
        .received_ms = mesh_now_ms(),
        .blocks = count,
    };
    if (store_add_blocks(&msg, id) != ESP_OK) {
        return false;
    }
    if (messages_is_local(&msg)) {
        ESP_LOGI(TAG, "incoming %sfile from %08" PRIx32 " (%u chunks)", dest ? "private " : "", origin, count);
        messages_notify(MESSAGES_EVENT_NEW, *id);
    } else {
        ESP_LOGI(TAG, "holding private file %08" PRIx32 "/%u for %08" PRIx32, origin, ref, dest);
    }
    return true;
}

static void on_info(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_file_info_t w;
    if (len < offsetof(wire_file_info_t, name) || len > sizeof(w)) {
        return;
    }
    memset(&w, 0, sizeof(w));
    memcpy(&w, payload, len);
    if (w.size == 0 || w.size > MAX_BYTES || blocks_for(w.size) != w.count || w.name_len > MESSAGES_FILE_NAME_MAX) {
        return;
    }
    uint32_t id;
    if (!find_or_add(w.origin, w.ref, w.dest, w.count, meta->hops, &id)) {
        return;
    }
    message_t msg;
    if (store_get(id, &msg) && msg.file_size == 0) {
        char name[MESSAGES_FILE_NAME_MAX + 1];
        memcpy(name, w.name, w.name_len);
        name[w.name_len] = '\0';
        w.mime[MESSAGES_MIME_MAX] = '\0';
        store_set_file_info(id, w.size, name, w.mime);
        if (messages_is_local(&msg)) {
            messages_notify(MESSAGES_EVENT_UPDATED, id);
        }
    }
}

static void on_chunk(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_file_chunk_t w;
    if (len < sizeof(w)) {
        return;
    }
    memcpy(&w, payload, sizeof(w));
    if (w.index >= w.count) {
        return;
    }
    uint32_t id;
    if (!find_or_add(w.origin, w.ref, w.dest, w.count, meta->hops, &id)) {
        return;
    }
    block_t block = {0};
    memcpy(block.data, w.data, sizeof(block.data));
    bool completed = false;
    const esp_err_t err = store_block_put(id, w.index, &block, &completed);

    message_t msg;
    if (!store_get(id, &msg)) {
        return;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        courier_heard(id);  // repetido: alguien mas lo esta mandando
        if (messages_dest_is_here(&msg) && msg.blocks_received == msg.blocks) {
            courier_ack(&msg);
        }
        return;
    }
    if (err == ESP_OK && completed) {
        ESP_LOGI(TAG, "file %" PRIu32 " complete (%s)", id, msg.text[0] ? msg.text : "sin datos todavia");
        if (messages_is_local(&msg)) {
            messages_notify(MESSAGES_EVENT_UPDATED, id);
        }
        if (messages_dest_is_here(&msg)) {
            courier_ack(&msg);
        }
    }
}

esp_err_t files_send_info(const message_t *msg)
{
    wire_file_info_t w = {
        .origin = msg->origin,
        .ref = msg->ref,
        .dest = msg->dest,
        .size = msg->file_size,
        .count = msg->blocks,
    };
    strlcpy(w.mime, msg->mime, sizeof(w.mime));
    const size_t name_len = strnlen(msg->text, MESSAGES_FILE_NAME_MAX);
    w.name_len = name_len;
    memcpy(w.name, msg->text, name_len);
    return mesh_send(MESH_TYPE_FILE_INFO, &w, offsetof(wire_file_info_t, name) + name_len, pdMS_TO_TICKS(500));
}

esp_err_t files_send_chunk(const message_t *msg, uint16_t index, const block_t *block)
{
    wire_file_chunk_t w = {
        .origin = msg->origin,
        .ref = msg->ref,
        .dest = msg->dest,
        .index = index,
        .count = msg->blocks,
    };
    memcpy(w.data, block->data, sizeof(w.data));
    return mesh_send(MESH_TYPE_FILE, &w, sizeof(w), pdMS_TO_TICKS(500));
}

esp_err_t files_resend(const message_t *msg)
{
    sender_post(msg->id, 0, msg->blocks, true);
    return ESP_OK;
}

esp_err_t files_init(void)
{
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_FILE_INFO, on_info, NULL), TAG, "register info failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_FILE, on_chunk, NULL), TAG, "register chunk failed");
    return ESP_OK;
}

// --- API -------------------------------------------------------------------------

size_t messages_file_max_bytes(void)
{
    return MAX_BYTES;
}

esp_err_t messages_file_begin(mesh_node_id_t from, mesh_node_id_t dest, const char *name, const char *mime,
                              uint32_t size, uint32_t *out_id)
{
    ESP_RETURN_ON_FALSE(messages_valid_sender(from), ESP_ERR_INVALID_ARG, TAG, "unknown sender");
    ESP_RETURN_ON_FALSE(dest != from, ESP_ERR_INVALID_ARG, TAG, "can't send to self");
    ESP_RETURN_ON_FALSE(size > 0 && size <= MAX_BYTES, ESP_ERR_INVALID_SIZE, TAG, "file of %" PRIu32 " bytes",
                        size);
    message_t msg = {
        .kind = MESSAGE_FILE,
        .origin = from,
        .ref = messages_next_ref(),
        .dest = dest,
        .hops = MESSAGE_HOPS_SELF,
        .received_ms = mesh_now_ms(),
        .blocks = blocks_for(size),
    };
    if (s_upload.id) {
        store_remove(s_upload.id);  // una subida anterior que quedo colgada
    }
    ESP_RETURN_ON_ERROR(store_add_blocks(&msg, &s_upload.id), TAG, "no room for file");
    store_set_file_info(s_upload.id, size, name && *name ? name : "archivo", mime && *mime ? mime : "application/octet-stream");
    s_upload.index = 0;
    s_upload.fill = 0;
    *out_id = s_upload.id;
    return ESP_OK;
}

esp_err_t messages_file_write(uint32_t id, const void *data, size_t len)
{
    ESP_RETURN_ON_FALSE(id && id == s_upload.id, ESP_ERR_INVALID_STATE, TAG, "not uploading %" PRIu32, id);
    const uint8_t *p = data;
    while (len > 0) {
        const size_t n = len < MESSAGES_FILE_CHUNK - s_upload.fill ? len : MESSAGES_FILE_CHUNK - s_upload.fill;
        memcpy(s_upload.block.data + s_upload.fill, p, n);
        s_upload.fill += n;
        p += n;
        len -= n;
        if (s_upload.fill == MESSAGES_FILE_CHUNK) {
            ESP_RETURN_ON_ERROR(store_block_put(id, s_upload.index, &s_upload.block, NULL), TAG, "file evicted");
            s_upload.index++;
            s_upload.fill = 0;
        }
    }
    return ESP_OK;
}

esp_err_t messages_file_end(uint32_t id, bool ok)
{
    ESP_RETURN_ON_FALSE(id && id == s_upload.id, ESP_ERR_INVALID_STATE, TAG, "not uploading %" PRIu32, id);
    s_upload.id = 0;
    if (ok && s_upload.fill > 0) {
        memset(s_upload.block.data + s_upload.fill, 0, MESSAGES_FILE_CHUNK - s_upload.fill);
        ok = store_block_put(id, s_upload.index++, &s_upload.block, NULL) == ESP_OK;
    }
    message_t msg;
    if (!ok || !store_get(id, &msg) || msg.blocks_received != msg.blocks) {
        store_remove(id);
        return ok ? ESP_ERR_INVALID_SIZE : ESP_FAIL;
    }
    ESP_LOGI(TAG, "sending file %" PRIu32 " \"%s\" (%" PRIu32 " bytes, %u chunks)", id, msg.text, msg.file_size,
             msg.blocks);
    messages_notify(MESSAGES_EVENT_NEW, id);
    sender_post(id, 0, msg.blocks, true);
    return ESP_OK;
}

esp_err_t messages_file_read(uint32_t id, uint16_t index, uint8_t *out, size_t *len)
{
    message_t msg;
    ESP_RETURN_ON_FALSE(store_get(id, &msg) && msg.kind == MESSAGE_FILE, ESP_ERR_NOT_FOUND, TAG, "no file");
    ESP_RETURN_ON_FALSE(index < msg.blocks, ESP_ERR_INVALID_ARG, TAG, "chunk out of range");
    block_t block;
    ESP_RETURN_ON_ERROR(store_block_get(id, index, &block), TAG, "chunk read failed");
    ESP_RETURN_ON_FALSE(block.present, ESP_ERR_INVALID_STATE, TAG, "chunk missing");
    const uint32_t offset = (uint32_t)index * MESSAGES_FILE_CHUNK;
    const uint32_t left = msg.file_size > offset ? msg.file_size - offset : 0;
    *len = left < MESSAGES_FILE_CHUNK ? left : MESSAGES_FILE_CHUNK;
    memcpy(out, block.data, *len);
    return ESP_OK;
}
