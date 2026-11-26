#include "msx_overlay.h"
#include "msx_watch_ui.h"
#include "watch_theme.h"
#include "watch_controls.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define WIDTH 384
#define HEIGHT 342
enum { MENU_LEFT = 4, MENU_TOP = 44, MENU_PITCH = 50, MENU_W = 376, MENU_H = 46,
       MENU_TEXT_INSET = 8, MENU_TEXT_W = MENU_W - 2 * MENU_TEXT_INSET };
LV_FONT_DECLARE(msx_ascii_20);
LV_FONT_DECLARE(msx_japanese_32);
static lv_obj_t *s_canvas;
static msx_watch_ui_t s_previous;
static bool s_valid;
static lv_obj_t *s_level_canvas;
static uint16_t *s_level_pixels;
static lv_obj_t *s_key_canvas;
static lv_obj_t *s_menu_canvas;
static lv_font_t s_menu_mixed_font;
static int s_menu_scroll_limit, s_menu_offset;
static int64_t s_menu_scroll_started;
static EXT_RAM_BSS_ATTR uint16_t s_menu_pixels[MENU_W * MENU_H];
static EXT_RAM_BSS_ATTR uint16_t s_key_pixels[92 * 45];
static EXT_RAM_BSS_ATTR uint8_t s_rotate_seen[(WIDTH * HEIGHT + 7) / 8];

static const char *s_keys[2][6][12] = {{
    {"Esc","F1","F2","F3","F4","F5","Home","Ins","Del","Stp","Sel","Caps"},
    {"1","2","3","4","5","6","7","8","9","0","-","^"},
    {"Tab","Q","W","E","R","T","Y","U","I","O","P","BS"},
    {"Ctrl","A","S","D","F","G","H","J","K","L",";","Ent"},
    {"Shf","Z","X","C","V","B","N","M",",",".","/","Up"},
    {"Grf","@","[","]","SP","\\","_",":","Left","Down","Right","Cnt"}},
   {{"Esc","F1","F2","F3","F4","F5","Home","Ins","Del","Stp","Sel","Caps"},
    {"!","\"","#","$","%","&","'","(",")","0","=","~"},
    {"Tab","Q","W","E","R","T","Y","U","I","O","P","BS"},
    {"Ctrl","A","S","D","F","G","H","J","K","L","+","Ent"},
    {"Shf","Z","X","C","V","B","N","M","<",">","?","Up"},
    {"Grf","`","{","}","SP","|","_","*","Left","Down","Right","Cnt"}}};

void msx_overlay_reset(void)
{
    /* Parent cleanup owns the hidden canvas. */
    s_canvas = NULL;
    s_level_canvas = NULL;
    s_key_canvas = NULL;
    s_menu_canvas = NULL;
    s_valid = false;
}

static void text(lv_layer_t *layer, const char *value, int x, int y,
                  int width, int height, uint32_t color, bool centered)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = watch_font();
    dsc.color = lv_color_hex(color);
    char display[128];
    strlcpy(display, value, sizeof(display));
    watch_upper_ascii(display);
    if (!centered) watch_fit_text(display, sizeof(display), width);
    dsc.text = display;
    dsc.text_local = 1;
    dsc.letter_space = centered ? -4 : 0;
    dsc.flag = LV_TEXT_FLAG_EXPAND;
    dsc.align = centered ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
    lv_area_t area = {x, y, x + width - 1, y + height - 1};
    layer->_clip_area = area;
    lv_draw_label(layer, &dsc, &area);
}

static void key_text(lv_layer_t *layer, const char *value, int x, int y,
                     int width, int height, uint32_t color)
{
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    char display[16];
    strlcpy(display, value, sizeof(display));
    watch_upper_ascii(display);
    const size_t length = strlen(display);
    dsc.font = length >= 4 ? &msx_ascii_20 : watch_font();
    dsc.color = lv_color_hex(color);
    dsc.text = display;
    dsc.text_local = 1;
    dsc.letter_space = length >= 4 ? -3 : -4;
    dsc.flag = LV_TEXT_FLAG_EXPAND;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    const int text_y = y + (height - dsc.font->line_height) / 2;
    lv_area_t area = {x, text_y, x + width - 1, text_y + dsc.font->line_height - 1};
    layer->_clip_area = area;
    lv_draw_label(layer, &dsc, &area);
}

static void rectangle(lv_layer_t *layer, int x, int y, int width, int height,
                       uint32_t color)
{
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(color);
    dsc.bg_opa = LV_OPA_COVER;
    dsc.radius = 0;
    dsc.border_width = 1;
    dsc.border_color = lv_color_hex(watch_theme_palette()->border);
    lv_area_t area = {x, y, x + width - 1, y + height - 1};
    layer->_clip_area = area;
    lv_draw_rect(layer, &dsc, &area);
}

static int menu_row(lv_layer_t *layer, const char *item, int x, int y, bool selected, int offset)
{
    const watch_palette_t *p = watch_theme_palette();
    char label[sizeof(((msx_watch_ui_t *)0)->items[0])];
    strlcpy(label, item, sizeof(label));
    const bool quit = strcmp(item, "Quit") == 0;
    watch_upper_ascii(label);
    rectangle(layer, x, y, MENU_W, MENU_H,
              selected ? (quit ? 0xdb6559 : p->selected) : p->background);

    const lv_font_t *font = watch_font();
    lv_point_t size;
    lv_text_get_size(&size, label, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    if (size.x > MENU_TEXT_W) {
        bool mixed = false;
        for (const unsigned char *c = (const unsigned char *)label; *c; ++c)
            if (*c >= 128) { mixed = true; break; }
        if (mixed && !s_menu_mixed_font.fallback) {
            s_menu_mixed_font = msx_ascii_20;
            s_menu_mixed_font.fallback = &msx_japanese_32;
            s_menu_mixed_font.line_height = 36;
            s_menu_mixed_font.base_line = 4;
        }
        font = mixed ? &s_menu_mixed_font : &msx_ascii_20;
        lv_text_get_size(&size, label, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_EXPAND);
    }

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = lv_color_hex(selected ? (quit ? 0x000000 : p->selected_text) :
                                       (quit ? p->error : p->text));
    dsc.text = label;
    dsc.text_local = 1;
    dsc.flag = LV_TEXT_FLAG_EXPAND;
    dsc.ofs_x = -offset;
    const int top = y + (MENU_H - font->line_height) / 2;
    const int left = x + MENU_TEXT_INSET;
    lv_area_t area = {left, top, left + MENU_TEXT_W - 1, top + font->line_height - 1};
    layer->_clip_area = area;
    lv_draw_label(layer, &dsc, &area);
    return size.x > MENU_TEXT_W ? size.x - MENU_TEXT_W : 0;
}

static void menu_scroll_reset(int limit)
{
    s_menu_scroll_limit = limit;
    s_menu_offset = 0;
    s_menu_scroll_started = esp_timer_get_time();
}

static int menu_scroll_position(void)
{
    enum { START_PAUSE_US = 750000, END_PAUSE_US = 1500000, STEP_US = 40000, STEP_PIXELS = 3 };
    int travel_us = ((s_menu_scroll_limit + STEP_PIXELS - 1) / STEP_PIXELS) * STEP_US;
    int elapsed = (esp_timer_get_time() - s_menu_scroll_started) %
                  (START_PAUSE_US + travel_us + END_PAUSE_US);
    if (elapsed < START_PAUSE_US) return 0;
    int offset = (elapsed - START_PAUSE_US) / STEP_US * STEP_PIXELS;
    return offset < s_menu_scroll_limit ? offset : s_menu_scroll_limit;
}

static void menu(const msx_watch_ui_t *state)
{
    const watch_palette_t *p = watch_theme_palette();
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    text(&layer, state->title, 10, 2, 364, 40, p->heading, false);
    lv_canvas_finish_layer(s_canvas, &layer);
    int limit = 0;
    for (int i = 0; i < MSX_WATCH_UI_ROWS && state->first + i < state->total; ++i) {
        bool selected = state->selected == state->first + i;
        lv_canvas_init_layer(s_canvas, &layer);
        int overflow = menu_row(&layer, state->items[i], MENU_LEFT, MENU_TOP + i * MENU_PITCH, selected, 0);
        if (selected) limit = overflow;
        lv_canvas_finish_layer(s_canvas, &layer);
    }
    menu_scroll_reset(limit);
    lv_canvas_init_layer(s_canvas, &layer);
    text(&layer, "A:OK B:BACK", 8, 302, 368, 40, p->muted, false);
    lv_canvas_finish_layer(s_canvas, &layer);
}

static int menu_redraw_row(uint16_t *portrait, const msx_watch_ui_t *state, int row,
                           bool selected, int offset, msx_overlay_region_t *region)
{
    if (!s_menu_canvas) {
        s_menu_canvas = lv_canvas_create(lv_obj_get_parent(s_canvas));
        lv_obj_set_hidden(s_menu_canvas, true);
        lv_canvas_set_buffer(s_menu_canvas, s_menu_pixels, MENU_W, MENU_H, LV_COLOR_FORMAT_RGB565);
    }
    lv_layer_t layer;
    lv_canvas_init_layer(s_menu_canvas, &layer);
    const int overflow = menu_row(&layer, state->items[row], 0, 0, selected, offset);
    lv_canvas_finish_layer(s_menu_canvas, &layer);
    const int y = MENU_TOP + row * MENU_PITCH;
    for (int dy = 0; dy < MENU_H; ++dy)
        for (int dx = 0; dx < MENU_W; ++dx)
            portrait[(MENU_LEFT + dx) * HEIGHT + (HEIGHT - 1 - (y + dy))] =
                s_menu_pixels[dy * MENU_W + dx];

    *region = (msx_overlay_region_t){HEIGHT - y - MENU_H, MENU_LEFT, MENU_H, MENU_W};
    return overflow;
}

static void keyboard(const msx_watch_ui_t *state)
{
    const watch_palette_t *p = watch_theme_palette();
    const int shifted = (state->modifiers & MSX_WATCH_UI_SHIFT) != 0;
    const int bank = state->column / 4;
    lv_layer_t layer;
    lv_canvas_init_layer(s_canvas, &layer);
    char title[40];
    snprintf(title, sizeof(title), "KEYS %d/3%s%s%s", bank + 1,
             shifted ? " SHIFT" : "", (state->modifiers & MSX_WATCH_UI_CTRL) ? " CTRL" : "",
             (state->modifiers & MSX_WATCH_UI_GRAPH) ? " GRAPH" : "");
    text(&layer, title, 8, 2, 368, 40, p->heading, false);
    lv_canvas_finish_layer(s_canvas, &layer);
    for (int row = 0; row < 6; ++row) {
        lv_canvas_init_layer(s_canvas, &layer);
        for (int column = 0; column < 4; ++column) {
            const int key_column = bank * 4 + column;
            const bool selected = state->row == row && state->column == key_column;
            rectangle(&layer, column * 96 + 2, row * 49 + 46, 92, 45,
                      selected ? 0xded087 : p->background);
            key_text(&layer, s_keys[shifted][row][key_column], column * 96 + 3, row * 49 + 50,
                     90, 39, selected ? 0x000000 : p->text);
        }
        lv_canvas_finish_layer(s_canvas, &layer);
    }
}

static void keyboard_key(uint16_t *portrait, const msx_watch_ui_t *state,
                         int row, int column, bool selected,
                         msx_overlay_region_t *region)
{
    if (!s_key_canvas) {
        s_key_canvas = lv_canvas_create(lv_obj_get_parent(s_canvas));
        lv_obj_set_hidden(s_key_canvas, true);
        lv_canvas_set_buffer(s_key_canvas, s_key_pixels, 92, 45, LV_COLOR_FORMAT_RGB565);
    }
    const watch_palette_t *p = watch_theme_palette();
    lv_layer_t layer;
    lv_canvas_init_layer(s_key_canvas, &layer);
    rectangle(&layer, 0, 0, 92, 45, selected ? 0xded087 : p->background);
    const int shifted = (state->modifiers & MSX_WATCH_UI_SHIFT) != 0;
    key_text(&layer, s_keys[shifted][row][column], 1, 4, 90, 39,
             selected ? 0x000000 : p->text);
    lv_canvas_finish_layer(s_key_canvas, &layer);

    const int x = (column % 4) * 96 + 2;
    const int y = row * 49 + 46;
    for (int dy = 0; dy < 45; ++dy)
        for (int dx = 0; dx < 92; ++dx)
            portrait[(x + dx) * HEIGHT + (HEIGHT - 1 - (y + dy))] =
                s_key_pixels[dy * 92 + dx];

    int x0 = (HEIGHT - y - 45) & ~1;
    int x1 = (HEIGHT - y + 1) & ~1;
    int y0 = x & ~1;
    int y1 = (x + 92 + 1) & ~1;
    *region = (msx_overlay_region_t){x0, y0, x1 - x0, y1 - y0};
}

msx_overlay_result_t msx_overlay_render(lv_obj_t *canvas, uint16_t *portrait,
                                        msx_overlay_region_t regions[2], int *count)
{
    msx_watch_ui_t state;
    *count = 0;
    msx_watch_ui_read(&state);
    if (state.type == MSX_WATCH_UI_NONE) { s_valid = false; return MSX_OVERLAY_NONE; }
    if (s_valid && memcmp(&state, &s_previous, sizeof(state)) == 0) {
        if (state.type == MSX_WATCH_UI_MENU && s_menu_scroll_limit) {
            const int offset = menu_scroll_position();
            if (offset != s_menu_offset) {
                const int row = state.selected - state.first;
                menu_redraw_row(portrait, &state, row, true, offset, &regions[0]);
                s_menu_offset = offset;
                *count = 1;
                return MSX_OVERLAY_PARTIAL;
            }
        }
        return MSX_OVERLAY_UNCHANGED;
    }
    if (s_valid && state.type == MSX_WATCH_UI_MENU &&
        s_previous.type == MSX_WATCH_UI_MENU &&
        state.selected != s_previous.selected &&
        state.first == s_previous.first && state.total == s_previous.total &&
        strcmp(state.title, s_previous.title) == 0 &&
        memcmp(state.items, s_previous.items, sizeof(state.items)) == 0) {
        const int old_row = s_previous.selected - state.first;
        const int new_row = state.selected - state.first;
        if (old_row >= 0 && old_row < MSX_WATCH_UI_ROWS &&
            new_row >= 0 && new_row < MSX_WATCH_UI_ROWS) {
            msx_overlay_region_t second;
            menu_redraw_row(portrait, &state, old_row, false, 0, &regions[0]);
            menu_scroll_reset(menu_redraw_row(portrait, &state, new_row, true, 0, &second));
            const int x0 = regions[0].x < second.x ? regions[0].x : second.x;
            const int y0 = regions[0].y < second.y ? regions[0].y : second.y;
            const int x1a = regions[0].x + regions[0].width;
            const int x1b = second.x + second.width;
            const int y1a = regions[0].y + regions[0].height;
            const int y1b = second.y + second.height;
            const int x1 = x1a > x1b ? x1a : x1b;
            const int y1 = y1a > y1b ? y1a : y1b;
            regions[0] = (msx_overlay_region_t){x0, y0, x1 - x0, y1 - y0};
            *count = 1;
            s_previous = state;
            return MSX_OVERLAY_PARTIAL;
        }
    }
    if (s_valid && state.type == MSX_WATCH_UI_KEYBOARD &&
        s_previous.type == MSX_WATCH_UI_KEYBOARD &&
        state.modifiers == s_previous.modifiers &&
        state.column / 4 == s_previous.column / 4 &&
        (state.row != s_previous.row || state.column != s_previous.column)) {
        keyboard_key(portrait, &state, s_previous.row, s_previous.column, false, &regions[0]);
        keyboard_key(portrait, &state, state.row, state.column, true, &regions[1]);
        *count = 2;
        s_previous = state;
        return MSX_OVERLAY_PARTIAL;
    }
    /* Draw into the existing portrait game buffer while treating it as a
     * landscape canvas, then rotate the same storage in place. This saves a
     * second full-frame allocation while the emulator is running. */
    s_canvas = canvas;
    lv_canvas_set_buffer(s_canvas, portrait, WIDTH, HEIGHT, LV_COLOR_FORMAT_RGB565);
    lv_canvas_fill_bg(s_canvas, lv_color_hex(watch_theme_palette()->background), LV_OPA_COVER);
    if (state.type == MSX_WATCH_UI_MENU) menu(&state);
    else keyboard(&state);
    /* Same portrait panel and game rectangle, but no 1.5x bitmap scaling:
     * the actual UI glyphs are 32 pixels high in landscape coordinates. */
    memset(s_rotate_seen, 0, sizeof(s_rotate_seen));
    const size_t pixels = WIDTH * HEIGHT;
    for (size_t start = 0; start < pixels; ++start) {
        if (s_rotate_seen[start >> 3] & (1U << (start & 7))) continue;
        size_t current = start;
        uint16_t carried = portrait[current];
        do {
            const size_t row = current / WIDTH;
            const size_t column = current - row * WIDTH;
            const size_t next = column * HEIGHT + (HEIGHT - 1 - row);
            const uint16_t displaced = portrait[next];
            portrait[next] = carried;
            carried = displaced;
            s_rotate_seen[current >> 3] |= 1U << (current & 7);
            current = next;
        } while (current != start);
    }
    if (!s_valid || state.type != s_previous.type || state.modifiers != s_previous.modifiers ||
        strcmp(state.title,s_previous.title) != 0)
        ESP_LOGI("msx_overlay", "large UI type=%d font=32 selected=%d row=%d col=%d modifiers=%u title=%s",
                 state.type, state.selected, state.row, state.column, state.modifiers, state.title);
    s_previous = state;
    s_valid = true;
    /* Restore the canvas to its portrait display buffer. */
    lv_canvas_set_buffer(canvas, portrait, HEIGHT, WIDTH, LV_COLOR_FORMAT_RGB565);
    return MSX_OVERLAY_FULL;
}

/* A short landscape strip is composited after the game/menu render. It never
 * changes the source framebuffer or uses a competing LCD transfer path. */
void msx_overlay_levels(lv_obj_t *screen, uint16_t *portrait)
{
    int value;
    int kind = watch_controls_notice((uint32_t)(esp_timer_get_time()/1000), &value);
    if (!kind) return;
    if (!s_level_pixels) s_level_pixels = heap_caps_malloc(WIDTH * 64 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_level_pixels) return;
    if (!s_level_canvas) {
        s_level_canvas = lv_canvas_create(screen);
        lv_obj_set_hidden(s_level_canvas, true);
        lv_canvas_set_buffer(s_level_canvas, s_level_pixels, WIDTH, 64, LV_COLOR_FORMAT_RGB565);
    }
    lv_canvas_fill_bg(s_level_canvas, lv_color_hex(watch_theme_palette()->background), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(s_level_canvas, &layer);
    char label[32];
    snprintf(label, sizeof(label), "%s %d%%", kind == 1 ? "VOLUME" : "LIGHT", value);
    text(&layer, label, 12, 2, 360, 36, watch_theme_palette()->heading, false);
    rectangle(&layer, 12, 46, 360, 12, 0x333333);
    if (value > 0) rectangle(&layer, 12, 46, value * 360 / 100, 12, watch_theme_palette()->heading);
    lv_canvas_finish_layer(s_level_canvas, &layer);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < WIDTH; ++x)
            portrait[x * HEIGHT + 63 - y] = s_level_pixels[y * WIDTH + x];
}

bool msx_overlay_can_stream(void)
{
    msx_watch_ui_t state;
    msx_watch_ui_read(&state);
    int value;
    if (state.type != MSX_WATCH_UI_NONE ||
        watch_controls_notice((uint32_t)(esp_timer_get_time()/1000), &value)) return false;
    s_valid = false;
    return true;
}

bool msx_overlay_is_drawn(void)
{
    return s_valid;
}
