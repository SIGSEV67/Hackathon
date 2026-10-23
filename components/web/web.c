#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"

#include "api.h"
#include "dns.h"
#include "web.h"

static const char *TAG = "web";

// Archivos de www/, embebidos en el firmware (EMBED_TXTFILES les agrega un \0).
#define EMBEDDED(name)                                                    \
    extern const char _binary_##name##_start[] asm("_binary_" #name "_start"); \
    extern const char _binary_##name##_end[] asm("_binary_" #name "_end")

EMBEDDED(index_html);
EMBEDDED(app_js);
EMBEDDED(style_css);

typedef struct {
    const char *start;
    const char *end;
    const char *type;
} asset_t;

static const asset_t s_index = {_binary_index_html_start, _binary_index_html_end, "text/html; charset=utf-8"};
static const asset_t s_app = {_binary_app_js_start, _binary_app_js_end, "text/javascript; charset=utf-8"};
static const asset_t s_style = {_binary_style_css_start, _binary_style_css_end, "text/css; charset=utf-8"};

static char s_portal_url[32];

static esp_err_t get_asset(httpd_req_t *req)
{
    const asset_t *a = req->user_ctx;
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, a->start, a->end - a->start - 1);
}

// Cualquier otra URL (incluidos los chequeos de conectividad de Android, iOS y
// Windows) redirige al portal: asi el telefono muestra "Iniciar sesion en la red".
static esp_err_t redirect_to_portal(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_portal_url);
    return httpd_resp_send(req, NULL, 0);
}

// Opcion DHCP 114 (RFC 8910): Android 11+ e iOS 14+ abren el portal directo.
static void announce_portal_via_dhcp(esp_netif_t *ap)
{
    esp_netif_dhcps_stop(ap);
    if (esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_url,
                               strlen(s_portal_url)) != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive portal option not set");
    }
    esp_netif_dhcps_start(ap);
}

esp_err_t web_init(void)
{
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    ESP_RETURN_ON_FALSE(ap, ESP_ERR_INVALID_STATE, TAG, "AP netif not found (radio_init first)");
    esp_netif_ip_info_t ip;
    ESP_RETURN_ON_ERROR(esp_netif_get_ip_info(ap, &ip), TAG, "no AP ip");
    snprintf(s_portal_url, sizeof(s_portal_url), "http://" IPSTR "/", IP2STR(&ip.ip));
    announce_portal_via_dhcp(ap);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;  // los telefonos abren muchas conexiones y no las cierran
    httpd_handle_t server;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &cfg), TAG, "httpd start failed");

    const httpd_uri_t assets[] = {
        {.uri = "/", .method = HTTP_GET, .handler = get_asset, .user_ctx = (void *)&s_index},
        {.uri = "/app.js", .method = HTTP_GET, .handler = get_asset, .user_ctx = (void *)&s_app},
        {.uri = "/style.css", .method = HTTP_GET, .handler = get_asset, .user_ctx = (void *)&s_style},
    };
    for (size_t i = 0; i < sizeof(assets) / sizeof(assets[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &assets[i]), TAG, "register %s failed",
                            assets[i].uri);
    }
    ESP_RETURN_ON_ERROR(api_register(server), TAG, "api register failed");
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_to_portal), TAG,
                        "404 handler failed");

    ESP_RETURN_ON_ERROR(dns_start(ip.ip.addr), TAG, "dns start failed");
    ESP_LOGI(TAG, "portal at %s", s_portal_url);
    return ESP_OK;
}
