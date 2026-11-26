#pragma once

#include "esp_attr.h"

/* Preserve LVGL's full 64 KiB object pool for the watch overlays while
 * placing its backing array in PSRAM. */
#define LV_ATTRIBUTE_LARGE_RAM_ARRAY EXT_RAM_BSS_ATTR
