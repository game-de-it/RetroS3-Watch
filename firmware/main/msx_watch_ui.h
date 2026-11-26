/* SPDX-License-Identifier: MIT */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum { MSX_WATCH_UI_NONE, MSX_WATCH_UI_MENU, MSX_WATCH_UI_KEYBOARD };
enum { MSX_WATCH_UI_SHIFT = 1, MSX_WATCH_UI_CTRL = 2, MSX_WATCH_UI_GRAPH = 4 };
enum { MSX_WATCH_UI_ROWS = 5, MSX_WATCH_UI_ITEM_BYTES = 192 };

typedef struct {
    int type, row, column, first, total, selected;
    unsigned modifiers;
    char title[40];
    char items[MSX_WATCH_UI_ROWS][MSX_WATCH_UI_ITEM_BYTES];
} msx_watch_ui_t;

void msx_watch_ui_read(msx_watch_ui_t *out);

#ifdef __cplusplus
}
#endif
