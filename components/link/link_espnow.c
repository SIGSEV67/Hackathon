#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "link.h"

static const char *TAG = "link_espnow";

#define SEND_TIMEOUT_MS 50

static const uint8_t s_broadcast[ESP_NOW_ETH_ALEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static link_rx_cb_t s_on_rx;
static void *s_rx_ctx;
static SemaphoreHandle_t s_sent;

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    link_rx_info_t rx = {.rssi = info->rx_ctrl->rssi};
    memcpy(rx.src, info->src_addr, LINK_ADDR_LEN);
    s_on_rx(&rx, data, len, s_rx_ctx);
}

// En broadcast no hay ACK: el status solo dice si el frame salio al aire.
static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    xSemaphoreGive(s_sent);
}

static esp_err_t start(link_rx_cb_t on_rx, void *ctx)
{
    ESP_RETURN_ON_FALSE(on_rx, ESP_ERR_INVALID_ARG, TAG, "null rx callback");
    s_on_rx = on_rx;
    s_rx_ctx = ctx;
    s_sent = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_sent, ESP_ERR_NO_MEM, TAG, "no mem for semaphore");

    ESP_RETURN_ON_ERROR(esp_now_init(), TAG, "esp_now_init failed");
    ESP_RETURN_ON_ERROR(esp_now_register_recv_cb(on_recv), TAG, "register recv failed");
    ESP_RETURN_ON_ERROR(esp_now_register_send_cb(on_sent), TAG, "register send failed");

    esp_now_peer_info_t peer = {
        .channel = 0,  // el canal actual del AP
        .ifidx = WIFI_IF_AP,
        .encrypt = false,
    };
    memcpy(peer.peer_addr, s_broadcast, ESP_NOW_ETH_ALEN);
    ESP_RETURN_ON_ERROR(esp_now_add_peer(&peer), TAG, "add broadcast peer failed");

    ESP_LOGI(TAG, "ESP-NOW link up (mtu %d)", ESP_NOW_MAX_DATA_LEN);
    return ESP_OK;
}

static esp_err_t broadcast(const uint8_t *data, size_t len)
{
    ESP_RETURN_ON_FALSE(len <= ESP_NOW_MAX_DATA_LEN, ESP_ERR_INVALID_SIZE, TAG, "frame too big");
    xSemaphoreTake(s_sent, 0);
    ESP_RETURN_ON_ERROR(esp_now_send(s_broadcast, data, len), TAG, "send failed");
    return xSemaphoreTake(s_sent, pdMS_TO_TICKS(SEND_TIMEOUT_MS)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void self_addr(uint8_t out[LINK_ADDR_LEN])
{
    esp_read_mac(out, ESP_MAC_WIFI_SOFTAP);
}

static const link_t s_link = {
    .name = "espnow",
    .mtu = ESP_NOW_MAX_DATA_LEN,
    .start = start,
    .broadcast = broadcast,
    .self_addr = self_addr,
};

const link_t *link_espnow(void)
{
    return &s_link;
}
