#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "sdkconfig.h"

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "display.h"
#include "link.h"
#include "mesh.h"
#include "messages.h"
#include "noise.h"
#include "presence.h"
#include "radio.h"

static const char *TAG = "app";

#ifndef CONFIG_SPIRAM
#define CONFIG_SPIRAM 0
#endif

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(noise_init());

    ESP_ERROR_CHECK(mesh_init(link_espnow()));
    const presence_config_t presence_cfg = {
        .flags = (BOARD_HAS_DISPLAY ? PRESENCE_FLAG_DISPLAY : 0) | (CONFIG_SPIRAM ? PRESENCE_FLAG_PSRAM : 0),
        .noise_level = noise_level,
        .noise_take_peak = noise_take_peak,
    };
    ESP_ERROR_CHECK(presence_init(&presence_cfg));
    ESP_ERROR_CHECK(messages_init());

    char name[PRESENCE_NAME_MAX + 1];
    presence_self_name(name, sizeof(name));
    ESP_ERROR_CHECK(radio_init(name));
    ESP_ERROR_CHECK(mesh_start());

    ESP_LOGI(TAG, "%s up", name);
}
