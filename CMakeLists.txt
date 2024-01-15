idf_component_register(
    SRCS "src/esp_wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi nvs_flash
)