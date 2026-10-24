#include <string.h>

#include "audio.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mesh.h"
#include "sdkconfig.h"
#include "ui.h"

#include "easter.h"
#include "internal.h"

// La secuencia: el kiwi baila el "6 7" (un tono por tiempo), explota (ruido) y
// la placa se reinicia. La que la empezo avisa por la mesh en el momento de la
// explosion; las demas arrancan su propia secuencia al recibir el aviso. Sin
// display pasa lo mismo, solo que no se ve.

static const char *TAG = "easter";

#define DANCE_BEATS    8
#define BEAT_MS        UI_KIWI_BEAT_MS
#define BEEP_MS        110
#define SIX_HZ         659   // mi
#define SEVEN_HZ       784   // sol
#define BOOM_MS        1800
#define BOOM_SOUND_MS  900
#define RESTART_MS     700
#define COPIES         3     // el broadcast no tiene ACK: mandarlo varias veces
#define COPY_GAP_MS    40
#define KIWI_MAGIC     0x6977696bu  // "kiwi"
#define ROUND_VALID    0x36373637u  // "6767": distingue lo guardado de la basura del encendido

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t round;  // lo sortea la que empieza; las copias de una ronda lo comparten
} wire_kiwi_t;

static TaskHandle_t s_task;
static volatile bool s_active;
static volatile bool s_leader;  // esta placa la empezo: avisa al explotar
static uint32_t s_round;

// La ultima ronda que hizo esta placa, en memoria RTC: sobrevive a esp_restart.
// Al volver, una copia de esa ronda que siga rebotando por la red no la vuelve
// a disparar (y asi para siempre); una ronda nueva si.
static RTC_NOINIT_ATTR uint32_t s_done_round;
static RTC_NOINIT_ATTR uint32_t s_done_valid;

static void boom_sound(void)
{
    // Ruido blanco filtrado (mas grave) que se apaga de a poco.
    static int16_t buf[256];
    const int total = AUDIO_SAMPLE_RATE_HZ * BOOM_SOUND_MS / 1000;
    int32_t low = 0;
    for (int done = 0; done < total;) {
        const int n = total - done < 256 ? total - done : 256;
        for (int i = 0; i < n; i++) {
            const int32_t env = 28000 * (total - done - i) / total;
            const int32_t noise = (int32_t)(esp_random() & 0xffff) - 0x8000;
            low += (noise - low) / 6;
            const int32_t v = low * env / 0x8000 * 3 / 2;  // el filtro baja el nivel: compensar
            buf[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
        }
        if (audio_speaker_write(buf, n, 200) != ESP_OK) {
            return;
        }
        done += n;
    }
}

static void announce(void)
{
    const wire_kiwi_t w = {.magic = KIWI_MAGIC, .round = s_round};
    for (int i = 0; i < COPIES; i++) {
        mesh_send(MESH_TYPE_KIWI, &w, sizeof(w), pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(COPY_GAP_MS));
    }
    ESP_LOGW(TAG, "told the mesh");
}

static void run(void)
{
    ESP_LOGW(TAG, "6 7 (%s)", s_leader ? "started here" : "from the mesh");
    ui_kiwi(UI_KIWI_DANCE);
    TickType_t wake = xTaskGetTickCount();
    for (int beat = 0; beat < DANCE_BEATS; beat++) {
        audio_play_tone(beat % 2 ? SEVEN_HZ : SIX_HZ, BEEP_MS);  // sin parlante vuelve enseguida
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(BEAT_MS));
    }

    ui_kiwi(UI_KIWI_BOOM);
    const TickType_t boom = xTaskGetTickCount();
    if (s_leader) {
        announce();
    }
    if (audio_has_speaker()) {
        boom_sound();
    }
    const TickType_t elapsed = xTaskGetTickCount() - boom;
    if (elapsed < pdMS_TO_TICKS(BOOM_MS)) {
        vTaskDelay(pdMS_TO_TICKS(BOOM_MS) - elapsed);
    }

    ui_kiwi(UI_KIWI_RESTART);
    ESP_LOGW(TAG, "boom, restarting");
    vTaskDelay(pdMS_TO_TICKS(RESTART_MS));
    esp_restart();
}

static void easter_task(void *arg)
{
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    run();
}

static bool already_done(uint32_t round)
{
    return s_done_valid == ROUND_VALID && s_done_round == round;
}

static void start(bool leader, uint32_t round)
{
    if (s_active) {
        return;
    }
    s_active = true;
    s_leader = leader;
    s_round = round;
    s_done_round = round;
    s_done_valid = ROUND_VALID;
    xTaskNotifyGive(s_task);
}

static void on_kiwi(const mesh_rx_meta_t *meta, const void *payload, size_t len, void *ctx)
{
    wire_kiwi_t w;
    if (len < sizeof(w)) {
        return;
    }
    memcpy(&w, payload, sizeof(w));
    if (w.magic != KIWI_MAGIC || already_done(w.round)) {
        return;
    }
    ESP_LOGW(TAG, "kiwi from %08" PRIx32 " (%u hops, round %08" PRIx32 ")", meta->origin, meta->hops, w.round);
    start(false, w.round);
}

void easter_trigger(void)
{
    if (s_task) {
        uint32_t round;
        do {
            round = esp_random();
        } while (already_done(round));
        start(true, round);
    }
}

esp_err_t easter_init(void)
{
#if CONFIG_SIGSEV_EASTER_EGG
    ESP_RETURN_ON_FALSE(xTaskCreate(easter_task, "easter", 3072, NULL, 4, &s_task) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    ESP_RETURN_ON_ERROR(mesh_register_handler(MESH_TYPE_KIWI, on_kiwi, NULL), TAG, "register failed");
    ESP_RETURN_ON_ERROR(konami_init(easter_trigger), TAG, "konami init failed");
    ESP_RETURN_ON_ERROR(serial_cmd_init(easter_trigger), TAG, "serial init failed");
#endif
    return ESP_OK;
}
