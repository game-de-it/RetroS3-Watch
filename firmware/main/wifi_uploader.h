#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wifi_uploader_start(bool sd_ready);
esp_err_t wifi_uploader_init(void);
void wifi_uploader_cancel(void);
void wifi_uploader_stop(void);
bool wifi_uploader_is_running(void);
esp_err_t wifi_uploader_save_credentials(const char *ssid, const char *password);
bool wifi_uploader_has_credentials(void);
const char *wifi_uploader_ssid(void);
const char *wifi_uploader_ip(void);

