idf_component_register(
    SRCS "main.cpp" "core.cpp" "record.cpp" "storage.cpp" "config.cpp" "services.cpp" "hardware.cpp" "network.cpp" "web.cpp" "ble.cpp" "mqtt.cpp"
    INCLUDE_DIRS "../include" "../private"
    REQUIRES stcc4 littlefs driver esp_wifi esp_netif esp_http_server esp_http_client
             esp_https_ota app_update nvs_flash json mbedtls esp_timer lwip esp_app_format esp_adc bt mqtt
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
idf_component_get_property(json_library json COMPONENT_LIB)
target_compile_definitions(${json_library} PRIVATE CJSON_NESTING_LIMIT=16)
