#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#include "neighbors.h"

#define MAX_NEIGHBORS 16
#define TIMEOUT_MS    (CONFIG_SIGSEV_MESH_NEIGHBOR_TIMEOUT_S * 1000u)

static mesh_neighbor_t s_table[MAX_NEIGHBORS];
static int s_count;
static SemaphoreHandle_t s_lock;

static bool is_alive(const mesh_neighbor_t *n, uint32_t now)
{
    return now - n->last_seen_ms < TIMEOUT_MS;
}

esp_err_t neighbors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void neighbors_heard(mesh_node_id_t id, const uint8_t addr[6], int8_t rssi)
{
    const uint32_t now = mesh_now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    mesh_neighbor_t *slot = NULL;
    for (int i = 0; i < s_count; i++) {
        if (s_table[i].id == id) {
            slot = &s_table[i];
            // Promedio movil: el RSSI de un frame suelto salta varios dB.
            slot->rssi = (int8_t)((slot->rssi * 3 + rssi) / 4);
            break;
        }
    }
    if (!slot) {
        // Lugar libre, o el vecino que hace mas que no escuchamos.
        if (s_count < MAX_NEIGHBORS) {
            slot = &s_table[s_count++];
        } else {
            slot = &s_table[0];
            for (int i = 1; i < s_count; i++) {
                if (s_table[i].last_seen_ms < slot->last_seen_ms) {
                    slot = &s_table[i];
                }
            }
        }
        slot->id = id;
        memcpy(slot->addr, addr, sizeof(slot->addr));
        slot->rssi = rssi;
    }
    slot->last_seen_ms = now;

    xSemaphoreGive(s_lock);
}

size_t neighbors_copy(mesh_neighbor_t *out, size_t max)
{
    const uint32_t now = mesh_now_ms();
    size_t n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && n < max; i++) {
        if (is_alive(&s_table[i], now)) {
            out[n++] = s_table[i];
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}
