#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_io_expander.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "app_state.h"
#include "bsp/esp-bsp.h"
#include "controller.h"
#include "lvgl.h"
#include "esp_lvgl_port_disp.h"
#include "pcf85063a.h"
#include "msx_runner.h"
#include "msx_overlay.h"
#include "msx_video_scale.h"
#include "msx_video_diff.h"
#include "display_bench.h"
#include "display_sync.h"
#include "esp_rom_sys.h"
#include "watch_theme.h"
#include "watch_controls.h"
#include "rom_filename.h"
#include "serial_console.h"
#include "wifi_uploader.h"
#include "nvs_flash.h"

#define ROM_DIRECTORY BSP_SD_MOUNT_POINT "/msx/roms"
#define MAX_ROMS 24
#define MAX_ROM_NAME 256
#define UI_LOCK_TIMEOUT_MS 1000
#define MSX_SOURCE_WIDTH MSX_RUNNER_VIDEO_WIDTH
#define MSX_SOURCE_HEIGHT MSX_RUNNER_VIDEO_HEIGHT
#define MSX_LANDSCAPE_WIDTH 384
#define MSX_LANDSCAPE_HEIGHT 342
#define MSX_CANVAS_WIDTH MSX_LANDSCAPE_HEIGHT
#define MSX_CANVAS_HEIGHT MSX_LANDSCAPE_WIDTH
#define MSX_CANVAS_X (((BSP_LCD_H_RES - MSX_CANVAS_WIDTH) / 2) & ~1)
#define MSX_CANVAS_Y ((BSP_LCD_V_RES - MSX_CANVAS_HEIGHT) / 2)
#define MSX_FRAME_WIDTH 2
#define LVGL_DMA_BUFFER_LINES 20

static const char *TAG = "retros3_watch";
static TaskHandle_t s_bench_worker;
static atomic_int s_bench_mode;
static atomic_int s_bench_visible_mode=-1;
static void benchmark_worker(void *argument);
static atomic_int s_video_mode=3; /* 0 Canvas, 1 full stream, 2 buffered, 3 TE + dirty regions */
static atomic_int s_video_phase_us; /* Diagnostic phase relative to fresh TE; runtime only. */
static uint16_t *s_video_previous; /* Owned last successfully presented source, never a recycled queue buffer. */
static bool s_video_previous_valid;
static unsigned s_video_previous_generation;

static pcf85063a_dev_t s_rtc;
static bool s_rtc_ready;
static bool s_sd_ready;
static i2c_master_dev_handle_t s_pmic;
static lv_obj_t *s_clock_date;
static lv_obj_t *s_clock_power;
static lv_timer_t *s_clock_timer;
static lv_obj_t *s_clock_time;
static bool s_display_awake = true;
static lv_obj_t *s_level_labels[2];
static char s_rom_names[MAX_ROMS][MAX_ROM_NAME];
static size_t s_rom_count;
static char s_selected_rom[MAX_ROM_NAME + sizeof(ROM_DIRECTORY) + 2];
static unsigned s_msx_model = 2;
static lv_obj_t *s_msx_canvas;
static uint16_t *s_msx_canvas_buffer;
typedef struct {
    uint16_t *pixels;
    unsigned generation;
} video_frame_t;
static QueueHandle_t s_video_free;
static QueueHandle_t s_video_ready;
static atomic_uint s_video_generation;
static atomic_int s_capture_state; /* 0 idle, 1 requested, 2 copying, 3 ready */
static uint16_t *s_capture_pixels;
typedef struct {
    int16_t samples[640];
    size_t count;
    unsigned generation;
} audio_chunk_t;
static QueueHandle_t s_audio_queue;
static esp_codec_dev_handle_t s_speaker;
static lv_display_t *s_display;
static atomic_int s_wifi_upload_status = ESP_ERR_NOT_FOUND;
static atomic_bool s_wifi_requested;
static TaskHandle_t s_wifi_worker;
static lv_timer_t *s_wifi_ui_timer;

static void wifi_worker(void *argument)
{
    (void)argument;
    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (atomic_load(&s_wifi_requested)) {
            const esp_err_t result = wifi_uploader_start(s_sd_ready);
            atomic_store(&s_wifi_upload_status, result);
        }
        if (!atomic_load(&s_wifi_requested)) {
            wifi_uploader_stop();
        }
    }
}

static lv_obj_t *active_screen(void)
{
#if LVGL_VERSION_MAJOR >= 9
    return lv_screen_active();
#else
    return lv_scr_act();
#endif
}

static uint8_t day_of_week(int year, int month, int day)
{
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (month < 3) {
        --year;
    }
    return (uint8_t)((year + year / 4 - year / 100 + year / 400 +
                      offsets[month - 1] + day) % 7);
}

static bool compile_datetime(pcf85063a_datetime_t *time)
{
    static const char *months[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun",
        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
    };
    char month_name[4] = {0};
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;

    if (sscanf(__DATE__, "%3s %d %d", month_name, &day, &year) != 3 ||
        sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second) != 3) {
        return false;
    }
    for (int i = 0; i < 12; ++i) {
        if (strcmp(month_name, months[i]) == 0) {
            month = i + 1;
            break;
        }
    }
    if (month == 0) {
        return false;
    }
    *time = (pcf85063a_datetime_t) {
        .year = (uint16_t)year,
        .month = (uint8_t)month,
        .day = (uint8_t)day,
        .dotw = day_of_week(year, month, day),
        .hour = (uint8_t)hour,
        .min = (uint8_t)minute,
        .sec = (uint8_t)second,
    };
    return true;
}

static esp_err_t pmic_read(uint8_t reg, uint8_t *value)
{
    if (s_pmic == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_pmic, &reg, 1, value, 1, 100);
}

static void init_pmic(void)
{
    const i2c_device_config_t config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bsp_i2c_get_handle(), &config, &s_pmic);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 unavailable: %s", esp_err_to_name(ret));
        s_pmic = NULL;
        return;
    }
    uint8_t id = 0;
    ret = pmic_read(0x03, &id);
    if (ret != ESP_OK || id != 0x4a) {
        ESP_LOGW(TAG, "AXP2101 identity check failed: ret=%s id=0x%02x",
                 esp_err_to_name(ret), id);
        i2c_master_bus_rm_device(s_pmic);
        s_pmic = NULL;
        return;
    }
    ESP_LOGI(TAG, "AXP2101 ready");
}

static void init_rtc(void)
{
    esp_err_t ret = pcf85063a_init(&s_rtc, bsp_i2c_get_handle(), PCF85063A_ADDRESS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A unavailable: %s", esp_err_to_name(ret));
        return;
    }

    pcf85063a_datetime_t now = {0};
    ret = pcf85063a_get_time_date(&s_rtc, &now);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063A read failed: %s", esp_err_to_name(ret));
        return;
    }
    if (now.year < 2024 || now.year > 2069 || now.month < 1 || now.month > 12 ||
        now.day < 1 || now.day > 31 || now.hour > 23 || now.min > 59 || now.sec > 59) {
        pcf85063a_datetime_t build_time = {0};
        if (!compile_datetime(&build_time)) {
            ESP_LOGW(TAG, "RTC unset and build timestamp unavailable");
            return;
        }
        ret = pcf85063a_set_time_date(&s_rtc, build_time);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "PCF85063A seed failed: %s", esp_err_to_name(ret));
            return;
        }
        ESP_LOGW(TAG, "RTC seeded from firmware build timestamp");
    }
    s_rtc_ready = true;
    ESP_LOGI(TAG, "PCF85063A ready");
}

static void ensure_directory(const char *path)
{
    if (mkdir(path, 0775) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir %s failed: errno=%d", path, errno);
    }
}

static void init_storage(void)
{
    esp_err_t ret = bsp_sdcard_mount();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "microSD unavailable: %s", esp_err_to_name(ret));
        return;
    }
    s_sd_ready = true;
    ensure_directory(BSP_SD_MOUNT_POINT "/msx");
    ensure_directory(ROM_DIRECTORY);
    ensure_directory(BSP_SD_MOUNT_POINT "/msx/disks");
    ensure_directory(BSP_SD_MOUNT_POINT "/msx/bios");
    ensure_directory(BSP_SD_MOUNT_POINT "/msx/saves");
    ensure_directory(BSP_SD_MOUNT_POINT "/msx/states");
    ensure_directory(BSP_SD_MOUNT_POINT "/config");
    ESP_LOGI(TAG, "microSD mounted; ROM directory=%s", ROM_DIRECTORY);
}

static void init_audio(void)
{
    s_speaker = bsp_audio_codec_speaker_init();
    if (s_speaker == NULL) {
        ESP_LOGW(TAG, "ES8311 speaker unavailable");
        return;
    }
    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = 32000,
        .mclk_multiple = 256,
    };
    int result = esp_codec_dev_open(s_speaker, &format);
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8311 open failed: %d", result);
        s_speaker = NULL;
        return;
    }
    result = esp_codec_dev_set_out_vol(s_speaker, watch_controls_volume());
    if (result != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "ES8311 volume failed: %d", result);
    } else {
        ESP_LOGI(TAG, "ES8311 ready: 32000 Hz mono, volume=%d", watch_controls_volume());
    }
}

static void style_screen(lv_obj_t *screen)
{
    lv_obj_set_style_text_font(screen, watch_font(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(screen, lv_color_hex(watch_theme_palette()->background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen, lv_color_hex(watch_theme_palette()->text), LV_PART_MAIN);
}

static lv_obj_t *make_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    char title[64];
    strlcpy(title, text, sizeof(title));
    watch_upper_ascii(title);
    if (watch_theme_get() == WATCH_BASIC && strcmp(title, "RETROS3") != 0) {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "> %.60s", title);
        strlcpy(title, prompt, sizeof(title));
    }
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, watch_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(watch_theme_palette()->heading), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 22, 20);
    static const uint32_t colors[] = {0x5955e0,0x65ebef,0xb766b5,0xdb6559,0xded087,0x74d07d};
    for (int i = 0; i < 6; ++i) {
        lv_obj_t *stripe = lv_obj_create(parent);
        lv_obj_remove_style_all(stripe);
        lv_obj_set_pos(stripe, 22 + i * 54, 63);
        lv_obj_set_size(stripe, 54, watch_theme_get() == WATCH_BASIC ? 2 : 4);
        lv_obj_set_style_bg_color(stripe, lv_color_hex(watch_theme_get() == WATCH_BASIC ? 0xffffff : colors[i]), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(stripe, LV_OPA_COVER, LV_PART_MAIN);
    }
    return label;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int y,
                             lv_event_cb_t callback, void *user_data)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 324, 60);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, y);
    watch_style_button(button, false);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, watch_font(), LV_PART_MAIN);
    lv_obj_set_width(label, 296);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

static void render_state_locked(app_state_t state);

static void set_state_locked(app_state_t state)
{
    const app_state_t previous = app_state_get();
    /* Owning LVGL proves the benchmark is outside its transfer section.
     * Delete its sleeping/waiting worker before another MSX stack is allocated. */
    if(previous==APP_DISPLAY_BENCH && state!=APP_DISPLAY_BENCH && s_bench_worker) {
        display_sync_forget_task(s_bench_worker);
        vTaskDelete(s_bench_worker); s_bench_worker=NULL;
    }
    if (previous == APP_MSX && state != APP_MSX) {
        s_msx_canvas = NULL;
        msx_runner_stop();
    }
    if (previous == APP_WIFI_UPLOAD && state != APP_WIFI_UPLOAD) {
        atomic_store(&s_wifi_requested, false);
        wifi_uploader_cancel();
        if (s_wifi_worker != NULL) xTaskNotifyGive(s_wifi_worker);
    }
    if (previous != APP_WIFI_UPLOAD && state == APP_WIFI_UPLOAD) {
        atomic_store(&s_wifi_upload_status, ESP_ERR_NOT_FINISHED);
        atomic_store(&s_wifi_requested, true);
        if (s_wifi_worker != NULL) xTaskNotifyGive(s_wifi_worker);
    }
    app_state_set(state);
    ESP_LOGI(TAG, "state: %s -> %s", app_state_name(previous), app_state_name(state));
    ESP_LOGI(TAG, "DMA heap free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
    render_state_locked(state);
}

static void request_state(app_state_t state)
{
    if (!bsp_display_lock(UI_LOCK_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "UI lock timeout requesting %s", app_state_name(state));
        return;
    }
    set_state_locked(state);
    bsp_display_unlock();
}

static void state_button_cb(lv_event_t *event)
{
    const app_state_t state = (app_state_t)(intptr_t)lv_event_get_user_data(event);
    set_state_locked(state);
}

/* USB diagnostic entry; console validates the basename before calling. */
esp_err_t app_console_launch_rom(const char *name, bool msx1)
{
    char path[MAX_ROM_NAME + sizeof(ROM_DIRECTORY) + 2];
    struct stat info;
    snprintf(path, sizeof(path), "%s/%s", ROM_DIRECTORY, name);
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) return ESP_ERR_NOT_FOUND;
    if (!s_sd_ready || msx_runner_is_running() ||
        !bsp_display_lock(UI_LOCK_TIMEOUT_MS)) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_selected_rom, path, sizeof(s_selected_rom));
    s_msx_model = msx1 ? 1 : 2;
    set_state_locked(APP_MSX);
    bsp_display_unlock();
    return ESP_OK;
}

static void clock_screen_cb(lv_event_t *event)
{
    (void)event;
    set_state_locked(APP_LAUNCHER);
}

static void update_clock(lv_timer_t *timer)
{
    (void)timer;
    if (!s_clock_time || !s_clock_date || !s_clock_power) return;
    char text[64];
    pcf85063a_datetime_t now = {0};
    if (s_rtc_ready && pcf85063a_get_time_date(&s_rtc, &now) == ESP_OK &&
        now.month >= 1 && now.month <= 12 && now.day >= 1 && now.day <= 31) {
        snprintf(text, sizeof(text), "%02u:%02u", (unsigned)now.hour, (unsigned)now.min);
        lv_label_set_text(s_clock_time, text);
        static const char *days[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
        snprintf(text, sizeof(text), "%02u/%02u %s", (unsigned)now.month, (unsigned)now.day,
                 days[day_of_week(now.year, now.month, now.day)]);
        lv_label_set_text(s_clock_date, text);
    } else {
        lv_label_set_text(s_clock_time, "--:--");
        lv_label_set_text(s_clock_date, "RTC ERROR");
    }
    uint8_t status = 0, percent = 0;
    if (pmic_read(0x00, &status) != ESP_OK) lv_label_set_text(s_clock_power, "POWER --");
    else if ((status & (1U << 3)) && pmic_read(0xa4, &percent) == ESP_OK && percent <= 100) {
        snprintf(text, sizeof(text), "BAT %u%%%s", (unsigned)percent,
                 (status & (1U << 5)) ? "+USB" : "");
        lv_label_set_text(s_clock_power, text);
    } else lv_label_set_text(s_clock_power, (status & (1U << 5)) ? "USB POWER" : "NO BATTERY");
}

static void render_clock(lv_obj_t *screen)
{
    lv_obj_set_clickable(screen, true);
    lv_obj_add_event_cb(screen, clock_screen_cb, LV_EVENT_CLICKED, NULL);
    make_title(screen, "RETROS3");
    s_clock_date = lv_label_create(screen);
    lv_obj_set_pos(s_clock_date, 22, 102);
    lv_obj_set_style_text_color(s_clock_date, lv_color_hex(watch_theme_palette()->muted), LV_PART_MAIN);
    s_clock_time = lv_label_create(screen);
    lv_obj_set_style_text_font(s_clock_time, watch_clock_font(), LV_PART_MAIN);
    lv_obj_set_width(s_clock_time, 332);
    lv_obj_set_pos(s_clock_time, 18, 170);
    lv_obj_set_style_text_align(s_clock_time, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_clock_time, lv_color_hex(watch_theme_get() == WATCH_BASIC ? 0xded087 : 0xffffff), LV_PART_MAIN);
    s_clock_power = lv_label_create(screen);
    lv_obj_set_pos(s_clock_power, 22, 306);
    lv_obj_set_width(s_clock_power, 324);
    lv_label_set_long_mode(s_clock_power, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_clock_power, lv_color_hex(watch_theme_palette()->good), LV_PART_MAIN);
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "BOOT:MENU");
    lv_obj_set_style_text_color(hint, lv_color_hex(watch_theme_palette()->muted), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
    update_clock(NULL);
    s_clock_timer = lv_timer_create(update_clock, 1000, NULL);
}

static lv_obj_t *make_touch_list(lv_obj_t *screen)
{
    lv_obj_t *list = lv_obj_create(screen);
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 12, 78);
    lv_obj_set_size(list, 344, 300);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(watch_theme_palette()->background), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, lv_color_hex(watch_theme_palette()->heading), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    return list;
}

static void render_launcher(lv_obj_t *screen)
{
    make_title(screen, "Launcher");
    lv_obj_t *list = make_touch_list(screen);
    const char *names[] = {"CLOCK", "MSX", "WI-FI\nUPLOAD", "DISPLAY\nTEST", "60FPS\nTEST", "SETTINGS"};
    const app_state_t states[] = {APP_CLOCK, APP_ROM_BROWSER, APP_WIFI_UPLOAD, APP_DISPLAY_TEST, APP_DISPLAY_BENCH, APP_SETTINGS};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); ++i) {
        lv_obj_t *button = make_button(list, names[i], 0, state_button_cb,
                                       (void *)(intptr_t)states[i]);
        lv_obj_set_size(button, 324, 100);
        watch_style_button(button, i == 1);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
    }

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "BOOT:CLOCK");
    lv_obj_set_style_text_color(hint, lv_color_hex(watch_theme_palette()->muted), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
}

void app_console_launcher(void)
{
    request_state(APP_LAUNCHER);
}

static void theme_button_cb(lv_event_t *event)
{
    const watch_theme_id_t theme = (watch_theme_id_t)(intptr_t)lv_event_get_user_data(event);
    const esp_err_t result = watch_theme_set(theme);
    if (result == ESP_OK) set_state_locked(APP_SETTINGS);
    else ESP_LOGE(TAG, "theme save: %s", esp_err_to_name(result));
}

static void level_button_cb(lv_event_t *event)
{
    int action = (int)(intptr_t)lv_event_get_user_data(event);
    watch_controls_adjust(abs(action), (action > 0 ? 1 : -1) * (abs(action) == 1 ? 5 : 10));
}

static void update_level_labels(void)
{
    if (s_level_labels[0]) lv_label_set_text_fmt(s_level_labels[0], "VOLUME %d%%", watch_controls_volume());
    if (s_level_labels[1]) lv_label_set_text_fmt(s_level_labels[1], "LIGHT %d%%", watch_controls_brightness());
}

static void render_settings(lv_obj_t *screen)
{
    make_title(screen, "SETTINGS");
    lv_obj_t *list = make_touch_list(screen);
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 320, 156);
        lv_obj_set_scrollable(row, false);
        s_level_labels[i] = lv_label_create(row);
        lv_obj_set_width(s_level_labels[i], 320);
        lv_obj_set_style_text_letter_space(s_level_labels[i], -4, LV_PART_MAIN);
        lv_obj_set_style_text_align(s_level_labels[i], LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        for (int sign = -1; sign <= 1; sign += 2) {
            lv_obj_t *button = make_button(row, sign < 0 ? "-" : "+", 0,
                level_button_cb, (void *)(intptr_t)(sign * (i + 1)));
            lv_obj_set_size(button, 144, 96);
            lv_obj_set_align(button, LV_ALIGN_TOP_LEFT);
            lv_obj_set_width(lv_obj_get_child(button, 0), 120);
            lv_obj_set_pos(button, sign < 0 ? 0 : 176, 52);
        }
    }
    update_level_labels();
    const char *names[] = {"CARTRIDGE\nBLACK", "BASIC\nBLUE"};
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *button = make_button(list, names[i], 0, theme_button_cb, (void *)(intptr_t)i);
        lv_obj_set_height(button, 112);
        watch_style_button(button, watch_theme_get() == i);
    }
    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "BOOT:HOME");
    lv_obj_set_style_text_color(hint, lv_color_hex(watch_theme_palette()->muted), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);
}

esp_err_t app_console_theme(bool basic)
{
    if (!bsp_display_lock(UI_LOCK_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
    esp_err_t result = watch_theme_set(basic ? WATCH_BASIC : WATCH_CARTRIDGE);
    if (result == ESP_OK) set_state_locked(APP_SETTINGS);
    bsp_display_unlock();
    return result;
}

void app_console_rom_browser(void) { request_state(APP_ROM_BROWSER); }
void app_console_wifi_upload(void) { request_state(APP_WIFI_UPLOAD); }
void app_console_font_check(unsigned codepoint)
{
    if (!bsp_display_lock(UI_LOCK_TIMEOUT_MS)) return;
    lv_font_glyph_dsc_t glyph;
    bool found = lv_font_get_glyph_dsc(watch_font(), &glyph, codepoint, 0);
    printf("FONT %s U+%04X box=%ux%u\n", found && !glyph.is_placeholder ? "OK" : "MISSING",
           codepoint, found ? (unsigned)glyph.box_w : 0, found ? (unsigned)glyph.box_h : 0);
    fflush(stdout);
    bsp_display_unlock();
}

static void refresh_wifi_upload(lv_timer_t *timer)
{
    static int previous_status = ESP_ERR_NOT_FINISHED;
    (void)timer;
    const int status = atomic_load(&s_wifi_upload_status);
    if (status != previous_status && app_state_get() == APP_WIFI_UPLOAD) {
        previous_status = status;
        render_state_locked(APP_WIFI_UPLOAD);
    }
}

static void render_wifi_upload(lv_obj_t *screen)
{
    make_title(screen, "UPLOAD");
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_size(panel, 344, 248);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_bg_opa(panel, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 8, LV_PART_MAIN);
    lv_obj_t *message = lv_label_create(panel);
    lv_obj_set_width(message, 320);
    lv_obj_set_style_text_font(message, watch_font(), LV_PART_MAIN);
    lv_obj_set_style_text_color(message, lv_color_hex(watch_theme_palette()->text), LV_PART_MAIN);
    lv_label_set_long_mode(message, LV_LABEL_LONG_WRAP);
    lv_obj_align(message, LV_ALIGN_TOP_LEFT, 0, 0);

    char text[320];
    if (atomic_load(&s_wifi_upload_status) == ESP_ERR_NOT_FINISHED) {
        snprintf(text, sizeof(text), "Connecting...\n\nPlease wait.\nBACK: Cancel");
    } else if (s_wifi_upload_status == ESP_OK && wifi_uploader_is_running()) {
        snprintf(text, sizeof(text),
                 "Connected:\n%s\n\nUpload URL:\nhttp://%s",
                 wifi_uploader_ssid(), wifi_uploader_ip());
        lv_obj_set_style_text_color(message, lv_color_hex(watch_theme_palette()->good), LV_PART_MAIN);
    } else if (!wifi_uploader_has_credentials()) {
        snprintf(text, sizeof(text),
                 "Wi-Fi not set.\n\nRun USB setup\non your computer.");
        lv_obj_set_style_text_color(message, lv_color_hex(watch_theme_palette()->warning), LV_PART_MAIN);
    } else {
        snprintf(text, sizeof(text),
                 "Wi-Fi failed\n\n%s\n\nCheck 2.4GHz\nSSID/password.",
                 esp_err_to_name(s_wifi_upload_status));
        lv_obj_set_style_text_color(message, lv_color_hex(watch_theme_palette()->error), LV_PART_MAIN);
    }
    lv_label_set_text(message, text);
    make_button(screen, "BACK", 348, state_button_cb, (void *)(intptr_t)APP_LAUNCHER);
    s_wifi_ui_timer = lv_timer_create(refresh_wifi_upload, 250, NULL);
}

static bool has_media_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL) return false;
    return !strcasecmp(dot, ".rom") || !strcasecmp(dot, ".dsk") || !strcasecmp(dot, ".cas");
}

static int compare_rom_names(const void *left, const void *right)
{
    return strcasecmp((const char *)left, (const char *)right);
}

static void scan_roms(void)
{
    s_rom_count = 0;
    if (!s_sd_ready) {
        return;
    }
    DIR *directory = opendir(ROM_DIRECTORY);
    if (directory == NULL) {
        ESP_LOGW(TAG, "Could not open %s: errno=%d", ROM_DIRECTORY, errno);
        return;
    }
    struct dirent *entry;
    while (s_rom_count < MAX_ROMS && (entry = readdir(directory)) != NULL) {
        if (!has_media_extension(entry->d_name)) {
            continue;
        }
        if (strlen(entry->d_name) >= sizeof(s_rom_names[0])) {
            ESP_LOGW(TAG, "ROM filename too long: skipped");
            continue;
        }
        strlcpy(s_rom_names[s_rom_count], entry->d_name, sizeof(s_rom_names[0]));
        ++s_rom_count;
    }
    closedir(directory);
    qsort(s_rom_names, s_rom_count, sizeof(s_rom_names[0]), compare_rom_names);
    ESP_LOGI(TAG, "ROM scan: %u file(s)", (unsigned)s_rom_count);
    for (size_t i = 0; i < s_rom_count; ++i)
        ESP_LOGI(TAG, "MEDIA %u: %s", (unsigned)i, s_rom_names[i]);
}

static void rom_button_cb(lv_event_t *event)
{
    const char *name = lv_event_get_user_data(event);
    snprintf(s_selected_rom, sizeof(s_selected_rom), "%s/%s", ROM_DIRECTORY, name);
    ESP_LOGI(TAG, "ROM selected: %s", s_selected_rom);
    set_state_locked(APP_MSX);
}

static const char *machine_label_text(void)
{
    static const char *const labels[] = {"MODEL\nMSX1", "MODEL\nMSX2", "MODEL\nMSX2+"};
    return labels[s_msx_model - 1];
}

static void machine_button_cb(lv_event_t *event)
{
    s_msx_model = s_msx_model % 3 + 1;
    lv_label_set_text(lv_event_get_user_data(event), machine_label_text());
}

static void render_rom_browser(lv_obj_t *screen)
{
    scan_roms();
    make_title(screen, "MSX MEDIA");

    lv_obj_t *list = make_touch_list(screen);
    lv_obj_t *machine = lv_button_create(list);
    lv_obj_set_size(machine, 324, 80);
    watch_style_button(machine, false);
    lv_obj_set_style_pad_all(machine, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(machine, lv_color_hex(0xded087), LV_PART_MAIN);
    lv_obj_set_style_border_color(machine, lv_color_hex(0xded087), LV_PART_MAIN);
    lv_obj_set_style_text_color(machine, lv_color_black(), LV_PART_MAIN);
    lv_obj_t *machine_label = lv_label_create(machine);
    lv_label_set_text(machine_label, machine_label_text());
    lv_obj_set_style_text_align(machine_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_center(machine_label);
    lv_obj_add_event_cb(machine, machine_button_cb, LV_EVENT_CLICKED, machine_label);

    lv_obj_t *hint = lv_label_create(screen);
    lv_label_set_text(hint, "BOOT:HOME");
    lv_obj_set_style_text_color(hint, lv_color_hex(watch_theme_palette()->muted), LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -18);

    if (!s_sd_ready || s_rom_count == 0) {
        lv_obj_t *empty = lv_label_create(list);
        lv_label_set_text(empty, !s_sd_ready
                                 ? "microSD is not mounted.\nClock and launcher remain available."
                                 : "No .rom files found.\nCopy files to /msx/roms on the SD card.");
        lv_obj_set_width(empty, 300);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(empty, lv_color_hex(watch_theme_palette()->muted), LV_PART_MAIN);
        return;
    }

    for (size_t i = 0; i < s_rom_count; ++i) {
        lv_obj_t *button = lv_button_create(list);
        lv_obj_set_size(button, 324, 112);
        lv_obj_set_flex_grow(button, 0);
        lv_obj_set_style_pad_all(button, 12, LV_PART_MAIN);
        lv_obj_set_scrollable(button, false);
        watch_style_button(button, false);
        lv_obj_add_event_cb(button, rom_button_cb, LV_EVENT_CLICKED, s_rom_names[i]);
        lv_obj_t *label = lv_label_create(button);
        char display_name[MAX_ROM_NAME];
        rom_filename_display(s_rom_names[i], display_name, sizeof(display_name));
        char *extension = strrchr(display_name, '.');
        if (extension != NULL) *extension = '\0';
        watch_upper_ascii(display_name);
        lv_label_set_text(label, display_name);
        lv_obj_set_size(label, 296, 78);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_center(label);
    }
    lv_obj_t *back = make_button(list, "BACK", 0, state_button_cb,
                                 (void *)(intptr_t)APP_LAUNCHER);
    lv_obj_set_height(back, 80);
}

typedef struct { const uint16_t *pixels; int64_t scale_us; } stream_frame_t;
static void render_msx_rows(void *context, uint16_t *dma, int first, int count)
{
    stream_frame_t *frame = context;
    int64_t started = esp_timer_get_time();
    msx_video_scale_rotate_rows(frame->pixels, dma, first, count);
    frame->scale_us += esp_timer_get_time() - started;
}

typedef struct {
    const uint16_t *pixels;
    msx_diff_region_t region;
    int64_t scale_us;
} diff_frame_t;
static void render_msx_region_rows(void *context,uint16_t *dma,int first,int count)
{
    diff_frame_t *frame=context;
    int64_t start=esp_timer_get_time();
    msx_video_scale_region_rows(frame->pixels,dma,frame->region,first,count);
    frame->scale_us+=esp_timer_get_time()-start;
}

typedef struct {
    const uint16_t *pixels;
    msx_overlay_region_t region;
} overlay_frame_t;
static void render_overlay_region_rows(void *context,uint16_t *dma,int first,int count)
{
    overlay_frame_t *frame=context;
    for(int row=0;row<count;++row)
        memcpy(dma+row*frame->region.width,
               frame->pixels+(frame->region.y+first+row)*MSX_CANVAS_WIDTH+frame->region.x,
               frame->region.width*sizeof(*dma));
}

static bool display_msx_frame(const uint16_t *pixels, size_t width, size_t height,
                              void *context,const msx_diff_region_t *prepared,int prepared_count,
                              int64_t diff_us)
{
    const int64_t frame_started = esp_timer_get_time();
    (void)context;
    if (width != MSX_SOURCE_WIDTH || height != MSX_SOURCE_HEIGHT ||
        s_msx_canvas_buffer == NULL || !bsp_display_lock(20)) {
        return false;
    }

    /* The emulator task can already be waiting for the LVGL lock when BOOT
     * switches away from APP_MSX. Re-check the state and object only after
     * taking the lock so a late frame can never invalidate a deleted canvas
     * or overwrite the newly rendered clock/launcher screen. */
    if (app_state_get() != APP_MSX || s_msx_canvas == NULL ||
        (unsigned)(uintptr_t)context != atomic_load(&s_video_generation)) {
        bsp_display_unlock();
        return false;
    }

    /* Keep LVGL and CO5300 in the proven portrait coordinate system. Rotate
     * only the emulator pixels so the watch can be held counter-clockwise for
     * a 384 x 342 landscape game view. Logical landscape top maps to the
     * physical right edge: (x, y) -> (341 - y, x). */
    lv_refr_now(s_display); /* Finish any pending screen transition first. */
    const int64_t copy_started = esp_timer_get_time();
    esp_err_t blit_result;
    int64_t scale_elapsed, transfer_elapsed;
    int mode=atomic_load(&s_video_mode);
    bool plain_game=msx_overlay_can_stream();
    static unsigned partial_frames,full_frames,unchanged_frames,region_count;
    static unsigned phase_min=UINT32_MAX,phase_max,phase_samples;
    static int64_t previous_frame;
    static unsigned previous_generation, gap_max, gap_over_25ms;
    unsigned current_generation = (unsigned)(uintptr_t)context;
    if (mode == 3 && plain_game) {
        if (previous_frame && previous_generation == current_generation) {
            unsigned gap = (unsigned)(frame_started - previous_frame);
            if (gap > gap_max) gap_max = gap;
            if (gap >= 25000) ++gap_over_25ms;
        }
        previous_frame = frame_started;
        previous_generation = current_generation;
    } else previous_frame = 0;
    if(mode==3 && plain_game && display_sync_available()) {
        display_sync_stats_t sync;display_sync_stats(&sync);
        unsigned phase=(unsigned)(esp_timer_get_time()-sync.last_rise_us);
        if(phase<phase_min) phase_min=phase;
        if(phase>phase_max) phase_max=phase;
        ++phase_samples;
    }
    if(mode==3 && plain_game) {
        lv_obj_set_hidden(s_msx_canvas,true);
        msx_diff_region_t regions[24];
        unsigned generation=(unsigned)(uintptr_t)context;
        int count=prepared_count;
        if(count>0) memcpy(regions,prepared,count*sizeof(*regions));
        scale_elapsed=esp_timer_get_time()-copy_started;
        blit_result=ESP_OK;
        if(count<0) {
            stream_frame_t frame={.pixels=pixels};
            blit_result=lvgl_port_blit_rgb565_render(s_display,MSX_CANVAS_X,MSX_CANVAS_Y,
                MSX_CANVAS_WIDTH,MSX_CANVAS_HEIGHT,render_msx_rows,&frame);
            scale_elapsed+=frame.scale_us;++full_frames;
        } else {
            for(int i=0;i<count;++i) for(int j=i+1;j<count;++j) if(regions[j].y<regions[i].y) {
                msx_diff_region_t swap=regions[i];regions[i]=regions[j];regions[j]=swap;
            }
            for(int i=0;i<count&&blit_result==ESP_OK;++i) {
                diff_frame_t frame={.pixels=pixels,.region=regions[i]};
                blit_result=lvgl_port_blit_rgb565_render(s_display,
                    MSX_CANVAS_X+frame.region.x,MSX_CANVAS_Y+frame.region.y,
                    frame.region.width,frame.region.height,render_msx_region_rows,&frame);
                scale_elapsed+=frame.scale_us;
            }
            if(count) {++partial_frames;region_count+=count;} else ++unchanged_frames;
        }
        if(blit_result==ESP_OK) {
            s_video_previous_generation=generation;s_video_previous_valid=true;
        } else s_video_previous_valid=false;
        transfer_elapsed=esp_timer_get_time()-copy_started-scale_elapsed;
    } else if (mode==0) {
        msx_overlay_region_t overlay_regions[2]; int overlay_count;
        if(msx_overlay_render(s_msx_canvas,s_msx_canvas_buffer,
                              overlay_regions,&overlay_count)==MSX_OVERLAY_NONE)
            msx_video_scale_rotate(pixels,s_msx_canvas_buffer);
        msx_overlay_levels(active_screen(),s_msx_canvas_buffer);
        scale_elapsed=esp_timer_get_time()-copy_started;
        int64_t transfer_started=esp_timer_get_time();
        lv_obj_set_hidden(s_msx_canvas,false);
        lv_obj_invalidate(s_msx_canvas);
        lv_refr_now(s_display);
        blit_result=lvgl_port_wait_for_flush(s_display);
        transfer_elapsed=esp_timer_get_time()-transfer_started;
    } else if (mode==1 && plain_game) {
        lv_obj_set_hidden(s_msx_canvas,true);
        stream_frame_t frame = {.pixels = pixels, .scale_us = 0};
        blit_result = lvgl_port_blit_rgb565_render(s_display, MSX_CANVAS_X, MSX_CANVAS_Y,
            MSX_CANVAS_WIDTH, MSX_CANVAS_HEIGHT, render_msx_rows, &frame);
        scale_elapsed = frame.scale_us;
        transfer_elapsed = esp_timer_get_time() - copy_started - scale_elapsed;
    } else {
        lv_obj_set_hidden(s_msx_canvas,true);
        msx_overlay_region_t overlay_regions[2]; int overlay_count;
        msx_overlay_result_t overlay=msx_overlay_render(s_msx_canvas,s_msx_canvas_buffer,
                                                        overlay_regions,&overlay_count);
        if (overlay==MSX_OVERLAY_NONE)
            msx_video_scale_rotate(pixels, s_msx_canvas_buffer);
        int notice_value;
        bool level_notice=watch_controls_notice((uint32_t)(esp_timer_get_time()/1000),
                                                &notice_value)!=0;
        if(level_notice) msx_overlay_levels(active_screen(),s_msx_canvas_buffer);
        scale_elapsed = esp_timer_get_time() - copy_started;
        int64_t transfer_started = esp_timer_get_time();
        if(overlay==MSX_OVERLAY_UNCHANGED&&!level_notice) {
            blit_result=ESP_OK;
            ++unchanged_frames;
        } else if(overlay==MSX_OVERLAY_PARTIAL&&!level_notice) {
            blit_result=ESP_OK;
            for(int i=0;i<overlay_count&&blit_result==ESP_OK;++i) {
                overlay_frame_t frame={.pixels=s_msx_canvas_buffer,.region=overlay_regions[i]};
                blit_result=lvgl_port_blit_rgb565_render(s_display,
                    MSX_CANVAS_X+frame.region.x,MSX_CANVAS_Y+frame.region.y,
                    frame.region.width,frame.region.height,render_overlay_region_rows,&frame);
            }
            ++partial_frames;region_count+=overlay_count;
        } else {
            blit_result=lvgl_port_blit_rgb565(s_display,MSX_CANVAS_X,MSX_CANVAS_Y,
                MSX_CANVAS_WIDTH,MSX_CANVAS_HEIGHT,s_msx_canvas_buffer);
            ++full_frames;
        }
        transfer_elapsed = esp_timer_get_time() - transfer_started;
    }
    if(mode!=3 || !plain_game) s_video_previous_valid=false;
    if (blit_result != ESP_OK) ESP_LOGE(TAG, "MSX blit: %s", esp_err_to_name(blit_result));
    bsp_display_unlock();
    static int64_t report_started;
    static int64_t display_us;
    static int64_t copy_us;
    static int64_t refresh_us;
    static unsigned frames;
    display_us += esp_timer_get_time() - frame_started + diff_us;
    copy_us += scale_elapsed+diff_us;
    refresh_us += transfer_elapsed;
    ++frames;
    const int64_t now = esp_timer_get_time();
    if (report_started == 0) report_started = now;
    if (now - report_started >= 5000000) {
        ESP_LOGI(TAG, "video timing: %u frames/5s, total=%u copy=%u refresh=%u us", frames,
                 (unsigned)(display_us / frames), (unsigned)(copy_us / frames),
                 (unsigned)(refresh_us / frames));
        ESP_LOGI(TAG,"video regions: mode=%d partial=%u full=%u unchanged=%u rects=%u te=%d",
            mode,partial_frames,full_frames,unchanged_frames,region_count,display_sync_available());
        ESP_LOGI(TAG,"video sync phase=%d us observed=%u..%u samples=%u",
            atomic_load(&s_video_phase_us),phase_samples?phase_min:0,phase_max,phase_samples);
        ESP_LOGI(TAG, "video cadence: max_gap=%u us gaps_ge_25ms=%u", gap_max, gap_over_25ms);
        gap_max = gap_over_25ms = 0;
        phase_min=UINT32_MAX;phase_max=phase_samples=0;
        partial_frames=full_frames=unchanged_frames=region_count=0;
        report_started = now;
        display_us = 0;
        copy_us = 0;
        refresh_us = 0;
        frames = 0;
    }
    return mode==3 && plain_game && blit_result==ESP_OK;
}

static void video_worker(void *argument)
{
    (void)argument;
    video_frame_t frame;
    while (true) {
        if (xQueueReceive(s_video_ready, &frame, portMAX_DELAY) == pdTRUE) {
            video_frame_t newer;
            while (xQueueReceive(s_video_ready, &newer, 0) == pdTRUE) {
                xQueueSend(s_video_free, &frame, portMAX_DELAY);
                frame = newer;
            }
            /* Compare before waiting for TE so the pulse starts actual transfer,
             * not a multi-millisecond PSRAM diff. Only this worker owns history.
             * Keep this immutable frame until transfer ends; newly queued frames
             * are considered on the next iteration. */
            msx_diff_region_t regions[24];
            int count=-1;
            int64_t diff_us=0;
            if(app_state_get()==APP_MSX && atomic_load(&s_video_mode)==3) {
                int64_t start=esp_timer_get_time();
                count=msx_video_diff(frame.pixels,
                    s_video_previous_valid&&s_video_previous_generation==frame.generation?
                        s_video_previous:NULL,regions,24);
                diff_us=esp_timer_get_time()-start;
                if(display_sync_available() && display_sync_wait(40)) {
                    display_sync_stats_t sync;display_sync_stats(&sync);
                    int64_t target=sync.last_rise_us+atomic_load(&s_video_phase_us);
                    int64_t left=target-esp_timer_get_time();
                    if(left>1500) vTaskDelay(pdMS_TO_TICKS((left-500)/1000));
                    left=target-esp_timer_get_time();
                    if(left>0) esp_rom_delay_us((uint32_t)left);
                }
            }
            if(display_msx_frame(frame.pixels, MSX_SOURCE_WIDTH, MSX_SOURCE_HEIGHT,
                                 (void *)(uintptr_t)frame.generation,regions,count,diff_us)) {
                /* Keep the immutable buffer that was just presented as the
                 * next diff history.  Return the old history allocation to
                 * the producer through this queue entry.  This replaces a
                 * full 256x228 PSRAM-to-PSRAM copy after every successful
                 * frame and preserves single-owner buffer lifetime. */
                uint16_t *old_history=s_video_previous;
                s_video_previous=frame.pixels;
                frame.pixels=old_history;
            }
            xQueueSend(s_video_free, &frame, portMAX_DELAY);
            /* xQueueReceive() blocks when the producer has no newer frame.
             * An unconditional tick here consumed roughly 1 ms of the
             * 16.9 ms TE budget and could make the next prepared update miss
             * its pulse, turning a near-60 Hz path into an every-other-TE
             * transfer.  Yield naturally through the queue instead. */
        }
    }
}

static uint16_t *msx_frame_acquire(void *context)
{
    (void)context;
    video_frame_t frame;
    return xQueueReceive(s_video_free, &frame, 0) == pdTRUE ? frame.pixels : NULL;
}

static void msx_frame_callback(const uint16_t *pixels, size_t width, size_t height,
                               void *context)
{
    if (width != MSX_SOURCE_WIDTH || height != MSX_SOURCE_HEIGHT || s_video_free == NULL) {
        return;
    }
    int expected = 1;
    if (atomic_compare_exchange_strong(&s_capture_state, &expected, 2)) {
        memcpy(s_capture_pixels,pixels,width*height*sizeof(*pixels));
        atomic_store(&s_capture_state,3);
    }
    video_frame_t frame = {.pixels = (uint16_t *)pixels, .generation = (unsigned)(uintptr_t)context};
    if (xQueueSend(s_video_ready, &frame, 0) != pdTRUE) {
        xQueueSend(s_video_free, &frame, portMAX_DELAY);
    }
}

static bool init_video_pipeline(void)
{
    if (s_video_free != NULL) return true;
    uint16_t *buffers[3] = {0};
    for (unsigned i = 0; i < 3; ++i) {
        buffers[i] = heap_caps_calloc(MSX_SOURCE_WIDTH * MSX_SOURCE_HEIGHT, sizeof(uint16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buffers[i] == NULL) {
            for (unsigned j = 0; j < 3; ++j) heap_caps_free(buffers[j]);
            return false;
        }
    }
    s_video_previous=heap_caps_calloc(MSX_SOURCE_WIDTH*MSX_SOURCE_HEIGHT, sizeof(uint16_t),
        MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_video_previous) goto fail;
    s_video_free = xQueueCreate(3, sizeof(video_frame_t));
    s_video_ready = xQueueCreate(3, sizeof(video_frame_t));
    if (s_video_free == NULL || s_video_ready == NULL) goto fail;
    for (unsigned i = 0; i < 3; ++i) {
        video_frame_t frame = {.pixels = buffers[i], .generation = 0};
        xQueueSend(s_video_free, &frame, 0);
    }
    if (xTaskCreatePinnedToCore(video_worker, "msx_video", 6144, NULL, 4, NULL, 0) != pdPASS)
        goto fail;
    return true;
fail:
    if (s_video_free != NULL) vQueueDelete(s_video_free);
    if (s_video_ready != NULL) vQueueDelete(s_video_ready);
    s_video_free = s_video_ready = NULL;
    heap_caps_free(s_video_previous);s_video_previous=NULL;
    for (unsigned i = 0; i < 3; ++i) heap_caps_free(buffers[i]);
    return false;
}

static void audio_worker(void *argument)
{
    (void)argument;
    audio_chunk_t chunk;
    while (true) {
        if (xQueueReceive(s_audio_queue, &chunk, portMAX_DELAY) == pdTRUE &&
            app_state_get() == APP_MSX &&
            chunk.generation == atomic_load(&s_video_generation)) {
            const int result = esp_codec_dev_write(s_speaker, chunk.samples,
                                                    (int)(chunk.count * sizeof(int16_t)));
            if (result != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "audio output error=%d", result);
        }
    }
}

static void msx_audio_callback(const int16_t *samples, size_t count, unsigned int rate,
                               void *context)
{
    (void)context;
    if (s_speaker == NULL || rate != 32000 || count == 0) {
        return;
    }
    if (s_audio_queue == NULL || count > sizeof(((audio_chunk_t *)0)->samples) / sizeof(*samples)) return;
    audio_chunk_t chunk = {.count = count, .generation = (unsigned)(uintptr_t)context};
    memcpy(chunk.samples, samples, count * sizeof(*samples));
    /* Bounded audio buffering absorbs short CPU/memory contention without
     * unbounded latency. A full queue provides real-time backpressure. */
    xQueueSend(s_audio_queue, &chunk, portMAX_DELAY);
}

static bool create_msx_surface(lv_obj_t *screen)
{
    if (s_msx_canvas_buffer == NULL) {
        s_msx_canvas_buffer = heap_caps_aligned_alloc(64, MSX_CANVAS_WIDTH * MSX_CANVAS_HEIGHT *
                                                   sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_msx_canvas_buffer == NULL) {
        lv_obj_t *error = lv_label_create(screen);
        lv_label_set_text(error, "Video buffer allocation failed");
        lv_obj_center(error);
        return false;
    }
    memset(s_msx_canvas_buffer, 0,
           MSX_CANVAS_WIDTH * MSX_CANVAS_HEIGHT * sizeof(uint16_t));
    /* Keep the frame outside the direct-blit rectangle. A canvas border is
     * drawn inside its bounds and is overwritten by every direct transfer. */
    lv_obj_t *frame = lv_obj_create(screen);
    lv_obj_remove_style_all(frame);
    lv_obj_set_pos(frame, MSX_CANVAS_X - MSX_FRAME_WIDTH,
                   MSX_CANVAS_Y - MSX_FRAME_WIDTH);
    lv_obj_set_size(frame, MSX_CANVAS_WIDTH + 2 * MSX_FRAME_WIDTH,
                    MSX_CANVAS_HEIGHT + 2 * MSX_FRAME_WIDTH);
    lv_obj_set_style_border_color(frame, lv_color_hex(watch_theme_get() == WATCH_BASIC ? 0xffffff : 0x5955e0), LV_PART_MAIN);
    lv_obj_set_style_border_width(frame, MSX_FRAME_WIDTH, LV_PART_MAIN);
    lv_obj_set_scrollable(frame, false);
    s_msx_canvas = lv_canvas_create(screen);
    lv_obj_remove_style_all(s_msx_canvas);
    lv_canvas_set_buffer(s_msx_canvas, s_msx_canvas_buffer,
                         MSX_CANVAS_WIDTH, MSX_CANVAS_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(s_msx_canvas, MSX_CANVAS_X, MSX_CANVAS_Y);
    lv_obj_set_hidden(s_msx_canvas,true); /* All game pixels use an explicit blit. */
    return true;
}

static void render_msx(lv_obj_t *screen)
{
    if (s_audio_queue == NULL && s_speaker != NULL) {
        s_audio_queue = xQueueCreate(3, sizeof(audio_chunk_t));
        if (s_audio_queue != NULL &&
            xTaskCreatePinnedToCore(audio_worker, "msx_audio", 4096, NULL, 7, NULL, 0) != pdPASS) {
            vQueueDelete(s_audio_queue);
            s_audio_queue = NULL;
        }
    }
    if (!init_video_pipeline() || !create_msx_surface(screen)) {
        ESP_LOGE(TAG, "video pipeline allocation failed");
        return;
    }

    const unsigned generation = atomic_fetch_add(&s_video_generation, 1) + 1;
    const esp_err_t ret = msx_runner_start(s_selected_rom, msx_frame_callback,
                                           msx_audio_callback, (void *)(uintptr_t)generation,
                                           s_msx_model, msx_frame_acquire);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "could not start the MSX emulator: %s", esp_err_to_name(ret));
    }
}

static void diagnostic_rect(uint16_t *pixels, int x, int y, int width, int height,
                             uint16_t color)
{
    for (int row = y; row < y + height; ++row) {
        if (row < 0 || row >= BSP_LCD_V_RES) continue;
        for (int column = x; column < x + width; ++column) {
            if (column >= 0 && column < BSP_LCD_H_RES)
                pixels[row * BSP_LCD_H_RES + column] = color;
        }
    }
}

static void diagnostic_outline(uint16_t *pixels, int x, int y, int width, int height,
                                uint16_t color)
{
    diagnostic_rect(pixels, x, y, width, 2, color);
    diagnostic_rect(pixels, x, y + height - 2, width, 2, color);
    diagnostic_rect(pixels, x, y, 2, height, color);
    diagnostic_rect(pixels, x + width - 2, y, 2, height, color);
}

static void render_display_test(lv_obj_t *screen)
{
    static uint16_t *pixels;
    const size_t bytes = BSP_LCD_H_RES * BSP_LCD_V_RES * sizeof(uint16_t);
    if (pixels == NULL) pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pixels == NULL) {
        lv_obj_t *error = lv_label_create(screen);
        lv_label_set_text(error, "Display buffer failed");
        lv_obj_center(error);
        return;
    }
    memset(pixels, 0, bytes);
    const int cx = BSP_LCD_H_RES / 2;
    const int cy = BSP_LCD_V_RES / 2;
    /* Symmetric grid and two-pixel center axes straddle the geometric
     * half-pixel center of this even-sized panel. Tick pitch is 16 pixels. */
    for (int d = 32; d < cy; d += 32) {
        diagnostic_rect(pixels, 0, cy - d, BSP_LCD_H_RES, 1, 0x2104);
        diagnostic_rect(pixels, 0, cy + d - 1, BSP_LCD_H_RES, 1, 0x2104);
    }
    for (int d = 32; d < cx; d += 32) {
        diagnostic_rect(pixels, cx - d, 0, 1, BSP_LCD_V_RES, 0x2104);
        diagnostic_rect(pixels, cx + d - 1, 0, 1, BSP_LCD_V_RES, 0x2104);
    }
    diagnostic_outline(pixels, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, 0xffff);
    diagnostic_outline(pixels, MSX_CANVAS_X, MSX_CANVAS_Y,
                        MSX_CANVAS_WIDTH, MSX_CANVAS_HEIGHT, 0x051f);
    diagnostic_rect(pixels, cx - 1, 0, 2, BSP_LCD_V_RES, 0xffe0);
    diagnostic_rect(pixels, 0, cy - 1, BSP_LCD_H_RES, 2, 0xffe0);
    for (int d = 16; d < cx; d += 16) {
        const int length = d % 64 == 0 ? 20 : 10;
        diagnostic_rect(pixels, cx - d, cy - length / 2, 1, length, 0xffe0);
        diagnostic_rect(pixels, cx + d - 1, cy - length / 2, 1, length, 0xffe0);
    }
    for (int d = 16; d < cy; d += 16) {
        const int length = d % 64 == 0 ? 20 : 10;
        diagnostic_rect(pixels, cx - length / 2, cy - d, length, 1, 0xffe0);
        diagnostic_rect(pixels, cx - length / 2, cy + d - 1, length, 1, 0xffe0);
    }
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_t *canvas = lv_canvas_create(screen);
    lv_obj_remove_style_all(canvas);
    lv_obj_set_scrollable(canvas, false);
    lv_canvas_set_buffer(canvas, pixels, BSP_LCD_H_RES, BSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 0, 0);
    lv_refr_now(s_display);
    /* Full-panel transfer also clears all margins and old panel contents. */
    const esp_err_t result = lvgl_port_blit_rgb565(
        s_display, 0, 0, BSP_LCD_H_RES, BSP_LCD_V_RES, pixels);
    ESP_LOGI(TAG, "display calibration: panel=%dx%d center=(%d.5,%d.5) tick=16 MSX=(%d,%d %dx%d) blit=%s",
             BSP_LCD_H_RES, BSP_LCD_V_RES, cx - 1, cy - 1,
             MSX_CANVAS_X, MSX_CANVAS_Y, MSX_CANVAS_WIDTH, MSX_CANVAS_HEIGHT,
             esp_err_to_name(result));
}

void app_console_display_test(void)
{
    request_state(APP_DISPLAY_TEST);
}

static void bench_touch(lv_event_t *event)
{
    (void)event;
    int mode=(atomic_load(&s_bench_mode)+1)%4;
    if(mode==3&&!display_sync_available()) mode=0;
    atomic_store(&s_bench_mode,mode);
    ESP_LOGI("display_bench","touch selected mode=%d",mode+1);
}
static void render_benchmark(lv_obj_t *screen)
{
    if(!create_msx_surface(screen)) return;
    atomic_store(&s_bench_visible_mode,-1);
    for(uint32_t i=0;i<lv_obj_get_child_count(screen);++i)
        lv_obj_set_clickable(lv_obj_get_child(screen,i),false);
    lv_obj_set_clickable(screen,true);
    lv_obj_add_event_cb(screen,bench_touch,LV_EVENT_PRESSED,NULL);
}

static void render_state_locked(app_state_t state)
{
    if (s_wifi_ui_timer != NULL) {
        lv_timer_delete(s_wifi_ui_timer);
        s_wifi_ui_timer = NULL;
    }
    if (s_clock_timer != NULL) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }
    s_clock_date = NULL;
    s_clock_power = NULL;
    s_clock_time = NULL;
    s_level_labels[0] = s_level_labels[1] = NULL;

    lv_obj_t *screen = active_screen();
    msx_overlay_reset();
    /* Child cleanup does not remove callbacks attached to the screen itself.
     * Keep the clock's tap-to-launch action scoped to the clock screen. */
    lv_obj_remove_event_cb(screen, clock_screen_cb);
    lv_obj_remove_event_cb(screen, bench_touch);
    lv_obj_clean(screen);
    style_screen(screen);
    lv_obj_set_scrollable(screen, false);
    lv_obj_scroll_to(screen, 0, 0, LV_ANIM_OFF);

    /* Rotation changes the logical resolution before the new state is built.
     * Child deletion alone only invalidates their old areas, leaving portions
     * of the portrait ROM browser in panel GRAM outside the landscape canvas.
     * Flush an opaque, empty screen over the full new coordinate space first. */
    lv_obj_invalidate(screen);
    lv_refr_now(s_display);

    switch (state) {
    case APP_CLOCK:
        render_clock(screen);
        break;
    case APP_LAUNCHER:
        render_launcher(screen);
        break;
    case APP_ROM_BROWSER:
        render_rom_browser(screen);
        break;
    case APP_WIFI_UPLOAD:
        render_wifi_upload(screen);
        break;
    case APP_MSX:
        render_msx(screen);
        break;
    case APP_DISPLAY_TEST:
        render_display_test(screen);
        break;
    case APP_DISPLAY_BENCH:
        render_benchmark(screen);
        break;
    case APP_SETTINGS:
        render_settings(screen);
        break;
    default:
        app_state_set(APP_CLOCK);
        render_clock(screen);
        break;
    }
}

static void check_msx_finished(lv_timer_t *timer)
{
    update_level_labels();
    if(app_state_get()==APP_DISPLAY_BENCH && !s_bench_worker &&
       !msx_runner_is_running() && s_msx_canvas) {
        if(xTaskCreatePinnedToCore(benchmark_worker,"display_bench",6144,NULL,4,&s_bench_worker,0)!=pdPASS)
            ESP_LOGE(TAG,"benchmark worker allocation failed");
    }
    (void)timer;
    /* Runs under LVGL's lock after the emulator has released its resources.
     * Consume completions after BOOT exits too, so they cannot affect a later run. */
    if (!msx_runner_is_running() && msx_runner_take_finished() &&
        app_state_get() == APP_MSX) {
        ESP_LOGI(TAG, "emulator finished: returning to launcher");
        set_state_locked(APP_LAUNCHER);
    }
}

static void handle_boot_short_press(void)
{
    switch (app_state_get()) {
    case APP_CLOCK:
        request_state(APP_LAUNCHER);
        break;
    case APP_LAUNCHER:
        request_state(APP_CLOCK);
        break;
    case APP_ROM_BROWSER:
    case APP_WIFI_UPLOAD:
    case APP_MSX:
    case APP_EMU_MENU:
    case APP_DISPLAY_TEST:
    case APP_DISPLAY_BENCH:
    case APP_SETTINGS:
        request_state(APP_LAUNCHER);
        break;
    default:
        request_state(APP_CLOCK);
        break;
    }
}

static void toggle_display(void)
{
    s_display_awake = !s_display_awake;
    if (!bsp_display_lock(UI_LOCK_TIMEOUT_MS)) { s_display_awake = !s_display_awake; return; }
    esp_err_t ret = bsp_display_brightness_set(s_display_awake ? watch_controls_brightness() : 0);
    bsp_display_unlock();
    ESP_LOGI(TAG, "display %s (%s)", s_display_awake ? "awake" : "dark",
             esp_err_to_name(ret));
}

static void button_task(void *argument)
{
    (void)argument;
    const gpio_config_t boot_config = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&boot_config));
    esp_io_expander_handle_t expander = bsp_io_expander_init();
    ESP_ERROR_CHECK(esp_io_expander_set_dir(expander, IO_EXPANDER_PIN_NUM_4,
                                            IO_EXPANDER_INPUT));

    bool previous_boot = false;
    bool previous_pwr = false;
    TickType_t boot_down_at = 0;
    TickType_t pwr_down_at = 0;
    ESP_LOGI(TAG, "button input ready");

    while (true) {
        watch_controls_tick((uint32_t)(esp_timer_get_time()/1000));
        static int applied_volume = -1, applied_brightness = -1;
        int volume = watch_controls_volume(), brightness = watch_controls_brightness();
        if (volume != applied_volume && s_speaker != NULL &&
            esp_codec_dev_set_out_vol(s_speaker, volume) == ESP_CODEC_DEV_OK) {
            applied_volume = volume;
            ESP_LOGI(TAG, "codec volume applied=%d", volume);
        }
        if (brightness != applied_brightness && bsp_display_lock(20)) {
            if (bsp_display_brightness_set(s_display_awake ? brightness : 0) == ESP_OK) {
                applied_brightness = brightness;
                ESP_LOGI(TAG, "panel brightness applied=%d awake=%d", brightness, s_display_awake);
            }
            bsp_display_unlock();
        }
        const bool boot = gpio_get_level(GPIO_NUM_0) == 0;
        uint32_t pwr_level = 0;
        esp_err_t ret = esp_io_expander_get_level(expander, IO_EXPANDER_PIN_NUM_4,
                                                   &pwr_level);
        const bool pwr = ret == ESP_OK && (pwr_level & IO_EXPANDER_PIN_NUM_4) != 0;
        const TickType_t now = xTaskGetTickCount();

        if (boot != previous_boot) {
            if (boot) {
                boot_down_at = now;
            } else if ((now - boot_down_at) < pdMS_TO_TICKS(1500)) {
                ESP_LOGI(TAG, "BOOT short press");
                handle_boot_short_press();
            } else {
                ESP_LOGI(TAG, "BOOT long press");
            }
            previous_boot = boot;
        }
        if (pwr != previous_pwr) {
            if (pwr) {
                pwr_down_at = now;
            } else if ((now - pwr_down_at) < pdMS_TO_TICKS(1500)) {
                ESP_LOGI(TAG, "PWR short press");
                toggle_display();
            } else {
                ESP_LOGI(TAG, "PWR long press observed");
            }
            previous_pwr = pwr;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

void app_main(void)
{
    app_state_init();
    ESP_LOGI(TAG, "starting RetroS3 Watch firmware");
    ESP_ERROR_CHECK(nvs_flash_init());
    watch_theme_init();
    watch_controls_init();
    const esp_err_t wifi_init_result = wifi_uploader_init();
    if (wifi_init_result != ESP_OK) {
        ESP_LOGW(TAG, "early Wi-Fi init: %s", esp_err_to_name(wifi_init_result));
    }

    const bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * LVGL_DMA_BUFFER_LINES,
        .double_buffer = false,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
        },
    };
    s_display = bsp_display_start_with_config(&display_config);
    if (s_display == NULL) {
        ESP_LOGE(TAG, "display initialization failed");
        abort();
    }
    ESP_ERROR_CHECK(bsp_display_brightness_set(watch_controls_brightness()));

    ESP_LOGI(TAG,"display TE input: %s",esp_err_to_name(display_sync_init()));
    init_pmic();
    init_rtc();
    init_storage();
    init_audio();
    if (xTaskCreate(wifi_worker, "wifi_upload", 8192, NULL, 5, &s_wifi_worker) != pdPASS) {
        ESP_LOGE(TAG, "could not start Wi-Fi worker");
    }
    ESP_ERROR_CHECK(serial_console_start(s_sd_ready));
    esp_err_t controller_result = controller_start();
    if (controller_result != ESP_OK) {
        ESP_LOGW(TAG, "BLE controller unavailable: %s", esp_err_to_name(controller_result));
    }

    if (!bsp_display_lock(UI_LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "could not lock LVGL for initial screen");
        abort();
    }
    set_state_locked(APP_CLOCK);
    lv_timer_create(check_msx_finished, 100, NULL);
    bsp_display_unlock();

    if (xTaskCreate(button_task, "side_buttons", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start side-button task");
    }

    uint32_t uptime = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        ++uptime;
        if (uptime % 30 == 0) {
            ESP_LOGI(TAG, "alive: uptime=%lus state=%s sd=%s", (unsigned long)uptime,
                     app_state_name(app_state_get()), s_sd_ready ? "mounted" : "missing");
        }
    }
}

void app_console_video_phase(int microseconds)
{
    if(microseconds>=0 && microseconds<=16000) atomic_store(&s_video_phase_us,microseconds);
    printf("VIDEO PHASE %d us\n",atomic_load(&s_video_phase_us));
    fflush(stdout);
}

void app_console_video_mode(int mode)
{
    if(mode>=0 && mode<=3) atomic_store(&s_video_mode,mode);
}

static void export_frame(const uint16_t *pixels,int width,int height)
{
    uint32_t crc=esp_rom_crc32_le(0,(const uint8_t *)pixels,width*height*2);
    printf("CAPTURE BEGIN %d %d %08lx\n",width,height,(unsigned long)crc);
    char line[1450];
    static const char hex[]="0123456789abcdef";
    for(int row=0;row<height;++row) {
        int offset=snprintf(line,sizeof(line),"CAPTURE ROW %d ",row);
        for(int column=0;column<width;++column) {
            unsigned value=pixels[row*width+column];
            for(int shift=12;shift>=0;shift-=4) line[offset++]=hex[(value>>shift)&15];
        }
        line[offset++]='\n';fwrite(line,1,offset,stdout);
    }
    printf("CAPTURE END %08lx\n",(unsigned long)crc);fflush(stdout);
}
/* Diagnostic snapshots never change SD states. Bench and overlay frames
 * are copied under the LVGL lock; gameplay captures use the source buffer. */
void app_console_capture_frame(void)
{
    app_state_t state = app_state_get();
    if (state == APP_DISPLAY_BENCH || state == APP_MSX) {
        if (!bsp_display_lock(1000)) {printf("CAPTURE ERROR lock\n");fflush(stdout);return;}
        state = app_state_get();
        bool canvas_visible = s_msx_canvas_buffer && (state == APP_DISPLAY_BENCH ||
                              (state == APP_MSX && msx_overlay_is_drawn()));
        uint16_t *snapshot = NULL;
        if (canvas_visible) {
            size_t bytes = MSX_CANVAS_WIDTH * MSX_CANVAS_HEIGHT * sizeof(*snapshot);
            snapshot = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (snapshot) memcpy(snapshot, s_msx_canvas_buffer, bytes);
        }
        bsp_display_unlock();
        if (canvas_visible) {
            if (snapshot) export_frame(snapshot, MSX_CANVAS_WIDTH, MSX_CANVAS_HEIGHT);
            else {printf("CAPTURE ERROR memory\n");fflush(stdout);}
            free(snapshot);
            return;
        }
    }
    video_frame_t frame;
    if(app_state_get()!=APP_MSX||!s_video_free||
       xQueueReceive(s_video_free,&frame,pdMS_TO_TICKS(1000))!=pdTRUE) {
        printf("CAPTURE ERROR no-frame\n");fflush(stdout);return;
    }
    s_capture_pixels=frame.pixels;atomic_store(&s_capture_state,1);
    int64_t deadline=esp_timer_get_time()+1000000;
    while(atomic_load(&s_capture_state)!=3&&esp_timer_get_time()<deadline) vTaskDelay(1);
    int expected=1;
    if(atomic_compare_exchange_strong(&s_capture_state,&expected,0)) printf("CAPTURE ERROR timeout\n");
    else {
        while(atomic_load(&s_capture_state)==2) vTaskDelay(1);
        export_frame(frame.pixels,256,228);atomic_store(&s_capture_state,0);
    }
    fflush(stdout);xQueueSend(s_video_free,&frame,portMAX_DELAY);
}

typedef struct { const uint16_t *pixels; bench_region_t region; } bench_strip_t;
static void bench_copy_rows(void *context,uint16_t *dma,int first,int count)
{
    const bench_strip_t *b=context;
    for(int row=0;row<count;++row)
        memcpy(dma+row*b->region.width,b->pixels+(b->region.y+first+row)*MSX_CANVAS_WIDTH+b->region.x,
            b->region.width*2);
}
static void benchmark_worker(void *argument)
{
    (void)argument;
    int last_mode=-1;
    int64_t started=0,window=0,next=0;
    unsigned completed=0,fps10=0,errors=0;
    uint64_t render_total=0,transfer_total=0;
    uint32_t max_us=0;
    bool status=false,warming=false;
    while(true) {
        int64_t now=esp_timer_get_time();
        int requested_mode=atomic_load(&s_bench_mode);
        if(requested_mode==3) {
            if(!display_sync_available()) {
                ESP_LOGW("display_bench","TE unavailable; returning to unsynchronized SMALL AREA");
                atomic_store(&s_bench_mode,1);continue;
            }
            if(!display_sync_wait(40)) {ESP_LOGW("display_bench","TE wait timeout");continue;}
        } else if(next>now) {
            int64_t delay=next-now;
            if(delay>1500) vTaskDelay(pdMS_TO_TICKS((delay-500)/1000));
            else esp_rom_delay_us((uint32_t)delay);
            continue;
        }
        if(!bsp_display_lock(20)) { vTaskDelay(1);continue; }
        if(app_state_get()!=APP_DISPLAY_BENCH || !s_msx_canvas) {
            bsp_display_unlock();vTaskDelay(1);continue;
        }
        int mode=atomic_load(&s_bench_mode);
        if(mode!=last_mode) {
            lv_obj_set_hidden(s_msx_canvas,false); /* Canonical bench image also survives touch-triggered redraws. */
            lv_refr_now(s_display);lvgl_port_wait_for_flush(s_display);
            display_bench_reset(s_msx_canvas_buffer,mode);
            if(mode==2) {lv_obj_invalidate(s_msx_canvas);lv_refr_now(s_display);lvgl_port_wait_for_flush(s_display);}
            else lvgl_port_blit_rgb565(s_display,MSX_CANVAS_X,MSX_CANVAS_Y,MSX_CANVAS_WIDTH,MSX_CANVAS_HEIGHT,s_msx_canvas_buffer);
            started=window=next=esp_timer_get_time();completed=errors=fps10=0;
            render_total=transfer_total=0;max_us=0;last_mode=mode;status=false;warming=true;
            atomic_store(&s_bench_visible_mode,mode);
            ESP_LOGI("display_bench","START mode=%s target=60 region=384x342 emulator=%s",
                mode==0?"FULL":mode==1?"PARTIAL":mode==2?"STOCK":"SYNC",msx_runner_is_running()?"ON":"OFF");
            if(mode==3) {bsp_display_unlock();continue;} /* Wait for a fresh TE after initial full paint. */
        }
        int64_t frame_start=esp_timer_get_time();
        bench_region_t regions[4];
        int count=display_bench_frame(s_msx_canvas_buffer,frame_start-started,status,fps10,regions);
        status=false;
        int64_t rendered=esp_timer_get_time();
        esp_err_t result=ESP_OK;
        if(mode==0) result=lvgl_port_blit_rgb565(s_display,MSX_CANVAS_X,MSX_CANVAS_Y,
            MSX_CANVAS_WIDTH,MSX_CANVAS_HEIGHT,s_msx_canvas_buffer);
        else if(mode==1||mode==3) {
            /* Present moving objects in panel row order. Keep the diagnostic
             * counter update last so it cannot delay the narrow moving bar. */
            for(int i=0;i<2;++i) for(int j=i+1;j<3;++j) if(regions[j].y<regions[i].y) {
                bench_region_t swap=regions[i];regions[i]=regions[j];regions[j]=swap;
            }
            for(int i=0;i<count&&result==ESP_OK;++i) {
                bench_strip_t strip={s_msx_canvas_buffer,regions[i]};
                result=lvgl_port_blit_rgb565_render(s_display,MSX_CANVAS_X+regions[i].x,
                    MSX_CANVAS_Y+regions[i].y,regions[i].width,regions[i].height,bench_copy_rows,&strip);
            }
        } else {
            lv_obj_invalidate(s_msx_canvas);lv_refr_now(s_display);
            result=lvgl_port_wait_for_flush(s_display);
        }
        int64_t done=esp_timer_get_time();
        if(result==ESP_OK) ++completed; else {++errors;ESP_LOGE("display_bench","transfer: %s",esp_err_to_name(result));}
        render_total+=rendered-frame_start;transfer_total+=done-rendered;
        if((uint32_t)(done-frame_start)>max_us) max_us=(uint32_t)(done-frame_start);
        next+=1000000/60;if(next<done) next=done;
        if(warming) {
            /* Start frequency measurement at the first completed transfer;
             * counting that initial frame at time zero biases the first window. */
            completed=errors=0;render_total=transfer_total=0;max_us=0;window=done;warming=false;
        } else if(done-window>=1000000) {
            unsigned attempts=completed+errors;
            fps10=(unsigned)((uint64_t)completed*10000000/(done-window));
            ESP_LOGI("display_bench","mode=%s target=60 done=%u.%u render_us=%u transfer_us=%u max_us=%u errors=%u emulator=%s",
                mode==0?"FULL":mode==1?"PARTIAL":mode==2?"STOCK":"SYNC",fps10/10,fps10%10,
                (unsigned)(render_total/attempts),(unsigned)(transfer_total/attempts),max_us,errors,
                msx_runner_is_running()?"ON":"OFF");
            completed=errors=0;render_total=transfer_total=0;max_us=0;window=done;status=true;
        }
        bsp_display_unlock();
        vTaskDelay(1); /* Give the touch/LVGL task a turn even in saturated FULL mode. */
    }
}
void app_console_display_bench(int mode)
{
    if(mode==3&&!display_sync_available()) {
        ESP_LOGW("display_bench","SYNC requested but no valid TE edges observed");return;
    }
    if(mode>=0&&mode<=3) atomic_store(&s_bench_mode,mode);
    if(app_state_get()!=APP_DISPLAY_BENCH) request_state(APP_DISPLAY_BENCH);
}

void app_console_bench_status(void)
{
    static const char *names[]={"FULL","SMALL","LVGL","SYNC"};
    int selected=atomic_load(&s_bench_mode),visible=atomic_load(&s_bench_visible_mode);
    printf("BENCH STATE=%s SELECTED=%d:%s VISIBLE=%d:%s\n",app_state_name(app_state_get()),
        selected+1,names[selected],visible+1,visible>=0&&visible<=3?names[visible]:"WAIT");
    fflush(stdout);
}
