#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "audio.h"
#include "buttons.h"
#include "display.h"
#include "link.h"
#include "mesh.h"
#include "noise.h"
#include "radio.h"

static const char *TAG = "app";

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
    char name[16];
    mesh_node_name(mesh_self_id(), name, sizeof(name));
    ESP_ERROR_CHECK(radio_init(name));
    ESP_ERROR_CHECK(mesh_start());

    ESP_LOGI(TAG, "%s up", name);
}
