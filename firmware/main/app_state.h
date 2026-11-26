#pragma once

typedef enum {
    APP_BOOT,
    APP_CLOCK,
    APP_LAUNCHER,
    APP_ROM_BROWSER,
    APP_WIFI_UPLOAD,
    APP_MSX,
    APP_EMU_MENU,
    APP_SETTINGS,
    APP_POWER_MENU,
    APP_DISPLAY_TEST,
    APP_DISPLAY_BENCH,
} app_state_t;

void app_state_init(void);
app_state_t app_state_get(void);
void app_state_set(app_state_t state);
const char *app_state_name(app_state_t state);
