set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

idf_component_register(
    SRCS
        "main.cpp"
        "${PROJECT_ROOT}/storage/storage.cpp"
        "${PROJECT_ROOT}/configuration/Configuration.cpp"
        "${PROJECT_ROOT}/wifi_manager/wifi_manager.cpp"
        "${PROJECT_ROOT}/config_portal/config_portal.cpp"
        "${PROJECT_ROOT}/mqtt/mqtt.cpp"
    INCLUDE_DIRS
        "."
        "${PROJECT_ROOT}"
        "${PROJECT_ROOT}/wifi_manager"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt esp_system log
    EMBED_TXTFILES
        "${PROJECT_ROOT}/certyficate/isrgrootx1.pem"
)
