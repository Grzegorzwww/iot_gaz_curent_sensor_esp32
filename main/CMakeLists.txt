set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

idf_component_register(
    SRCS
        "main.cpp"
        "${PROJECT_ROOT}/storage/storage.cpp"
        "${PROJECT_ROOT}/configuration/Configuration.cpp"
        "${PROJECT_ROOT}/wifi_manager/wifi_manager.cpp"
        "${PROJECT_ROOT}/config_portal/config_portal.cpp"
        "${PROJECT_ROOT}/mqtt/mqtt.cpp"
        "${PROJECT_ROOT}/bme680/bme680.cpp"
        "${PROJECT_ROOT}/lm35/lm35.cpp"
    INCLUDE_DIRS
        "."
        "${PROJECT_ROOT}"
        "${PROJECT_ROOT}/wifi_manager"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt esp_system log
        esp_driver_gpio esp_driver_i2c esp_timer esp_hw_support esp_adc
    EMBED_TXTFILES
        "${PROJECT_ROOT}/certyficate/isrgrootx1.pem"
        "${PROJECT_ROOT}/config_portal/jsQR.min.js"
)
