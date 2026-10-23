#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include "dns.h"

static const char *TAG = "dns";

#define DNS_PORT     53
#define DNS_MAX_LEN  512
#define DNS_TYPE_A   1
#define DNS_CLASS_IN 1
#define DNS_TTL_S    30

typedef struct __attribute__((packed)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_header_t;

typedef struct __attribute__((packed)) {
    uint16_t name_ptr;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t rdlength;
    uint32_t addr;
} dns_answer_t;

static uint32_t s_ip;

// Devuelve el largo de la consulta hasta el final de la primera pregunta, o 0
// si esta mal formada. Deja en *qtype el tipo pedido.
static size_t question_end(const uint8_t *msg, size_t len, uint16_t *qtype)
{
    size_t pos = sizeof(dns_header_t);
    while (pos < len && msg[pos]) {
        if (msg[pos] & 0xc0) {
            return 0;  // las preguntas no traen punteros de compresion
        }
        pos += msg[pos] + 1;
    }
    pos++;  // label vacio final
    if (pos + 4 > len) {
        return 0;
    }
    *qtype = (msg[pos] << 8) | msg[pos + 1];
    return pos + 4;
}

static int build_reply(uint8_t *msg, size_t len)
{
    if (len < sizeof(dns_header_t)) {
        return -1;
    }
    dns_header_t *h = (dns_header_t *)msg;
    if ((ntohs(h->flags) & 0x8000) || ntohs(h->qdcount) < 1) {
        return -1;  // no es una consulta
    }
    uint16_t qtype;
    const size_t qend = question_end(msg, len, &qtype);
    if (!qend) {
        return -1;
    }

    // Respuesta autoritativa con solo la primera pregunta.
    h->flags = htons(0x8400 | 0x0100 | 0x0080);  // QR, AA, RD, RA
    h->qdcount = htons(1);
    h->nscount = 0;
    h->arcount = 0;
    if (qtype != DNS_TYPE_A) {
        h->ancount = 0;  // ej. AAAA: sin respuesta, el telefono cae a IPv4
        return qend;
    }
    if (qend + sizeof(dns_answer_t) > DNS_MAX_LEN) {
        return -1;
    }
    h->ancount = htons(1);
    const dns_answer_t ans = {
        .name_ptr = htons(0xc000 | sizeof(dns_header_t)),
        .type = htons(DNS_TYPE_A),
        .class = htons(DNS_CLASS_IN),
        .ttl = htonl(DNS_TTL_S),
        .rdlength = htons(4),
        .addr = s_ip,
    };
    memcpy(msg + qend, &ans, sizeof(ans));
    return qend + sizeof(ans);
}

static void dns_task(void *arg)
{
    const int sock = (int)(intptr_t)arg;
    uint8_t buf[DNS_MAX_LEN];
    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;
        }
        const int reply_len = build_reply(buf, n);
        if (reply_len > 0) {
            sendto(sock, buf, reply_len, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

esp_err_t dns_start(uint32_t ip)
{
    s_ip = ip;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    ESP_RETURN_ON_FALSE(sock >= 0, ESP_FAIL, TAG, "socket failed");

    const struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (const struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        ESP_LOGE(TAG, "bind failed");
        return ESP_FAIL;
    }
    if (xTaskCreate(dns_task, "dns", 3072, (void *)(intptr_t)sock, 3, NULL) != pdPASS) {
        close(sock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "captive DNS up");
    return ESP_OK;
}
