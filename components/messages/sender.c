#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "internal.h"

// Task que manda rangos de bloques de una voz: reenvios del courier y
// respuestas a pedidos de bloques perdidos. Los manda espaciados para dejarles
// aire a los reenvios de los vecinos.

static const char *TAG = "sender";

#define SEND_SPACING_MS 5
#define QUEUE_LEN       16

typedef struct {
    uint32_t id;
    uint16_t start;
    uint16_t count;
} job_t;

static QueueHandle_t s_jobs;

static void run(const job_t *job)
{
    message_t msg;
    if (!store_get(job->id, &msg) || msg.kind != MESSAGE_VOICE) {
        return;
    }
    const uint32_t end = (uint32_t)job->start + job->count;
    uint16_t sent = 0;
    for (uint32_t i = job->start; i < end && i < msg.blocks; i++) {
        block_t block;
        if (store_block_get(msg.id, i, &block) != ESP_OK) {
            return;  // se borro
        }
        if (block.present && voice_send_block(&msg, i, &block) == ESP_OK) {
            sent++;
            vTaskDelay(pdMS_TO_TICKS(SEND_SPACING_MS));
        }
    }
    ESP_LOGD(TAG, "%" PRIu32 ": sent %u blocks from %u", msg.id, sent, job->start);
    if (sent) {
        courier_sent(msg.id);
    }
}

static void sender_task(void *arg)
{
    while (1) {
        job_t job;
        xQueueReceive(s_jobs, &job, portMAX_DELAY);
        run(&job);
    }
}

esp_err_t sender_init(void)
{
    s_jobs = xQueueCreate(QUEUE_LEN, sizeof(job_t));
    ESP_RETURN_ON_FALSE(s_jobs, ESP_ERR_NO_MEM, TAG, "no mem");
    ESP_RETURN_ON_FALSE(xTaskCreate(sender_task, "sender", 3072, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG,
                        "task create failed");
    return ESP_OK;
}

void sender_post(uint32_t id, uint16_t start, uint16_t count)
{
    const job_t job = {.id = id, .start = start, .count = count};
    if (xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropped job for %" PRIu32, id);
    }
}
