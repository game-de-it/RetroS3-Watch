#include "app_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static app_state_t s_state = APP_BOOT;

void app_state_init(void)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state = APP_BOOT;
    portEXIT_CRITICAL(&s_state_lock);
}

app_state_t app_state_get(void)
{
    app_state_t state;
    portENTER_CRITICAL(&s_state_lock);
    state = s_state;
    portEXIT_CRITICAL(&s_state_lock);
    return state;
}

void app_state_set(app_state_t state)
{
    portENTER_CRITICAL(&s_state_lock);
    s_state = state;
    portEXIT_CRITICAL(&s_state_lock);
}

const char *app_state_name(app_state_t state)
{
    switch (state) {
    case APP_BOOT: return "BOOT";
    case APP_CLOCK: return "CLOCK";
    case APP_LAUNCHER: return "LAUNCHER";
    case APP_ROM_BROWSER: return "ROM_BROWSER";
    case APP_WIFI_UPLOAD: return "WIFI_UPLOAD";
    case APP_MSX: return "MSX";
    case APP_EMU_MENU: return "EMU_MENU";
    case APP_SETTINGS: return "SETTINGS";
    case APP_POWER_MENU: return "POWER_MENU";
    case APP_DISPLAY_TEST: return "DISPLAY_TEST";
    case APP_DISPLAY_BENCH: return "DISPLAY_BENCH";
    default: return "UNKNOWN";
    }
}
