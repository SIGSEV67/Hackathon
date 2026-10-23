idf_component_register(
    SRCS "web.c" "api.c" "dns.c" "json_out.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_http_server esp_netif lwip mesh presence messages
    EMBED_TXTFILES "www/index.html" "www/app.js" "www/style.css"
)
