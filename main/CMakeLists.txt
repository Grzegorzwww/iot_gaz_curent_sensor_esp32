set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

idf_component_register(
    SRCS
        "main.cpp"
        "${PROJECT_ROOT}/storage/storage.cpp"
        "${PROJECT_ROOT}/configuration/Configuration.cpp"
        "${PROJECT_ROOT}/wifi_manager/wifi_manager.cpp"
        "${PROJECT_ROOT}/config_portal/config_portal.cpp"
        "${PROJECT_ROOT}/mqtt/mqtt.cpp"
        "${PROJECT_ROOT}/energy_meter/energy_meter.cpp"
        "${PROJECT_ROOT}/energy_meter/consumption_history.cpp"
        "${PROJECT_ROOT}/energy_meter/lp_counter.cpp"
    INCLUDE_DIRS
        "."
        "${PROJECT_ROOT}"
        "${PROJECT_ROOT}/wifi_manager"
    REQUIRES
        nvs_flash esp_wifi esp_netif esp_event esp_http_server mqtt esp_system log
        esp_driver_gpio esp_timer hal soc ulp esp_hw_support
    EMBED_TXTFILES
        "${PROJECT_ROOT}/certyficate/isrgrootx1.pem"
        "${PROJECT_ROOT}/config_portal/jsQR.min.js"
)

# Program rdzenia LP (liczenie impulsow w trybie bateryjnym). Zmienne z lp_core/main.c sa dostepne
# po stronie HP jako ulp_<nazwa> przez wygenerowany naglowek ulp_main.h.
set(ulp_app_name ulp_${COMPONENT_NAME})
set(ulp_sources "${PROJECT_ROOT}/energy_meter/lp_core/main.c")
set(ulp_exp_dep_srcs "${PROJECT_ROOT}/energy_meter/lp_counter.cpp")
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
