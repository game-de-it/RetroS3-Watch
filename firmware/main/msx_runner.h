#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define MSX_RUNNER_VIDEO_WIDTH 256
#define MSX_RUNNER_VIDEO_HEIGHT 228
#define MSX_RUNNER_BUTTON_UP (1U << 0)
#define MSX_RUNNER_BUTTON_DOWN (1U << 1)
#define MSX_RUNNER_BUTTON_LEFT (1U << 2)
#define MSX_RUNNER_BUTTON_RIGHT (1U << 3)
#define MSX_RUNNER_BUTTON_A (1U << 4)
#define MSX_RUNNER_BUTTON_B (1U << 5)
#define MSX_RUNNER_BUTTON_START (1U << 6)
#define MSX_RUNNER_BUTTON_SELECT (1U << 7)

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*msx_runner_frame_cb_t)(const uint16_t *pixels, size_t width, size_t height,
                                      void *context);
typedef void (*msx_runner_audio_cb_t)(const int16_t *samples, size_t count, unsigned int rate,
                                      void *context);
/* If supplied, the runner returns every acquired frame via frame_callback. */
typedef uint16_t *(*msx_runner_acquire_cb_t)(void *context);

esp_err_t msx_runner_start(const char *rom_path, msx_runner_frame_cb_t frame_callback,
                           msx_runner_audio_cb_t audio_callback, void *context, unsigned model,
                           msx_runner_acquire_cb_t acquire_frame);
void msx_runner_stop(void);
void msx_runner_set_buttons(uint32_t buttons);
void msx_runner_set_serial_buttons(uint32_t buttons);
bool msx_runner_is_running(void);
bool msx_runner_take_finished(void);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif
void msx_runner_set_render_percent(unsigned percent);
esp_err_t msx_runner_state_request(bool save, int slot);
esp_err_t msx_runner_disk_request(int drive, const char *filename);
#ifdef __cplusplus
}
#endif
