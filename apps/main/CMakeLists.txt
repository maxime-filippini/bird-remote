idf_component_register(
    SRCS "main.c" "bird_remote_client.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 esp_http_client esp_http_server esp_wifi nvs_flash esp_netif
)
