#include "controller.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hid_common.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "msx_runner.h"
#include "watch_controls.h"

#define SCAN_SECONDS 5
#define RESCAN_DELAY_MS 5000

static const char *TAG = "controller";
static volatile bool s_connected;
static volatile bool s_opening;
static uint32_t s_last_buttons;
static esp_bd_addr_t s_target_bda;
static bool s_raw_ff13_active;
static uint16_t s_raw_conn_id;
static uint16_t s_raw_char_handle;
static esp_gatt_if_t s_raw_gattc_if;
static uint8_t s_raw_init_step;

static void publish_buttons(uint32_t buttons);

static uint16_t crc16_modbus(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xa001) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static esp_err_t ff13_write_request(uint8_t type, uint8_t p1, uint8_t p2,
                                    uint32_t total, const uint8_t *data,
                                    uint16_t data_length)
{
    uint8_t frame[32] = {0};
    if (data_length > sizeof(frame) - 17) {
        return ESP_ERR_INVALID_SIZE;
    }
    frame[0] = 0x04;
    frame[1] = type;
    frame[3] = p1;
    frame[4] = p2;
    frame[5] = data_length & 0xff;
    frame[6] = data_length >> 8;
    uint16_t crc = crc16_modbus(data, data_length);
    frame[7] = crc & 0xff;
    frame[8] = crc >> 8;
    frame[9] = total & 0xff;
    frame[10] = (total >> 8) & 0xff;
    frame[11] = (total >> 16) & 0xff;
    frame[12] = (total >> 24) & 0xff;
    if (data_length != 0) {
        memcpy(&frame[17], data, data_length);
    }
    ESP_LOGI(TAG, "FF13 write type=0x%02x p1=%u p2=%u len=%u", type, p1, p2,
             data_length);
    return esp_ble_gattc_write_char(s_raw_gattc_if, s_raw_conn_id,
                                    s_raw_char_handle, 17 + data_length, frame,
                                    ESP_GATT_WRITE_TYPE_RSP,
                                    ESP_GATT_AUTH_REQ_NONE);
}

static uint32_t ff13_button_state(const uint8_t *frame, size_t length)
{
    if (length < 19 || frame[0] != 0x04 || frame[2] != 0x03) {
        return 0;
    }
    const uint8_t *state = &frame[16];
    static uint32_t previous_raw = UINT32_MAX;
    uint32_t raw = state[0] | ((uint32_t)state[1] << 8) | ((uint32_t)state[2] << 16);
    if (raw != previous_raw) {
        ESP_LOGI(TAG, "raw buttons=%06lx", (unsigned long)raw);
        previous_raw = raw;
    }
    uint32_t buttons = 0;
    if (state[1] & 0x02) buttons |= MSX_RUNNER_BUTTON_UP;
    if (state[1] & 0x01) buttons |= MSX_RUNNER_BUTTON_DOWN;
    if (state[0] & 0x80) buttons |= MSX_RUNNER_BUTTON_LEFT;
    if (state[0] & 0x40) buttons |= MSX_RUNNER_BUTTON_RIGHT;
    if (state[1] & 0x40) buttons |= WATCH_BUTTON_L2; /* Captured physical L2: 004000. */
    if (state[1] & 0x20) buttons |= MSX_RUNNER_BUTTON_A;
    if (state[1] & 0x10) buttons |= MSX_RUNNER_BUTTON_B;
    if (state[0] & 0x08) buttons |= MSX_RUNNER_BUTTON_START;
    if (state[0] & 0x01) buttons |= MSX_RUNNER_BUTTON_SELECT;
    return buttons;
}

static void uuid_to_string(const esp_bt_uuid_t *uuid, char *out, size_t out_size)
{
    if (uuid->len == ESP_UUID_LEN_16) {
        snprintf(out, out_size, "%04x", uuid->uuid.uuid16);
    } else if (uuid->len == ESP_UUID_LEN_32) {
        snprintf(out, out_size, "%08lx", (unsigned long)uuid->uuid.uuid32);
    } else if (uuid->len == ESP_UUID_LEN_128) {
        size_t used = 0;
        for (int i = ESP_UUID_LEN_128 - 1; i >= 0 && used + 2 < out_size; --i) {
            used += (size_t)snprintf(out + used, out_size - used, "%02x", uuid->uuid.uuid128[i]);
        }
    } else {
        snprintf(out, out_size, "len%u", uuid->len);
    }
}

/*
 * 80EL advertises over BLE even in controller modes that do not expose the
 * standard BLE HID service.  Keep the normal HID handler, but inspect the
 * completed GATT cache before it closes such a connection.  This tells us
 * whether a proprietary notify characteristic can carry controller reports.
 */
static uint16_t log_gatt_database(esp_gatt_if_t gattc_if, uint16_t conn_id)
{
    uint16_t ff13_handle = 0;
    const uint16_t service_capacity = 16;
    esp_gattc_service_elem_t *services = calloc(service_capacity, sizeof(*services));
    if (services == NULL) {
        ESP_LOGE(TAG, "GATT probe service allocation failed");
        return 0;
    }
    uint16_t service_count = service_capacity;
    esp_gatt_status_t status = esp_ble_gattc_get_service(
        gattc_if, conn_id, NULL, services, &service_count, 0);
    ESP_LOGI(TAG, "GATT probe conn=%u status=0x%x services=%u",
             conn_id, status, service_count);
    if (status != ESP_GATT_OK) {
        free(services);
        return 0;
    }

    for (uint16_t i = 0; i < service_count; ++i) {
        char uuid[40] = {0};
        uuid_to_string(&services[i].uuid, uuid, sizeof(uuid));
        ESP_LOGI(TAG, "GATT service uuid=%s handles=%u-%u primary=%u", uuid,
                 services[i].start_handle, services[i].end_handle,
                 services[i].is_primary);

        const uint16_t char_capacity = 32;
        esp_gattc_char_elem_t *chars = calloc(char_capacity, sizeof(*chars));
        if (chars == NULL) {
            continue;
        }
        uint16_t char_count = char_capacity;
        status = esp_ble_gattc_get_all_char(gattc_if, conn_id,
                                            services[i].start_handle,
                                            services[i].end_handle,
                                            chars, &char_count, 0);
        if (status != ESP_GATT_OK) {
            free(chars);
            continue;
        }
        for (uint16_t j = 0; j < char_count; ++j) {
            char char_uuid[40] = {0};
            uuid_to_string(&chars[j].uuid, char_uuid, sizeof(char_uuid));
            ESP_LOGI(TAG, "GATT char uuid=%s handle=%u props=0x%02x%s",
                     char_uuid, chars[j].char_handle, chars[j].properties,
                     (chars[j].properties & (ESP_GATT_CHAR_PROP_BIT_NOTIFY |
                                             ESP_GATT_CHAR_PROP_BIT_INDICATE))
                         ? " NOTIFY" : "");
            if (chars[j].uuid.len == ESP_UUID_LEN_16 &&
                chars[j].uuid.uuid.uuid16 == 0xff13 &&
                (chars[j].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY)) {
                ff13_handle = chars[j].char_handle;
            }
        }
        free(chars);
    }
    free(services);
    return ff13_handle;
}

static void gattc_probe_callback(esp_gattc_cb_event_t event,
                                 esp_gatt_if_t gattc_if,
                                 esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_SEARCH_RES_EVT) {
        char uuid[40] = {0};
        uuid_to_string(&param->search_res.srvc_id.uuid, uuid, sizeof(uuid));
        ESP_LOGI(TAG, "GATT discovered uuid=%s handles=%u-%u", uuid,
                 param->search_res.start_handle, param->search_res.end_handle);
    } else if (event == ESP_GATTC_SEARCH_CMPL_EVT &&
               param->search_cmpl.status == ESP_GATT_OK) {
        uint16_t ff13 = log_gatt_database(gattc_if, param->search_cmpl.conn_id);
        if (ff13 != 0) {
            s_raw_ff13_active = true;
            s_raw_conn_id = param->search_cmpl.conn_id;
            s_raw_char_handle = ff13;
            s_raw_gattc_if = gattc_if;
            s_raw_init_step = 0;
            s_connected = true;
            s_opening = false;
            esp_err_t ret = esp_ble_gattc_register_for_notify(
                gattc_if, s_target_bda, s_raw_char_handle);
            ESP_LOGI(TAG, "80EL FF13 raw channel selected; notify registration=%s",
                     esp_err_to_name(ret));
            return; /* Keep the proprietary connection open instead of HID-closing it. */
        }
    } else if (event == ESP_GATTC_REG_FOR_NOTIFY_EVT && s_raw_ff13_active &&
               param->reg_for_notify.handle == s_raw_char_handle) {
        ESP_LOGI(TAG, "FF13 notify registration status=0x%x",
                 param->reg_for_notify.status);
        if (param->reg_for_notify.status == ESP_GATT_OK) {
            esp_bt_uuid_t ccc_uuid = {
                .len = ESP_UUID_LEN_16,
                .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG,
            };
            esp_gattc_descr_elem_t descriptor;
            uint16_t count = 1;
            esp_gatt_status_t status = esp_ble_gattc_get_descr_by_char_handle(
                gattc_if, s_raw_conn_id, s_raw_char_handle, ccc_uuid,
                &descriptor, &count);
            if (status == ESP_GATT_OK && count == 1) {
                uint16_t notify_enable = 1;
                esp_err_t ret = esp_ble_gattc_write_char_descr(
                    gattc_if, s_raw_conn_id, descriptor.handle,
                    sizeof(notify_enable), (uint8_t *)&notify_enable,
                    ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
                ESP_LOGI(TAG, "FF13 CCC handle=%u write=%s", descriptor.handle,
                         esp_err_to_name(ret));
            } else {
                ESP_LOGW(TAG, "FF13 CCC not found status=0x%x count=%u", status, count);
            }
        }
        return;
    } else if (event == ESP_GATTC_WRITE_DESCR_EVT && s_raw_ff13_active) {
        ESP_LOGI(TAG, "FF13 notifications enabled status=0x%x", param->write.status);
        if (param->write.status == ESP_GATT_OK) {
            s_raw_init_step = 1;
            esp_err_t ret = ff13_write_request(0x5a, 0, 0, 1,
                                               (const uint8_t[]){0}, 1);
            ESP_LOGI(TAG, "FF13 hello=%s", esp_err_to_name(ret));
        }
        return;
    } else if (event == ESP_GATTC_WRITE_CHAR_EVT && s_raw_ff13_active) {
        ESP_LOGI(TAG, "FF13 command step=%u status=0x%x", s_raw_init_step,
                 param->write.status);
        if (param->write.status != ESP_GATT_OK) {
            return;
        }
        if (s_raw_init_step == 1) {
            s_raw_init_step = 2;
            esp_err_t ret = ff13_write_request(0x50, 1, 5, 0, NULL, 0);
            ESP_LOGI(TAG, "FF13 realtime enable=%s", esp_err_to_name(ret));
        } else if (s_raw_init_step == 2) {
            s_raw_init_step = 3;
            const uint8_t empty_state[12] = {0};
            esp_err_t ret = ff13_write_request(0x03, 0, 0,
                                               sizeof(empty_state), empty_state,
                                               sizeof(empty_state));
            ESP_LOGI(TAG, "FF13 realtime seed=%s", esp_err_to_name(ret));
        } else if (s_raw_init_step == 3) {
            s_raw_init_step = 4;
            ESP_LOGI(TAG, "FF13 realtime button stream ready");
        }
        return;
    } else if (event == ESP_GATTC_NOTIFY_EVT && s_raw_ff13_active &&
               param->notify.handle == s_raw_char_handle) {
        if (param->notify.value_len >= 3 && param->notify.value[0] == 0x04 &&
            param->notify.value[2] == 0x03) {
            publish_buttons(ff13_button_state(param->notify.value,
                                              param->notify.value_len));
            return;
        }
        /* A 12-byte realtime state is split at the default ATT MTU. The
         * trailing eight bytes contain timing/telemetry, not button bits. */
        if (param->notify.value_len == 8) {
            return;
        }
        ESP_LOGI(TAG, "FF13 %s len=%u",
                 param->notify.is_notify ? "notify" : "indicate",
                 param->notify.value_len);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->notify.value,
                                 param->notify.value_len, ESP_LOG_INFO);
        return;
    } else if (event == ESP_GATTC_DISCONNECT_EVT && s_raw_ff13_active &&
               param->disconnect.conn_id == s_raw_conn_id) {
        ESP_LOGW(TAG, "FF13 disconnected reason=0x%x", param->disconnect.reason);
        s_raw_ff13_active = false;
        s_connected = false;
        s_opening = false;
    }
    esp_hidh_gattc_event_handler(event, gattc_if, param);
}

static bool is_8bitdo(const char *name)
{
    return name != NULL && (strstr(name, "8BitDo") != NULL ||
                            strstr(name, "8BITDO") != NULL ||
                            strcmp(name, "80EL") == 0);
}

static uint32_t keyboard_buttons(const uint8_t *data, size_t length)
{
    if (length < 8) {
        return 0;
    }
    uint32_t buttons = 0;
    for (size_t i = 2; i < length; ++i) {
        switch (data[i]) {
        case 0x52: buttons |= MSX_RUNNER_BUTTON_UP; break;
        case 0x51: buttons |= MSX_RUNNER_BUTTON_DOWN; break;
        case 0x50: buttons |= MSX_RUNNER_BUTTON_LEFT; break;
        case 0x4f: buttons |= MSX_RUNNER_BUTTON_RIGHT; break;
        case 0x1d: buttons |= MSX_RUNNER_BUTTON_A; break;      /* Z */
        case 0x1b: buttons |= MSX_RUNNER_BUTTON_B; break;      /* X */
        case 0x28: buttons |= MSX_RUNNER_BUTTON_START; break;  /* Enter */
        case 0x2c: buttons |= MSX_RUNNER_BUTTON_SELECT; break; /* Space */
        default: break;
        }
    }
    return buttons;
}

/*
 * D-input mode commonly exposes four centred 8-bit axes followed by a hat and
 * button bits.  The exact Micro layout is logged on every change so the
 * physical controller can be calibrated without hiding unknown reports.
 */
static uint32_t dinput_buttons(const uint8_t *data, size_t length)
{
    uint32_t buttons = 0;
    if (length >= 2) {
        if (data[0] < 0x40) buttons |= MSX_RUNNER_BUTTON_LEFT;
        if (data[0] > 0xc0) buttons |= MSX_RUNNER_BUTTON_RIGHT;
        if (data[1] < 0x40) buttons |= MSX_RUNNER_BUTTON_UP;
        if (data[1] > 0xc0) buttons |= MSX_RUNNER_BUTTON_DOWN;
    }
    if (length >= 5) {
        const uint8_t hat = data[4] & 0x0f;
        if (hat == 0 || hat == 1 || hat == 7) buttons |= MSX_RUNNER_BUTTON_UP;
        if (hat == 1 || hat == 2 || hat == 3) buttons |= MSX_RUNNER_BUTTON_RIGHT;
        if (hat == 3 || hat == 4 || hat == 5) buttons |= MSX_RUNNER_BUTTON_DOWN;
        if (hat == 5 || hat == 6 || hat == 7) buttons |= MSX_RUNNER_BUTTON_LEFT;
        const uint8_t face = data[4] >> 4;
        if (face & 0x01) buttons |= MSX_RUNNER_BUTTON_A;
        if (face & 0x02) buttons |= MSX_RUNNER_BUTTON_B;
        if (face & 0x04) buttons |= MSX_RUNNER_BUTTON_B;
        if (face & 0x08) buttons |= MSX_RUNNER_BUTTON_A;
    }
    if (length >= 6) {
        if (data[5] & 0x01) buttons |= MSX_RUNNER_BUTTON_A;
        if (data[5] & 0x02) buttons |= MSX_RUNNER_BUTTON_B;
        if (data[5] & 0x10) buttons |= MSX_RUNNER_BUTTON_SELECT;
        if (data[5] & 0x20) buttons |= MSX_RUNNER_BUTTON_START;
    }
    if (length >= 7) {
        if (data[6] & 0x01) buttons |= MSX_RUNNER_BUTTON_SELECT;
        if (data[6] & 0x02) buttons |= MSX_RUNNER_BUTTON_START;
    }
    return buttons;
}

static void publish_buttons(uint32_t buttons)
{
    if (buttons == s_last_buttons) {
        return;
    }
    s_last_buttons = buttons;
    msx_runner_set_buttons(buttons);
    ESP_LOGI(TAG, "MSX buttons=0x%03lx", (unsigned long)buttons);
}

static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id,
                          void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch ((esp_hidh_event_t)id) {
    case ESP_HIDH_OPEN_EVENT:
        s_opening = false;
        if (param->open.status == ESP_OK) {
            s_connected = true;
            ESP_LOGI(TAG, "connected: %s", esp_hidh_dev_name_get(param->open.dev));
            esp_hidh_dev_dump(param->open.dev, stdout);
        } else {
            ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(param->open.status));
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        ESP_LOGI(TAG, "battery=%u%%", param->battery.level);
        break;
    case ESP_HIDH_INPUT_EVENT: {
        ESP_LOGI(TAG, "input usage=%s map=%u id=%u len=%u",
                 esp_hid_usage_str(param->input.usage), param->input.map_index,
                 param->input.report_id, param->input.length);
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, param->input.data, param->input.length, ESP_LOG_INFO);
        uint32_t buttons = param->input.usage == ESP_HID_USAGE_KEYBOARD
                               ? keyboard_buttons(param->input.data, param->input.length)
                               : dinput_buttons(param->input.data, param->input.length);
        publish_buttons(buttons);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGW(TAG, "disconnected: reason=%d", param->close.reason);
        publish_buttons(0);
        s_connected = false;
        s_opening = false;
        esp_hidh_dev_free(param->close.dev);
        break;
    default:
        break;
    }
}

static void scan_task(void *argument)
{
    (void)argument;
    while (true) {
        if (!s_connected && !s_opening) {
            size_t count = 0;
            esp_hid_scan_result_t *results = NULL;
            ESP_LOGI(TAG, "scanning for 8BitDo Micro...");
            esp_err_t ret = esp_hid_scan(SCAN_SECONDS, &count, &results);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(ret));
            } else {
                ESP_LOGI(TAG, "scan complete: %u HID device(s)", (unsigned)count);
                for (esp_hid_scan_result_t *item = results; item != NULL; item = item->next) {
                    ESP_LOGI(TAG, "candidate name='%s' rssi=%d usage=%s",
                             item->name ? item->name : "", item->rssi,
                             esp_hid_usage_str(item->usage));
                    if (!s_opening && is_8bitdo(item->name)) {
                        memcpy(s_target_bda, item->bda, sizeof(s_target_bda));
                        esp_hidh_dev_t *dev = esp_hidh_dev_open(
                            item->bda, item->transport, item->ble.addr_type);
                        if (dev != NULL) {
                            s_opening = true;
                            ESP_LOGI(TAG, "opening %s", item->name);
                        } else {
                            ESP_LOGW(TAG, "open request failed");
                        }
                    }
                }
                esp_hid_scan_results_free(results);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(RESCAN_DELAY_MS));
    }
}

esp_err_t controller_start(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_hid_gap_init(HIDH_BLE_MODE);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = esp_ble_gattc_register_callback(gattc_probe_callback);
    if (ret != ESP_OK) {
        return ret;
    }
    const esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 6144,
        .callback_arg = NULL,
    };
    ret = esp_hidh_init(&config);
    if (ret != ESP_OK) {
        return ret;
    }
    if (xTaskCreate(scan_task, "ble_hid_scan", 6144, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "BLE HID host ready");
    return ESP_OK;
}
