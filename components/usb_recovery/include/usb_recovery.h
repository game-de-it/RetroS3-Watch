#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start a console task that enters the ESP32-S3 ROM loader when it receives
 * the ASCII character 'd' or 'D'. This is intended for USB Serial/JTAG debug
 * builds on boards whose PMIC prevents reliable automatic reset.
 */
esp_err_t usb_recovery_start(void);

#ifdef __cplusplus
}
#endif
