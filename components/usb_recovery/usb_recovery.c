#include "usb_recovery.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

static const char *TAG = "usb_recovery";

static void usb_recovery_task(void *arg)
{
    (void)arg;
    setvbuf(stdin, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "Ready: send 'd' to enter ROM download mode");

    while (true) {
        const int c = getchar();
        if (c == 'd' || c == 'D') {
            ESP_LOGW(TAG, "Entering ROM download mode");
            vTaskDelay(pdMS_TO_TICKS(100));
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        }
        if (c == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

esp_err_t usb_recovery_start(void)
{
    return xTaskCreate(usb_recovery_task, "usb_recovery", 3072, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
