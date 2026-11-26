idf_component_register(
    SRCS "app_main.c" "app_state.c" "serial_console.c" "wifi_uploader.c" "msx_runner.cpp" "msx_overlay.c"
         "display_sync.c" "display_bench.c" "msx_video_scale.c" "msx_video_diff.c" "watch_controls.c" "watch_theme.c" "rom_filename.c" "fonts/msx_ascii_32.c" "fonts/msx_ascii_20.c" "fonts/msx_clock_64.c" "fonts/msx_japanese_32.c"
         "controller.c" "esp_hid_gap.c"
    INCLUDE_DIRS "."
    REQUIRES esp32_s3_touch_amoled_1_8 pcf85063a mbedtls retros3_msx esp_hid nvs_flash bt
             esp_wifi esp_http_server esp_netif esp_event)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
if(TARGET ${LVGL_LIB})
    target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes)
endif()
