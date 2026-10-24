#include <ctype.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "internal.h"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#endif

// Comandos de texto por el USB serie de la placa, para disparar cosas desde la
// compu (tools/konami.py). Hoy uno solo: "konami".

static const char *TAG = "serial_cmd";

#define CMD_LEN_MAX 32

static void (*s_on_konami)(void);

static void run_line(char *line)
{
    // Sin espacios ni fin de linea a los costados.
    char *end = line + strlen(line);
    while (end > line && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    while (isspace((unsigned char)*line)) {
        line++;
    }
    if (strcasecmp(line, "konami") == 0) {
        ESP_LOGI(TAG, "konami from the computer");
        s_on_konami();
    } else if (*line) {
        ESP_LOGW(TAG, "unknown command \"%s\"", line);
    }
}

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

static void serial_task(void *arg)
{
    char line[CMD_LEN_MAX + 1];
    size_t len = 0;
    while (1) {
        char c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            run_line(line);
            len = 0;
        } else if (len < CMD_LEN_MAX) {
            line[len++] = c;
        }
    }
}

esp_err_t serial_cmd_init(void (*on_konami)(void))
{
    s_on_konami = on_konami;
    // Para leer hace falta el driver. Con el driver, el log tambien pasa por el:
    // sin una compu conectada descarta lo que no sale en 50 ms, no se traba.
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&cfg), TAG, "driver install failed");
    usb_serial_jtag_vfs_use_driver();
    ESP_RETURN_ON_FALSE(xTaskCreate(serial_task, "serial_cmd", 3072, NULL, 2, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    return ESP_OK;
}

#else

esp_err_t serial_cmd_init(void (*on_konami)(void))
{
    (void)run_line;
    ESP_LOGW(TAG, "console is not USB serial/JTAG: no commands from the computer");
    return ESP_OK;
}

#endif
