#include "serial_console.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"
#include "wifi_uploader.h"
#include "msx_runner.h"
#include "app_state.h"
#include "rom_filename.h"
#include "watch_theme.h"
#include "watch_controls.h"
#include "display_sync.h"

#define ROM_DIRECTORY "/sdcard/msx/roms"
#define BIOS_DIRECTORY "/sdcard/msx/bios"
#define LINE_CAPACITY 1152
#define DATA_CAPACITY 512

static const char *TAG = "serial_console";
static bool s_sd_ready;
extern esp_err_t app_console_launch_rom(const char *name, bool msx1);
extern void app_console_display_test(void);
extern void app_console_display_bench(int mode);
extern void app_console_video_mode(int mode);
extern void app_console_video_phase(int microseconds);
extern void app_console_launcher(void);
extern void app_console_capture_frame(void);
extern void app_console_bench_status(void);
extern void app_console_rom_browser(void);
extern void app_console_wifi_upload(void);
extern esp_err_t app_console_theme(bool basic);
extern void app_console_font_check(unsigned codepoint);

static int read_line(char *line, size_t capacity)
{
    size_t length = 0;
    while (true) {
        const int value = getchar();
        if (value == EOF) {
            clearerr(stdin);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (value == '\r') {
            continue;
        }
        if (value == '\n') {
            line[length] = '\0';
            return (int)length;
        }
        if (length + 1 >= capacity) {
            while (true) {
                const int discard = getchar();
                if (discard == '\n' || discard == EOF) {
                    break;
                }
            }
            line[0] = '\0';
            return -1;
        }
        line[length++] = (char)value;
    }
}

static int hex_nibble(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool decode_hex(const char *text, uint8_t *output, size_t capacity, size_t *written)
{
    const size_t length = strlen(text);
    if ((length & 1U) != 0 || length / 2 > capacity) {
        return false;
    }
    for (size_t i = 0; i < length / 2; ++i) {
        const int high = hex_nibble(text[i * 2]);
        const int low = hex_nibble(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (uint8_t)((high << 4) | low);
    }
    *written = length / 2;
    return true;
}

static bool valid_filename(const char *name) { return rom_filename_valid(name); }

static bool parse_digest(const char *text, uint8_t digest[32])
{
    if (strlen(text) != 64) {
        return false;
    }
    size_t written = 0;
    return decode_hex(text, digest, 32, &written) && written == 32;
}

static void hash_rom(const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/%s", ROM_DIRECTORY, name);
    FILE *file = fopen(path, "rb");
    if (!file) { printf("HASH ERROR open\n"); fflush(stdout); return; }
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    int result = mbedtls_sha256_starts(&sha, 0);
    unsigned char buffer[512], digest[32];
    size_t count, total = 0;
    while (result == 0 && (count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        total += count;
        result = mbedtls_sha256_update(&sha, buffer, count);
    }
    if (ferror(file)) result = -1;
    if (fclose(file) != 0) result = -1;
    if (result == 0) result = mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);
    if (result) printf("HASH ERROR read\n");
    else {
        printf("HASH OK %s %u ", name, (unsigned)total);
        for (size_t i = 0; i < sizeof(digest); ++i) printf("%02x", digest[i]);
        printf("\n");
    }
    fflush(stdout);
}

static __attribute__((noinline)) void upload_file(const char *directory, const char *filename,
                                                  size_t expected_size,
                                                  const char *digest_text)
{
    char temp_path[160];
    snprintf(temp_path, sizeof(temp_path), "%s/.upload.tmp", directory);
    uint8_t expected_digest[32];
    if (!s_sd_ready) {
        printf("UPLOAD ERROR sd-not-mounted\n");
        return;
    }
    if (!valid_filename(filename) || expected_size == 0 || expected_size > 4U * 1024U * 1024U ||
        !parse_digest(digest_text, expected_digest)) {
        printf("UPLOAD ERROR invalid-request\n");
        return;
    }

    FILE *file = fopen(temp_path, "wb");
    if (file == NULL) {
        printf("UPLOAD ERROR open-%d\n", errno);
        return;
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (mbedtls_sha256_starts(&sha, 0) != 0) {
        fclose(file);
        unlink(temp_path);
        mbedtls_sha256_free(&sha);
        printf("UPLOAD ERROR sha-init\n");
        return;
    }

    printf("UPLOAD READY\n");
    fflush(stdout);
    char line[LINE_CAPACITY];
    uint8_t data[DATA_CAPACITY];
    size_t received = 0;
    bool failed = false;

    while (true) {
        const int line_length = read_line(line, sizeof(line));
        if (line_length < 0) {
            printf("UPLOAD ERROR line-too-long\n");
            failed = true;
            break;
        }
        if (strcmp(line, "END") == 0) {
            break;
        }
        if (line[0] != 'D') {
            printf("UPLOAD ERROR expected-data\n");
            failed = true;
            break;
        }
        size_t decoded = 0;
        if (!decode_hex(line + 1, data, sizeof(data), &decoded) || decoded == 0 ||
            received + decoded > expected_size || fwrite(data, 1, decoded, file) != decoded ||
            mbedtls_sha256_update(&sha, data, decoded) != 0) {
            printf("UPLOAD ERROR data\n");
            failed = true;
            break;
        }
        received += decoded;
        printf("UPLOAD ACK %u\n", (unsigned)received);
        fflush(stdout);
    }

    uint8_t actual_digest[32] = {0};
    if (!failed && (received != expected_size || mbedtls_sha256_finish(&sha, actual_digest) != 0 ||
                    memcmp(actual_digest, expected_digest, sizeof(actual_digest)) != 0)) {
        failed = true;
        printf("UPLOAD ERROR verify\n");
    }
    mbedtls_sha256_free(&sha);

    if (fflush(file) != 0 || fsync(fileno(file)) != 0 || fclose(file) != 0) {
        failed = true;
        printf("UPLOAD ERROR flush\n");
    }
    if (failed) {
        unlink(temp_path);
        fflush(stdout);
        return;
    }

    char target[160];
    char backup[sizeof(target) + 5];
    snprintf(target, sizeof(target), "%s/%s", directory, filename);
    snprintf(backup, sizeof(backup), "%s.bak", target);
    unlink(backup);
    const bool had_target = rename(target, backup) == 0;
    if (rename(temp_path, target) != 0) {
        if (had_target) {
            rename(backup, target);
        }
        unlink(temp_path);
        printf("UPLOAD ERROR rename-%d\n", errno);
        fflush(stdout);
        return;
    }
    if (had_target) {
        unlink(backup);
    }
    ESP_LOGI(TAG, "ROM upload complete: %s (%u bytes, SHA-256 %s)", target,
             (unsigned)received, digest_text);
    printf("UPLOAD OK %s %u %s\n", filename, (unsigned)received, digest_text);
    fflush(stdout);
}

static void console_task(void *argument)
{
    (void)argument;
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    ESP_LOGI(TAG, "Ready: ROM/BIOS upload and private Wi-Fi configuration over USB");

    char line[LINE_CAPACITY];
    while (true) {
        const int length = read_line(line, sizeof(line));
        if (length < 0) {
            printf("CONSOLE ERROR line-too-long\n");
            continue;
        }
        if (strcasecmp(line, "d") == 0) {
            ESP_LOGW(TAG, "Entering ROM download mode");
            vTaskDelay(pdMS_TO_TICKS(100));
            REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);
            esp_restart();
        }

        char filename[129];
        char digest[65];
        size_t size = 0;
        char extra = '\0';
        char ssid_hex[65];
        char password_hex[129];
        unsigned buttons;
        char encoded_name[257];
        if(strcmp(line,"bench-status")==0) {
            app_console_bench_status();
        } else if(strcmp(line,"te-status")==0) {
            display_sync_stats_t stats;display_sync_stats(&stats);
            printf("TE READY=%d RISES=%lu PERIOD_US=%lu MIN_US=%lu MAX_US=%lu HIGH_US=%lu\n",
                display_sync_available(),(unsigned long)stats.rises,(unsigned long)stats.period_us,
                (unsigned long)stats.min_us,(unsigned long)stats.max_us,(unsigned long)stats.high_us);
            fflush(stdout);
        } else if (strcmp(line,"display-bench full")==0 || strcmp(line,"display-bench partial")==0 || strcmp(line,"display-bench stock")==0 || strcmp(line,"display-bench sync")==0) {
            int mode=strcmp(line,"display-bench full")==0?0:strcmp(line,"display-bench partial")==0?1:strcmp(line,"display-bench stock")==0?2:3;
            if(mode==3&&!display_sync_available()) printf("DISPLAY BENCH ERROR no-te\n");
            else {app_console_display_bench(mode);printf("DISPLAY BENCH OK\n");}
            fflush(stdout);
        } else if (strcmp(line,"capture-frame")==0) {
            app_console_capture_frame();
        } else if (sscanf(line, "state-save %u %c", &buttons, &extra) == 1 ||
            sscanf(line, "state-load %u %c", &buttons, &extra) == 1) {
            printf("STATE QUEUED %s\n", esp_err_to_name(msx_runner_state_request(strncmp(line,"state-save ",11)==0, buttons)));
            fflush(stdout);
        } else if (strcmp(line,"video stock")==0 || strcmp(line,"video buffered")==0 || strcmp(line,"video stream")==0 || strcmp(line,"video sync")==0) {
            int mode=strcmp(line,"video sync")==0?3:strcmp(line,"video stock")==0?0:strcmp(line,"video stream")==0?1:2;
            app_console_video_mode(mode);
            printf("VIDEO MODE %s\n",mode==0? "STOCK":mode==1? "STREAM":mode==3?"SYNC":"BUFFERED");
        } else if (sscanf(line,"video-phase %u %c",&buttons,&extra)==1 && buttons<=16000) {
            app_console_video_phase((int)buttons);
        } else if(strcmp(line,"video-phase")==0) {
            app_console_video_phase(-1);
        } else if (sscanf(line, "msx-render %u %c", &buttons, &extra) == 1 && (buttons == 50 || buttons == 75 || buttons == 100)) {
            msx_runner_set_render_percent(buttons);
            printf("RENDER REQUEST %u\n", buttons);
        } else if (strcmp(line, "levels") == 0) {
            printf("LEVELS volume=%d brightness=%d\n", watch_controls_volume(), watch_controls_brightness());
            fflush(stdout);
        } else if (strcmp(line, "ui-status") == 0) {
            printf("UI STATE %s THEME %s FONT CBIOS32\n", app_state_name(app_state_get()),
                   watch_theme_get() == WATCH_BASIC ? "BASIC" : "CARTRIDGE");
            fflush(stdout);
        } else if (sscanf(line, "font-check %x %c", &buttons, &extra) == 1) {
            app_console_font_check(buttons);
        } else if (strcmp(line, "theme cartridge") == 0 || strcmp(line, "theme basic") == 0) {
            printf("THEME %s\n", esp_err_to_name(app_console_theme(strcmp(line, "theme basic") == 0)));
            fflush(stdout);
        } else if (strcmp(line, "rom-list") == 0) {
            app_console_rom_browser();
            printf("ROM LIST OK\n"); fflush(stdout);
        } else if (strcmp(line, "wifi-upload") == 0) {
            app_console_wifi_upload();
            printf("WIFI UPLOAD OK\n"); fflush(stdout);
        } else if ((sscanf(line, "run-msx1-utf8 %256s %c", encoded_name, &extra) == 1 ||
                    sscanf(line, "run-rom-utf8 %256s %c", encoded_name, &extra) == 1) &&
                   rom_filename_decode_hex(encoded_name, filename, sizeof(filename))) {
            printf("RUN ROM %s\n", esp_err_to_name(app_console_launch_rom(filename, strncmp(line, "run-msx1-utf8 ", 14) == 0)));
            fflush(stdout);
        } else if (sscanf(line, "hash-rom-utf8 %256s %c", encoded_name, &extra) == 1 &&
                   rom_filename_decode_hex(encoded_name, filename, sizeof(filename))) {
            hash_rom(filename);
        } else if (sscanf(line, "upload-rom-utf8 %256s %zu %64s %c", encoded_name, &size, digest, &extra) == 3 &&
                   rom_filename_decode_hex(encoded_name, filename, sizeof(filename))) {
            upload_file(ROM_DIRECTORY, filename, size, digest);
        } else if (sscanf(line, "upload-bios-utf8 %256s %zu %64s %c", encoded_name, &size, digest, &extra) == 3 &&
                   rom_filename_decode_hex(encoded_name, filename, sizeof(filename))) {
            upload_file(BIOS_DIRECTORY, filename, size, digest);
        } else if (sscanf(line, "msx-buttons %x %c", &buttons, &extra) == 1 && buttons <= 0x1ff &&
            app_state_get() == APP_MSX && msx_runner_is_running()) {
            msx_runner_set_serial_buttons(buttons);
            printf("MSX BUTTONS OK\n");
            fflush(stdout);
        } else if ((sscanf(line, "disk-a %128s %c", filename, &extra) == 1 ||
                    sscanf(line, "disk-b %128s %c", filename, &extra) == 1) &&
                   valid_filename(filename) &&
                   (strrchr(filename, '.') != NULL) &&
                   !strcasecmp(strrchr(filename, '.'), ".dsk")) {
            const int drive = strncmp(line, "disk-b ", 7) == 0 ? 1 : 0;
            printf("DISK QUEUED %s\n",
                   esp_err_to_name(msx_runner_disk_request(drive, filename)));
            fflush(stdout);
        } else if (strcmp(line, "launcher") == 0) {
            app_console_launcher();
            printf("LAUNCHER OK\n");
            fflush(stdout);
        } else if (strcmp(line, "display-test") == 0) {
            app_console_display_test();
            printf("DISPLAY TEST OK\n");
            fflush(stdout);
        } else if ((sscanf(line, "run-rom %128s %c", filename, &extra) == 1 ||
             sscanf(line, "run-msx1 %128s %c", filename, &extra) == 1) && valid_filename(filename)) {
            const esp_err_t result = app_console_launch_rom(filename, strncmp(line, "run-msx1 ", 9) == 0);
            printf("RUN ROM %s\n", esp_err_to_name(result));
            fflush(stdout);
        } else if (sscanf(line, "hash-rom %128s %c", filename, &extra) == 1 && valid_filename(filename)) {
            hash_rom(filename);
        } else if (sscanf(line, "wifi-config %64s %128s %c", ssid_hex, password_hex, &extra) == 2) {
            uint8_t ssid[33] = {0};
            uint8_t password[65] = {0};
            size_t ssid_size = 0;
            size_t password_size = 0;
            const bool open_network = strcmp(password_hex, "-") == 0;
            if (!decode_hex(ssid_hex, ssid, sizeof(ssid) - 1, &ssid_size) ||
                ssid_size == 0 ||
                (!open_network &&
                 !decode_hex(password_hex, password, sizeof(password) - 1, &password_size))) {
                printf("WIFI CONFIG ERROR invalid-request\n");
            } else {
                const esp_err_t result = wifi_uploader_save_credentials(
                    (const char *)ssid, (const char *)password);
                memset(password, 0, sizeof(password));
                if (result == ESP_OK) {
                    printf("WIFI CONFIG OK\n");
                } else {
                    printf("WIFI CONFIG ERROR save-%s\n", esp_err_to_name(result));
                }
            }
            fflush(stdout);
        } else if (sscanf(line, "upload-rom %128s %zu %64s %c", filename, &size, digest,
                          &extra) == 3) {
            upload_file(ROM_DIRECTORY, filename, size, digest);
        } else if (sscanf(line, "upload-bios %128s %zu %64s %c", filename, &size, digest,
                          &extra) == 3) {
            upload_file(BIOS_DIRECTORY, filename, size, digest);
        } else if (length > 0) {
            printf("CONSOLE ERROR unknown-command\n");
        }
    }
}

esp_err_t serial_console_start(bool sd_ready)
{
    s_sd_ready = sd_ready;
    return xTaskCreate(console_task, "serial_console", 12288, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
