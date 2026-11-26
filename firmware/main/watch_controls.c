#include "watch_controls.h"
#include <stdbool.h>
#include <stdatomic.h>
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"

static atomic_uint s_buttons;
static atomic_int s_volume = 100, s_brightness = 70;
static atomic_uint s_changed_at, s_notice;
static unsigned s_saved;
static const char *TAG = "watch_controls";
static unsigned packed(void) { return (unsigned)watch_controls_volume() | ((unsigned)watch_controls_brightness() << 8); }
int watch_controls_volume(void) { return atomic_load(&s_volume); }
int watch_controls_brightness(void) { return atomic_load(&s_brightness); }
void watch_controls_init(void)
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open("watch_ui", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u16(h, "levels", &v) == ESP_OK && (v & 255) <= 100 &&
            (v >> 8) >= 10 && (v >> 8) <= 100) {
            atomic_store(&s_volume, v & 255); atomic_store(&s_brightness, v >> 8);
        }
        nvs_close(h);
    }
    s_saved = packed();
    ESP_LOGI(TAG, "loaded volume=%d brightness=%d", watch_controls_volume(), watch_controls_brightness());
}
uint32_t watch_controls_filter(uint32_t buttons)
{
    static uint32_t swallowed;
    atomic_store(&s_buttons, buttons);
    /* Keep a consumed direction blocked until it is physically released,
     * including when L2 is released before the direction. */
    swallowed &= buttons & 15U;
    if (buttons & WATCH_BUTTON_L2) swallowed |= buttons & 15U;
    return buttons & ~(WATCH_BUTTON_L2 | swallowed);
}
void watch_controls_adjust(int kind, int delta)
{
    atomic_int *target = kind == 1 ? &s_volume : &s_brightness;
    int previous = atomic_load(target), next;
    do {
        next = previous + delta;
        int minimum = kind == 1 ? 0 : 10;
        if (next < minimum) next = minimum;
        if (next > 100) next = 100;
    } while (!atomic_compare_exchange_weak(target, &previous, next));
    atomic_store(&s_notice, kind);
    atomic_store(&s_changed_at, (uint32_t)(esp_timer_get_time()/1000));
    ESP_LOGI(TAG, "%s=%d", kind == 1 ? "volume" : "brightness", next);
}
int watch_controls_notice(uint32_t now, int *value)
{
    unsigned kind = atomic_load(&s_notice);
    if (!kind || now - atomic_load(&s_changed_at) >= 1500) return 0;
    *value = kind == 1 ? watch_controls_volume() : watch_controls_brightness();
    return (int)kind;
}
void watch_controls_tick(uint32_t now)
{
    static unsigned previous;
    static uint32_t repeat_at;
    unsigned buttons = atomic_load(&s_buttons);
    unsigned directions = (buttons & WATCH_BUTTON_L2) ? buttons & 15U : 0;
    if (directions && (directions != previous || (int32_t)(now-repeat_at) >= 0)) {
        if ((directions & 3U) == 1U) watch_controls_adjust(1, 5);
        if ((directions & 3U) == 2U) watch_controls_adjust(1, -5);
        if ((directions & 12U) == 4U) watch_controls_adjust(2, -10);
        if ((directions & 12U) == 8U) watch_controls_adjust(2, 10);
        repeat_at = now + (directions != previous ? 450 : 150);
    }
    previous = directions;
    unsigned values = packed();
    if (!(buttons & WATCH_BUTTON_L2) && values != s_saved &&
        now - atomic_load(&s_changed_at) >= 1000) {
        nvs_handle_t h;
        esp_err_t result = nvs_open("watch_ui", NVS_READWRITE, &h);
        if (result == ESP_OK) {
            result = nvs_set_u16(h, "levels", values);
            if (result == ESP_OK) result = nvs_commit(h);
            nvs_close(h);
        }
        if (result == ESP_OK) {
            s_saved = values;
            ESP_LOGI(TAG, "saved volume=%u brightness=%u", values & 255, values >> 8);
        } else {
            ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(result));
            atomic_store(&s_changed_at, now); /* Bounded retry, not every tick. */
        }
    }
}
