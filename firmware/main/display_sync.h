#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
typedef struct {
    uint32_t rises,period_us,min_us,max_us,high_us;
    int64_t last_rise_us;
} display_sync_stats_t;
esp_err_t display_sync_init(void);
void display_sync_stats(display_sync_stats_t *stats);
bool display_sync_available(void);
bool display_sync_wait(uint32_t timeout_ms);
void display_sync_forget_task(TaskHandle_t task);
