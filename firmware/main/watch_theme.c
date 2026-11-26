#include "watch_theme.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

LV_FONT_DECLARE(msx_ascii_32);
LV_FONT_DECLARE(msx_clock_64);
LV_FONT_DECLARE(msx_japanese_32);
static lv_font_t s_font;
static watch_theme_id_t s_theme;
static const watch_palette_t palettes[] = {
    {0x000000,0xffffff,0x65ebef,0x5955e0,0x5955e0,0xffffff,0xcccccc,0x74d07d,0xded087,0xdb6559},
    {0x5955e0,0xffffff,0xffffff,0x8076f1,0x65ebef,0x000000,0xcccccc,0x65ebef,0xded087,0xded087}
};

void watch_theme_init(void)
{
    s_font = msx_ascii_32;
    s_font.fallback = &msx_japanese_32;
    /* Shared baseline: keep full 32px Japanese glyphs within a 36px line. */
    s_font.line_height = 36;
    s_font.base_line = 4;
    nvs_handle_t handle;
    uint8_t value = 0;
    if (nvs_open("watch_ui", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "theme", &value);
        nvs_close(handle);
    }
    s_theme = value == WATCH_BASIC ? WATCH_BASIC : WATCH_CARTRIDGE;
    ESP_LOGI("watch_theme", "theme=%s; C-BIOS 8x8 at 32px + Japanese fallback",
             s_theme == WATCH_BASIC ? "BASIC" : "CARTRIDGE");
}

esp_err_t watch_theme_set(watch_theme_id_t theme)
{
    if (theme != WATCH_CARTRIDGE && theme != WATCH_BASIC) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t result = nvs_open("watch_ui", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_u8(handle, "theme", theme);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    if (result == ESP_OK) {
        s_theme = theme;
        ESP_LOGI("watch_theme", "saved theme=%s", theme == WATCH_BASIC ? "BASIC" : "CARTRIDGE");
    }
    return result;
}
watch_theme_id_t watch_theme_get(void) { return s_theme; }
const watch_palette_t *watch_theme_palette(void) { return &palettes[s_theme]; }
const lv_font_t *watch_font(void) { return &s_font; }
const lv_font_t *watch_clock_font(void) { return &msx_clock_64; }

void watch_style_button(lv_obj_t *button, bool selected)
{
    const watch_palette_t *p = watch_theme_palette();
    lv_obj_set_style_radius(button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(selected ? p->selected : p->background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(selected ? p->heading : p->border), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 2, LV_PART_MAIN);
    lv_obj_set_style_border_side(button, s_theme == WATCH_BASIC ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_FULL, LV_PART_MAIN);
    lv_obj_set_style_text_color(button, lv_color_hex(selected ? p->selected_text : p->text), LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(p->selected), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_hex(p->selected_text), LV_PART_MAIN | LV_STATE_PRESSED);
}
void watch_upper_ascii(char *text)
{
    for (; *text; ++text) if (*text >= 'a' && *text <= 'z') *text -= 'a' - 'A';
}
void watch_fit_text(char *text, size_t capacity, int width)
{
    lv_point_t size;
    lv_text_get_size(&size, text, watch_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (size.x <= width) return;
    size_t length = strlen(text);
    while (length) {
        do { --length; } while (length && ((unsigned char)text[length] & 0xc0) == 0x80);
        text[length] = 0;
        lv_text_get_size(&size, text, watch_font(), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (size.x + 96 <= width && length + 4 <= capacity) break;
    }
    if (length + 4 <= capacity) memcpy(text + length, "...", 4);
}
