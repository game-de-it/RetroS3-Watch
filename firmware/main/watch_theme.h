#pragma once
#include "lvgl.h"
#include "esp_err.h"

typedef enum { WATCH_CARTRIDGE, WATCH_BASIC } watch_theme_id_t;
typedef struct {
    uint32_t background, text, heading, border, selected, selected_text;
    uint32_t muted, good, warning, error;
} watch_palette_t;
void watch_theme_init(void);
esp_err_t watch_theme_set(watch_theme_id_t theme);
watch_theme_id_t watch_theme_get(void);
const watch_palette_t *watch_theme_palette(void);
const lv_font_t *watch_font(void);
const lv_font_t *watch_clock_font(void);
void watch_style_button(lv_obj_t *button, bool selected);
/* Uppercase ASCII only. Unicode names are preserved byte for byte. */
void watch_upper_ascii(char *text);
void watch_fit_text(char *text, size_t capacity, int width);
