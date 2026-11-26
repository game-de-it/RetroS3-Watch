#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define WATCH_BUTTON_L2 (1U << 8)
void watch_controls_init(void);
uint32_t watch_controls_filter(uint32_t buttons);
void watch_controls_tick(uint32_t now);
void watch_controls_adjust(int kind, int delta); /* 1 volume, 2 brightness */
int watch_controls_volume(void);
int watch_controls_brightness(void);
int watch_controls_notice(uint32_t now, int *value);
#ifdef __cplusplus
}
#endif
