#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

typedef struct { int x, y, width, height; } msx_overlay_region_t;
typedef enum {
    MSX_OVERLAY_NONE,
    MSX_OVERLAY_UNCHANGED,
    MSX_OVERLAY_PARTIAL,
    MSX_OVERLAY_FULL,
} msx_overlay_result_t;

/* Called only while holding the LVGL lock. Output is the existing 342x384
 * portrait game buffer. Cursor movement and scrolling return up to two small
 * portrait-coordinate dirty regions; page/content changes return FULL. */
msx_overlay_result_t msx_overlay_render(lv_obj_t *screen, uint16_t *portrait,
                                        msx_overlay_region_t regions[2], int *count);
void msx_overlay_reset(void);

void msx_overlay_levels(lv_obj_t *screen, uint16_t *portrait);

bool msx_overlay_can_stream(void);
bool msx_overlay_is_drawn(void);
