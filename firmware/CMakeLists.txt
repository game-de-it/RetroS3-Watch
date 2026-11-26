cmake_minimum_required(VERSION 3.16)

# LVGL's custom attribute header is consumed by LVGL and every component that
# includes lvgl.h, so expose the project-owned header globally.
include_directories("${CMAKE_CURRENT_LIST_DIR}/main")

set(EXTRA_COMPONENT_DIRS
    "../components/usb_recovery"
    "../components/esp32_s3_touch_amoled_1_8")

set(PROJECT_VER "0.1.1")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(retros3_watch)

# ESP-IDF diagnostics can retain absolute source paths in the firmware image.
# Map the current builder's home directory to a neutral prefix so release
# binaries do not disclose local usernames or directory layouts.
idf_build_set_property(COMPILE_OPTIONS
    "-ffile-prefix-map=$ENV{HOME}=/build-home"
    "-fmacro-prefix-map=$ENV{HOME}=/build-home"
    APPEND)
