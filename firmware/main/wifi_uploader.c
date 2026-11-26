#include "rom_filename.h"
#include "wifi_uploader.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

#define ROM_DIRECTORY "/sdcard/msx/roms"
#define BIOS_DIRECTORY "/sdcard/msx/bios"
#define WIFI_NAMESPACE "wifi_upload"
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAILED_BIT BIT1
#define WIFI_CANCEL_BIT BIT2
#define WIFI_CONNECT_RETRIES 5
#define MAX_UPLOAD_SIZE (4U * 1024U * 1024U)

static const char *TAG = "wifi_uploader";
static const char s_index_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>RetroS3 Watch Upload</title><style>"
    "body{font:16px system-ui;max-width:620px;margin:40px auto;padding:0 18px;background:#07111d;color:#eef7ff}"
    "main{background:#122238;padding:24px;border-radius:16px}input,select,button{font:inherit;margin:8px 0;padding:10px;width:100%;box-sizing:border-box}"
    "button{background:#1689d8;color:white;border:0;border-radius:8px}pre{white-space:pre-wrap}</style></head>"
    "<body><main><h1>RetroS3 Watch Upload</h1>"
    "<p>Upload a .rom file to microSD. The watch verifies the written file with SHA-256 before installing it.</p>"
    "<select id=kind><option value=rom>Game ROM</option><option value=bios>BIOS ROM</option></select>"
    "<input id=file type=file accept=.rom><button id=send>Upload</button><pre id=status>Ready</pre>"
    "<script>const send=document.getElementById('send'),file=document.getElementById('file'),kind=document.getElementById('kind'),status=document.getElementById('status');"
    "send.onclick=async()=>{const f=file.files[0];if(!f){status.textContent='Choose a file';return;}"
    "status.textContent='Uploading '+f.name+'...';send.disabled=true;try{const u='/upload?kind='+kind.value+'&name='+encodeURIComponent(f.name.normalize('NFC'));"
    "const r=await fetch(u,{method:'POST',headers:{'Content-Type':'application/octet-stream'},body:f});"
    "status.textContent=await r.text();if(!r.ok)throw new Error(status.textContent);}catch(e){status.textContent='Error: '+e.message;}finally{send.disabled=false;}};</script>"
    "</main></body></html>";

static httpd_handle_t s_server;
static esp_netif_t *s_netif;
static EventGroupHandle_t s_wifi_events;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static bool s_wifi_initialized;
static bool s_wifi_active;
static int s_connect_retries;
static char s_ssid[33];
static char s_ip[16];

static bool valid_filename(const char *name) { return rom_filename_valid(name); }

static bool decode_uri_component(const char *input, char *output, size_t capacity)
{
    size_t written = 0;
    while (*input != '\0') {
        if (written + 1 >= capacity) {
            return false;
        }
        if (*input == '%') {
            if (input[1] == '\0' || input[2] == '\0' ||
                !isxdigit((unsigned char)input[1]) || !isxdigit((unsigned char)input[2])) {
                return false;
            }
            char encoded[3] = {input[1], input[2], '\0'};
            const unsigned long decoded = strtoul(encoded, NULL, 16);
            if (decoded == 0) {
                return false;
            }
            output[written++] = (char)decoded;
            input += 3;
        } else {
            output[written++] = *input == '+' ? ' ' : *input;
            ++input;
        }
    }
    output[written] = '\0';
    return true;
}

static esp_err_t sha256_file(const char *path, uint8_t digest[32])
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return ESP_FAIL;
    }
    uint8_t *buffer = malloc(4096);
    if (buffer == NULL) {
        fclose(file);
        return ESP_ERR_NO_MEM;
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int result = mbedtls_sha256_starts(&sha, 0);
    while (result == 0) {
        const size_t count = fread(buffer, 1, 4096, file);
        if (count > 0) {
            result = mbedtls_sha256_update(&sha, buffer, count);
        }
        if (count < 4096) {
            if (ferror(file)) {
                result = -1;
            }
            break;
        }
    }
    if (result == 0) {
        result = mbedtls_sha256_finish(&sha, digest);
    }
    mbedtls_sha256_free(&sha);
    free(buffer);
    fclose(file);
    return result == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t index_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, s_index_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t upload_handler(httpd_req_t *request)
{
    char query[640];
    char kind[8];
    char encoded_name[400];
    char filename[129];
    const size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length >= sizeof(query) ||
        httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "kind", kind, sizeof(kind)) != ESP_OK ||
        httpd_query_key_value(query, "name", encoded_name, sizeof(encoded_name)) != ESP_OK ||
        !decode_uri_component(encoded_name, filename, sizeof(filename)) ||
        !valid_filename(filename)) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid upload request");
    }
    const char *directory = NULL;
    if (strcmp(kind, "rom") == 0) {
        directory = ROM_DIRECTORY;
    } else if (strcmp(kind, "bios") == 0) {
        directory = BIOS_DIRECTORY;
    } else {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid destination");
    }
    if (request->content_len <= 0 || (size_t)request->content_len > MAX_UPLOAD_SIZE) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "File must be 1 byte to 4 MiB");
    }

    char temporary[192];
    char target[192];
    char backup[196];
    snprintf(temporary, sizeof(temporary), "%s/.wifi-upload.tmp", directory);
    snprintf(target, sizeof(target), "%s/%s", directory, filename);
    snprintf(backup, sizeof(backup), "%s.bak", target);
    FILE *file = fopen(temporary, "wb");
    if (file == NULL) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not open microSD destination");
    }
    uint8_t *buffer = malloc(4096);
    if (buffer == NULL) {
        fclose(file);
        unlink(temporary);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int sha_result = mbedtls_sha256_starts(&sha, 0);
    int remaining = request->content_len;
    bool failed = sha_result != 0;
    while (!failed && remaining > 0) {
        int received = httpd_req_recv(request, (char *)buffer,
                                      remaining < 4096 ? remaining : 4096);
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            failed = true;
            break;
        }
        if (received <= 0 || fwrite(buffer, 1, (size_t)received, file) != (size_t)received ||
            mbedtls_sha256_update(&sha, buffer, (size_t)received) != 0) {
            failed = true;
            break;
        }
        remaining -= received;
    }
    uint8_t received_digest[32] = {0};
    if (!failed && mbedtls_sha256_finish(&sha, received_digest) != 0) {
        failed = true;
    }
    mbedtls_sha256_free(&sha);
    free(buffer);
    const int flush_result = fflush(file);
    const int sync_result = fsync(fileno(file));
    const int close_result = fclose(file);
    if (flush_result != 0 || sync_result != 0 || close_result != 0) {
        failed = true;
    }
    uint8_t readback_digest[32] = {0};
    if (!failed && (sha256_file(temporary, readback_digest) != ESP_OK ||
                    memcmp(received_digest, readback_digest, sizeof(received_digest)) != 0)) {
        failed = true;
    }
    if (failed) {
        unlink(temporary);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Transfer or microSD read-back verification failed");
    }

    unlink(backup);
    const bool had_target = rename(target, backup) == 0;
    if (rename(temporary, target) != 0) {
        if (had_target) {
            rename(backup, target);
        }
        unlink(temporary);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Could not install verified file");
    }
    if (had_target) {
        unlink(backup);
    }
    char digest_text[65];
    for (size_t i = 0; i < sizeof(readback_digest); ++i) {
        snprintf(digest_text + i * 2, 3, "%02x", readback_digest[i]);
    }
    char response[256];
    snprintf(response, sizeof(response), "Upload complete\n%s\n%d bytes\nSHA-256 %s",
             filename, request->content_len, digest_text);
    ESP_LOGI(TAG, "verified upload: %s (%d bytes, SHA-256 %s)", target,
             request->content_len, digest_text);
    httpd_resp_set_type(request, "text/plain; charset=utf-8");
    return httpd_resp_send(request, response, HTTPD_RESP_USE_STRLEN);
}

static void wifi_event_handler(void *argument, esp_event_base_t base, int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        const esp_err_t result = esp_wifi_connect();
        ESP_LOGI(TAG, "STA connect request: %s", esp_err_to_name(result));
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_wifi_active) {
            return;
        }
        const wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "STA disconnected: reason=%u retry=%d", event->reason,
                 s_connect_retries);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_connect_retries < WIFI_CONNECT_RETRIES) {
            ++s_connect_retries;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAILED_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        esp_ip4addr_ntoa(&event->ip_info.ip, s_ip, sizeof(s_ip));
        s_connect_retries = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "connected to '%s' at %s", s_ssid, s_ip);
    }
}

static esp_err_t load_credentials(char ssid[33], char password[65])
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return result;
    }
    size_t ssid_size = 33;
    size_t password_size = 65;
    result = nvs_get_str(handle, "ssid", ssid, &ssid_size);
    if (result == ESP_OK) {
        result = nvs_get_str(handle, "pass", password, &password_size);
    }
    nvs_close(handle);
    return result;
}

esp_err_t wifi_uploader_save_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || strlen(ssid) == 0 || strlen(ssid) > 32 ||
        strlen(password) > 64 || (strlen(password) > 0 && strlen(password) < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(handle, "ssid", ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, "pass", password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

bool wifi_uploader_has_credentials(void)
{
    char ssid[33];
    char password[65];
    const bool present = load_credentials(ssid, password) == ESP_OK;
    memset(password, 0, sizeof(password));
    return present;
}

esp_err_t wifi_uploader_init(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        wifi_uploader_stop();
        return ESP_ERR_NO_MEM;
    }
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    /* Leave internal DMA RAM for CO5300 SPI bounce buffers and BLE.
     * Six RX buffers match the configured receive BA window. */
    wifi_init.static_rx_buf_num = 6;
    wifi_init.static_tx_buf_num = 4;
    wifi_init.dynamic_rx_buf_num = 8;
    wifi_init.cache_tx_buf_num = 8;
    ret = esp_wifi_init(&wifi_init);
    if (ret != ESP_OK) {
        wifi_uploader_stop();
        return ret;
    }
    s_wifi_initialized = true;
    ESP_LOGI(TAG, "Wi-Fi initialized: internal free=%u largest=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &s_wifi_handler),
        fail, TAG, "Wi-Fi event registration");
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &s_ip_handler),
        fail, TAG, "IP event registration");
    return ESP_OK;
fail:
    return ret;
}

void wifi_uploader_cancel(void)
{
    if (s_wifi_events != NULL) {
        xEventGroupSetBits(s_wifi_events, WIFI_CANCEL_BIT);
    }
}

esp_err_t wifi_uploader_start(bool sd_ready)
{
    if (!sd_ready || !s_wifi_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_server != NULL) {
        return ESP_OK;
    }
    char password[65] = {0};
    esp_err_t ret = load_credentials(s_ssid, password);
    if (ret != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT | WIFI_CANCEL_BIT);
    s_wifi_active = true;
    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, s_ssid, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    config.sta.threshold.authmode = password[0] == '\0' ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    memset(password, 0, sizeof(password));
    ESP_GOTO_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), fail, TAG, "wifi storage");
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), fail, TAG, "wifi mode");
    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &config), fail, TAG, "wifi config");
    memset(&config, 0, sizeof(config));
    ESP_GOTO_ON_ERROR(esp_wifi_start(), fail, TAG, "wifi start");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT | WIFI_CANCEL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(45000));
    if ((bits & WIFI_CANCEL_BIT) != 0 || (bits & WIFI_CONNECTED_BIT) == 0) {
        ret = (bits & WIFI_CANCEL_BIT) != 0 ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
        goto fail;
    }
    httpd_config_t server_config = HTTPD_DEFAULT_CONFIG();
    server_config.stack_size = 8192;
    server_config.recv_wait_timeout = 15;
    ESP_GOTO_ON_ERROR(httpd_start(&s_server, &server_config), fail, TAG, "http server");
    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t upload_uri = {.uri = "/upload", .method = HTTP_POST,
                                    .handler = upload_handler};
    ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_server, &index_uri), fail, TAG, "index URI");
    ESP_GOTO_ON_ERROR(httpd_register_uri_handler(s_server, &upload_uri), fail, TAG, "upload URI");
    return ESP_OK;

fail:
    wifi_uploader_stop();
    return ret;
}

void wifi_uploader_stop(void)
{
    if (s_server != NULL) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    if (s_wifi_initialized) {
        s_wifi_active = false;
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    s_connect_retries = 0;
    s_ip[0] = '\0';
}

bool wifi_uploader_is_running(void)
{
    return s_server != NULL;
}

const char *wifi_uploader_ssid(void)
{
    return s_ssid;
}

const char *wifi_uploader_ip(void)
{
    return s_ip;
}

