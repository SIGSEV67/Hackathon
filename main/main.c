// SIGSEV: red mesh peer-to-peer para emergencias y marchas.
//
// main solo arma las piezas, en orden de dependencia. La logica vive en los
// componentes (components/) y el mapeo de botones en controls.c.

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "audio.h"
#include "board.h"
#include "buttons.h"
#include "call.h"
#include "controls.h"
#include "link.h"
#include "mesh.h"
#include "messages.h"
#include "noise.h"
#include "presence.h"
#include "radio.h"
#include "ui.h"
#include "web.h"

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

    // Hardware
    ESP_ERROR_CHECK(buttons_init());
    ESP_ERROR_CHECK(audio_init());
    ESP_ERROR_CHECK(noise_init());

    // Mesh y servicios encima. La mesh se prepara antes que la radio porque el
    // nombre del nodo (y por lo tanto el SSID) sale de su identidad.
    ESP_ERROR_CHECK(mesh_init(link_espnow()));
    const presence_config_t presence_cfg = {
        .flags = (BOARD_HAS_DISPLAY ? PRESENCE_FLAG_DISPLAY : 0) | (CONFIG_SPIRAM ? PRESENCE_FLAG_PSRAM : 0),
        .noise_level = noise_level,
        .noise_take_peak = noise_take_peak,
    };
    ESP_ERROR_CHECK(presence_init(&presence_cfg));
    ESP_ERROR_CHECK(messages_init());
    ESP_ERROR_CHECK(call_init());

    // Radio: AP para telefonos + ESP-NOW para la mesh, mismo canal.
    char name[PRESENCE_NAME_MAX + 1];
    presence_self_name(name, sizeof(name));
    ESP_ERROR_CHECK(radio_init(name));
    ESP_ERROR_CHECK(mesh_start());

    // Interfaces
    ESP_ERROR_CHECK(web_init());
    const ui_config_t ui_cfg = {
        .key_labels = {controls_key_label(BTN_KEY1), controls_key_label(BTN_KEY2), controls_key_label(BTN_KEY3),
                       controls_key_label(BTN_KEY4)},
    };
    if (ui_init(&ui_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ui init failed, continuing without screen");
    }
    ESP_ERROR_CHECK(controls_init());

    ESP_LOGI(TAG, "%s up: connect to WiFi \"%s\" for the portal", name, name);
    ESP_LOGI(TAG, "internal RAM free %u KB, largest block %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
}
