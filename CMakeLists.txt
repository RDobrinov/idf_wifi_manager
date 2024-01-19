idf_component_register(
    SRCS "src/idf_wifi_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi nvs_flash
)