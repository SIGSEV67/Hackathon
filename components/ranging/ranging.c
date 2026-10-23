#include <math.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "ranging.h"

static const char *TAG = "ranging";

#define MAX_PEERS       16
#define EVERY_MS        2500   // una medicion por vez, rotando entre vecinos
#define SESSION_WAIT_MS 4000
#define FRESH_MS        30000  // una medicion FTM vale esto; despues, RSSI
#define RETRY_FAILED_MS 60000  // vecino que no contesta FTM: reintentar cada tanto
#define FAILS_TO_GIVE_UP 3

typedef struct {
    mesh_node_id_t id;
    float dist_m;  // FTM, promedio movil
    uint32_t ok_ms;
    uint32_t fail_ms;
    uint8_t fails;
} peer_t;

static peer_t s_peers[MAX_PEERS];
static SemaphoreHandle_t s_lock;

#if CONFIG_SIGSEV_RADIO_FTM
static SemaphoreHandle_t s_done;
static wifi_event_ftm_report_t s_report;
#endif

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

// Con el lock tomado.
static peer_t *peer_of(mesh_node_id_t id, bool create)
{
    peer_t *oldest = &s_peers[0];
    for (int i = 0; i < MAX_PEERS; i++) {
        if (s_peers[i].id == id) {
            return &s_peers[i];
        }
        const uint32_t last = s_peers[i].ok_ms > s_peers[i].fail_ms ? s_peers[i].ok_ms : s_peers[i].fail_ms;
        const uint32_t oldest_last = oldest->ok_ms > oldest->fail_ms ? oldest->ok_ms : oldest->fail_ms;
        if (s_peers[i].id == 0 || last < oldest_last) {
            oldest = &s_peers[i];
        }
    }
    if (!create) {
        return NULL;
    }
    *oldest = (peer_t){.id = id};
    return oldest;
}

// Modelo log-distancia: RSSI(d) = RSSI(1 m) - 10 n log10(d).
static float rssi_to_m(int8_t rssi)
{
    const float n = CONFIG_SIGSEV_RANGING_PATH_LOSS_X10 / 10.0f;
    return powf(10.0f, (CONFIG_SIGSEV_RANGING_RSSI_AT_1M - rssi) / (10.0f * n));
}

#if CONFIG_SIGSEV_RADIO_FTM

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_report = *(const wifi_event_ftm_report_t *)data;
    xSemaphoreGive(s_done);
}

static void measure(const mesh_neighbor_t *n)
{
    wifi_ftm_initiator_cfg_t cfg = {
        .channel = CONFIG_SIGSEV_RADIO_CHANNEL,
        .frm_count = 16,
        .burst_period = 2,
    };
    memcpy(cfg.resp_mac, n->addr, sizeof(cfg.resp_mac));
    xSemaphoreTake(s_done, 0);
    const uint32_t now = mesh_now_ms();
    bool ok = false;
    float dist_m = 0;
    if (esp_wifi_ftm_initiate_session(&cfg) == ESP_OK) {
        if (xSemaphoreTake(s_done, pdMS_TO_TICKS(SESSION_WAIT_MS)) == pdTRUE) {
            ok = s_report.status == FTM_STATUS_SUCCESS;
            dist_m = ((int32_t)s_report.dist_est - CONFIG_SIGSEV_RANGING_FTM_OFFSET_CM) / 100.0f;
            esp_wifi_ftm_get_report(NULL, 0);  // libera el reporte detallado, no lo usamos
        } else {
            esp_wifi_ftm_end_session();
        }
    }

    LOCK();
    peer_t *p = peer_of(n->id, true);
    if (ok) {
        dist_m = dist_m < 0 ? 0 : dist_m;
        p->dist_m = p->ok_ms ? p->dist_m * 0.6f + dist_m * 0.4f : dist_m;
        p->ok_ms = now;
        p->fails = 0;
    } else {
        p->fail_ms = now;
        p->fails = p->fails < UINT8_MAX ? p->fails + 1 : p->fails;
    }
    UNLOCK();
    if (ok) {
        ESP_LOGD(TAG, "%08" PRIx32 ": %.2f m (FTM)", n->id, dist_m);
    } else {
        ESP_LOGD(TAG, "%08" PRIx32 ": FTM failed (status %d)", n->id, s_report.status);
    }
}

static bool worth_measuring(mesh_node_id_t id, uint32_t now)
{
    LOCK();
    const peer_t *p = peer_of(id, false);
    const bool worth = !p || p->fails < FAILS_TO_GIVE_UP || now - p->fail_ms > RETRY_FAILED_MS;
    UNLOCK();
    return worth;
}

static void ranging_task(void *arg)
{
    size_t next = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(EVERY_MS));
        mesh_neighbor_t neighbors[MAX_PEERS];
        const size_t n = mesh_neighbors(neighbors, MAX_PEERS);
        // Probar a lo sumo una vuelta para encontrar a quien medir.
        for (size_t tries = 0; tries < n; tries++) {
            const mesh_neighbor_t *nb = &neighbors[next++ % n];
            if (worth_measuring(nb->id, mesh_now_ms())) {
                measure(nb);
                break;
            }
        }
    }
}

#endif  // CONFIG_SIGSEV_RADIO_FTM

esp_err_t ranging_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "no mem");
#if CONFIG_SIGSEV_RADIO_FTM
    s_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_done, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_FTM_REPORT, on_wifi, NULL), TAG,
                        "register failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(ranging_task, "ranging", 3072, NULL, 2, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    ESP_LOGI(TAG, "measuring neighbors with FTM");
#else
    ESP_LOGI(TAG, "FTM disabled: distances from RSSI only");
#endif
    return ESP_OK;
}

bool ranging_get(mesh_node_id_t id, ranging_t *out)
{
    mesh_neighbor_t neighbors[MAX_PEERS];
    const size_t n = mesh_neighbors(neighbors, MAX_PEERS);
    const mesh_neighbor_t *nb = NULL;
    for (size_t i = 0; i < n && !nb; i++) {
        nb = neighbors[i].id == id ? &neighbors[i] : NULL;
    }
    if (!nb) {
        return false;
    }
    const uint32_t now = mesh_now_ms();
    LOCK();
    const peer_t *p = peer_of(id, false);
    const bool fresh = p && p->ok_ms && now - p->ok_ms < FRESH_MS;
    const float ftm_m = fresh ? p->dist_m : 0;
    UNLOCK();

    const float m = fresh ? ftm_m : rssi_to_m(nb->rssi);
    const float dm = m * 10.0f;
    out->dist_dm = dm > UINT16_MAX ? UINT16_MAX : (uint16_t)(dm + 0.5f);
    out->method = fresh ? RANGING_FTM : RANGING_RSSI;
    return true;
}
