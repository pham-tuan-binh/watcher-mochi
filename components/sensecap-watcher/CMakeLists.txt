
set(requires "esp_mm" "esp_psram" "esp_lcd" "fatfs" "esp_timer" "driver" "spiffs" "lvgl")
# set(requires )

idf_component_register(
    SRCS "sensecap-watcher.c"
    INCLUDE_DIRS "include"
    # PRIV_REQUIRES ${priv_requires}
    REQUIRES ${requires}
)
