#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "radio.h"

static const char *TAG = "radio";

esp_err_t radio_init(const char *ssid)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_FALSE(esp_netif_create_default_wifi_ap(), ESP_FAIL, TAG, "ap netif failed");

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode failed");

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = CONFIG_SIGSEV_RADIO_CHANNEL,
            .max_connection = CONFIG_SIGSEV_RADIO_MAX_CLIENTS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strlcpy((char *)ap_cfg.ap.ssid, ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set config failed");

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set ps failed");

    ESP_LOGI(TAG, "AP \"%s\" up on channel %d", ssid, CONFIG_SIGSEV_RADIO_CHANNEL);
    return ESP_OK;
}
