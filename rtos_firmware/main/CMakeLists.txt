idf_component_register(
    SRCS
        "ISMS_MQTT_v8_0_RTOS_PERF.c"
        "isms_web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        freertos
        driver
        esp_wifi
        esp_event
        esp_http_client
        nvs_flash
        esp_timer
        esp_netif
        lwip
        esp_http_server
        spiffs
)

spiffs_create_partition_image(spiffs ../spiffs_image FLASH_IN_PROJECT)
