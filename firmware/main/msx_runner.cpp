/* SPDX-License-Identifier: MIT */
#include "msx_runner.h"
#include "msx_watch_ui.h"
#include "retros3_msx.h"
#include "watch_controls.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
constexpr auto Tag = "retros3_msx";
constexpr auto BiosDir = "/sdcard/msx/bios/";
constexpr auto MediaDir = "/sdcard/msx/roms";
constexpr auto StateDir = "/sdcard/msx/states";
constexpr unsigned Width = MSX_RUNNER_VIDEO_WIDTH;
constexpr unsigned Height = MSX_RUNNER_VIDEO_HEIGHT;
constexpr unsigned TopBorder = (Height - RS_MSX_HEIGHT) / 2;
constexpr unsigned MaxImage = 4 * 1024 * 1024;
constexpr size_t PathSize = 512;
constexpr int DiskLimit = 48;

enum class Action { none, save, load, disk };
enum class Menu { root, save, load, drive_a, drive_b };
struct Request {
    Action action;
    int slot;
    char path[PathSize];
};

std::atomic<bool> running{false}, stop_requested{false}, finished{false};
std::atomic<uint32_t> gamepad{0};
std::atomic<unsigned> render_percent{100};
std::atomic<int64_t> serial_until{0};
portMUX_TYPE shared_lock = portMUX_INITIALIZER_UNLOCKED;
msx_watch_ui_t shared_ui;
Request queued;
char media_path[PathSize];
unsigned machine_model;
msx_runner_frame_cb_t frame_callback;
msx_runner_acquire_cb_t acquire_frame_callback;
msx_runner_audio_cb_t audio_callback;
void *callback_context;

struct MediaList {
    char names[DiskLimit][MSX_WATCH_UI_ITEM_BYTES];
    int count;
};

bool extension(const char *path, const char *suffix)
{
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, suffix) == 0;
}

void *read_file(const char *path, size_t &size, size_t max_size = MaxImage)
{
    FILE *file = fopen(path, "rb");
    if (!file) return nullptr;
    const long length = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    void *data = nullptr;
    if (length > 0 && static_cast<unsigned long>(length) <= max_size && !fseek(file, 0, SEEK_SET)) {
        data = heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data && fread(data, 1, length, file) != static_cast<size_t>(length)) {
            free(data);
            data = nullptr;
        }
    }
    fclose(file);
    if (data) size = static_cast<size_t>(length);
    return data;
}

bool bios(rs_msx *m, const char *name, unsigned slot, unsigned sub, unsigned page, bool disk = false)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s", BiosDir, name);
    size_t size = 0;
    void *data = read_file(path, size, 65536);
    const bool ok = data && (disk ? rs_msx_mount_disk_rom(m, slot, sub, data, size) :
                                   rs_msx_mount_rom(m, slot, sub, page, data, size));
    free(data);
    return ok;
}

bool exists(const char *prefix, const char *name)
{
    char path[96];
    struct stat info;
    snprintf(path, sizeof(path), "%s%s", prefix, name);
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

rs_cart_mapper mapper_for(const uint8_t *rom, size_t size)
{
    char path[PathSize + 8];
    snprintf(path, sizeof(path), "%s.mapper", media_path);
    FILE *file = fopen(path, "r");
    if (file) {
        char name[16] = {};
        const int count = fscanf(file, "%15s", name);
        fclose(file);
        static constexpr const char *names[] = {"linear","ascii8","ascii16","konami","scc"};
        if (count == 1)
            for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
                if (!strcasecmp(names[i], name)) return static_cast<rs_cart_mapper>(i);
    }
    if (size <= 65536) return RS_CART_LINEAR;
    unsigned ascii8 = 0, ascii16 = 0, konami = 0, scc = 0;
    for (size_t i = 0; i + 2 < size; ++i) {
        if (rom[i] != 0x32 || rom[i + 1] != 0) continue; /* LD (nn),A */
        switch (rom[i + 2]) {
        case 0x50: case 0x90: case 0xb0: scc += 2; break;
        case 0x60: ++ascii16; ++konami; break;
        case 0x68: case 0x78: ascii8 += 3; break;
        case 0x70: ascii16 += 2; ++scc; break;
        case 0x80: case 0xa0: konami += 3; break;
        }
    }
    if (konami > ascii8 && konami > ascii16 && konami >= scc) return RS_CART_KONAMI;
    if (scc > ascii8 && scc > ascii16) return RS_CART_KONAMI_SCC;
    return ascii8 >= ascii16 ? RS_CART_ASCII8 : RS_CART_ASCII16;
}

bool mount_game(rs_msx *m)
{
    const bool is_cart = extension(media_path, ".rom");
    const bool is_disk = extension(media_path, ".dsk");
    if (!is_cart && !is_disk && !extension(media_path, ".cas")) return false;
    static constexpr const char *main_rom[] = {"MSX.ROM","MSX2.ROM","MSX2P.ROM"};
    static constexpr const char *sub_rom[] = {"","MSX2EXT.ROM","MSX2PEXT.ROM"};
    static constexpr const char *logo_rom[] = {"CBIOS1.ROM","CBIOS2.ROM","CBIOS2P.ROM"};
    const unsigned profile = machine_model - 1;
    const bool first = machine_model == 1;
    if (!bios(m, main_rom[profile], 0, 0, 0)) return false;
    if (!first && !bios(m, sub_rom[profile], 3, 1, 0)) return false;
    if (is_cart && exists(BiosDir, logo_rom[profile]))
        (void)bios(m, logo_rom[profile], first ? 2 : 3, first ? 0 : 1, 1);
    const bool disk_enabled = bios(m, "DISK.ROM", first ? 2 : 3, first ? 0 : 2, 1, true);
    if (is_disk && !disk_enabled) return false;

    size_t size = 0;
    void *data = read_file(media_path, size);
    if (!data) return false;
    bool ok;
    if (is_cart) ok = rs_msx_mount_cart(m, 1, mapper_for(static_cast<const uint8_t *>(data), size), data, size);
    else if (is_disk) ok = rs_msx_mount_disk(m, 0, data, size);
    else ok = rs_msx_mount_cas(m, data, size);
    free(data);
    return ok && rs_msx_reset(m);
}

uint32_t path_hash()
{
    uint32_t hash = 2166136261u;
    for (const char *p = media_path; *p; ++p) hash = (hash ^ static_cast<uint8_t>(*p)) * 16777619u;
    return hash;
}

bool state_file(rs_msx *m, int slot, bool save)
{
    char path[96], staged[104], backup[104];
    snprintf(path, sizeof(path), "%s/independent-%08lx-m%u-%d.rss", StateDir,
             static_cast<unsigned long>(path_hash()), machine_model, slot);
    const size_t size = rs_msx_state_size(m);
    snprintf(backup, sizeof(backup), "%s.old", path);
    if (!save) {
        size_t read_size = 0;
        void *data = read_file(path, read_size, size);
        if (!data) data = read_file(backup, read_size, size);
        const bool ok = data && rs_msx_load(m, data, read_size);
        free(data);
        return ok;
    }
    mkdir(StateDir, 0775);
    snprintf(staged, sizeof(staged), "%s.tmp", path);
    void *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    FILE *file = data && rs_msx_save(m, data, size) ? fopen(staged, "wb") : nullptr;
    bool ok = false;
    if (file) {
        ok = fwrite(data, 1, size, file) == size;
        ok = fclose(file) == 0 && ok;
        if (ok) {
            /* FAT rename does not replace an existing destination. Preserve
             * the previous snapshot until the new one has its final name. */
            struct stat previous;
            const bool has_previous = stat(path, &previous) == 0;
            if (has_previous) {
                if (std::remove(backup) != 0 && errno != ENOENT) ok = false;
                if (ok) ok = rename(path, backup) == 0;
            }
            if (ok) {
                ok = rename(staged, path) == 0;
                if (ok) std::remove(backup);
                else if (has_previous) (void)rename(backup, path);
            }
        }
        if (!ok) std::remove(staged);
    }
    free(data);
    return ok;
}

void apply_request(rs_msx *m, const Request &req)
{
    if (req.action == Action::none) return;
    if (req.action == Action::disk) {
        size_t size = 0;
        void *data = read_file(req.path, size);
        bool ok = data && rs_msx_mount_disk(m, static_cast<unsigned>(req.slot), data, size);
        free(data);
        ESP_LOGI(Tag, "drive %c mount %s", 'A' + req.slot, ok ? "OK" : "failed");
    } else {
        const bool write = req.action == Action::save;
        ESP_LOGI(Tag, "state %s slot %d: %s", write ? "save" : "load", req.slot,
                 state_file(m, req.slot, write) ? "OK" : "failed");
    }
}

bool enqueue(const Request &req)
{
    portENTER_CRITICAL(&shared_lock);
    bool ok = queued.action == Action::none && running.load();
    if (ok) queued = req;
    portEXIT_CRITICAL(&shared_lock);
    return ok;
}

bool disk_request(Request &req, int drive, const char *name)
{
    if (drive < 0 || drive > 1 || !name || !extension(name, ".dsk") ||
        strchr(name, '/') || strchr(name, '\\')) return false;
    const int n = snprintf(req.path, sizeof(req.path), "%s/%s", MediaDir, name);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(req.path)) return false;
    req.action = Action::disk;
    req.slot = drive;
    return true;
}

void scan_disks(MediaList &files)
{
    files.count = 0;
    DIR *dir = opendir(MediaDir);
    if (!dir) return;
    while (files.count < DiskLimit) {
        const dirent *entry = readdir(dir);
        if (!entry) break;
        if (!extension(entry->d_name, ".dsk") || strlen(entry->d_name) >= sizeof(files.names[0])) continue;
        strlcpy(files.names[files.count++], entry->d_name, sizeof(files.names[0]));
    }
    closedir(dir);
}

struct WatchUI {
    msx_watch_ui_t page{};
    Menu menu = Menu::root;
    int row = 0, column = 0, selected = 0, key_frames = 0;
    unsigned modifiers = 0, held = 0, repeats = 0;
    uint32_t previous = 0;
    bool shift_previous = false;
    MediaList disks{};

    void publish()
    {
        page.row = row;
        page.column = column;
        page.modifiers = modifiers;
        if (page.type == MSX_WATCH_UI_MENU) {
            static constexpr const char *main[] = {"Resume","Save state","Load state",
                "Change disk A","Change disk B","Rewind tape","Reset","Quit"};
            const bool list = menu == Menu::drive_a || menu == Menu::drive_b;
            const bool root = menu == Menu::root;
            const bool save = menu == Menu::save;
            const char *title = root ? "MSX MENU" : list ?
                (menu == Menu::drive_a ? "DISK A" : "DISK B") : (save ? "SAVE STATE" : "LOAD STATE");
            strlcpy(page.title, title, sizeof(page.title));
            page.total = root ? sizeof(main) / sizeof(main[0]) : list ? disks.count : 3;
            page.selected = selected;
            const int first = selected - MSX_WATCH_UI_ROWS + 1;
            page.first = first < 0 ? 0 : first;
            for (int item = 0; item < MSX_WATCH_UI_ROWS && page.first + item < page.total; ++item) {
                const int i = page.first + item;
                if (root) strlcpy(page.items[item], main[i], sizeof(page.items[item]));
                else if (list) strlcpy(page.items[item], disks.names[i], sizeof(page.items[item]));
                else snprintf(page.items[item], sizeof(page.items[item]), "SLOT %d", i + 1);
            }
        }
        portENTER_CRITICAL(&shared_lock);
        shared_ui = page;
        portEXIT_CRITICAL(&shared_lock);
    }

    void select_item(rs_msx *m)
    {
        if (menu == Menu::root) {
            switch (selected) {
            case 0: page.type = MSX_WATCH_UI_NONE; break;
            case 1: menu = Menu::save; selected = 0; break;
            case 2: menu = Menu::load; selected = 0; break;
            case 3: case 4:
                menu = selected == 3 ? Menu::drive_a : Menu::drive_b;
                scan_disks(disks);
                selected = 0;
                break;
            case 5: rs_msx_rewind_cas(m); page.type = MSX_WATCH_UI_NONE; break;
            case 6: (void)rs_msx_reset(m); page.type = MSX_WATCH_UI_NONE; break;
            case 7: stop_requested.store(true); break;
            }
        } else if (menu == Menu::save || menu == Menu::load) {
            apply_request(m, Request{menu == Menu::save ? Action::save : Action::load, selected + 1, {}});
            page.type = MSX_WATCH_UI_NONE;
        } else if (selected < disks.count) {
            Request req{};
            if (disk_request(req, menu == Menu::drive_b, disks.names[selected])) apply_request(m, req);
            page.type = MSX_WATCH_UI_NONE;
        }
    }

    void press_key(rs_msx *m)
    {
        /* PPI matrix codes for the positions in the existing large keyboard. */
        static constexpr uint8_t keys[6][12] = {
            {58,53,54,55,56,57,65,66,67,60,62,51},
            {1,2,3,4,5,6,7,8,9,0,10,11},
            {59,38,44,26,39,41,46,42,30,36,37,61},
            {49,22,40,25,27,28,29,31,32,33,15,63},
            {48,47,45,24,43,23,35,34,18,19,20,69},
            {50,13,14,17,64,12,21,16,68,70,71,255}
        };
        if (row == 5 && column == 11) { page.type = MSX_WATCH_UI_NONE; return; }
        const unsigned code = keys[row][column];
        if (code >= 48 && code <= 50) { modifiers ^= 1u << (code - 48); return; }
        rs_msx_key_row(m, code / 8, 1u << (code & 7));
        uint8_t applied = static_cast<uint8_t>(modifiers);
        if (code / 8 == 6) applied |= 1u << (code & 7);
        rs_msx_key_row(m, 6, applied);
        key_frames = 8;
    }

    bool update(rs_msx *m, uint32_t buttons)
    {
        const uint32_t edges = buttons & ~previous;
        previous = buttons;
        const unsigned direction = buttons & 15;
        bool move = (edges & 15) != 0;
        if (held != direction) { held = direction; repeats = 0; }
        else if (held && ++repeats >= 16) move |= (repeats - 16) % 4 == 0;
        const bool shift = buttons & WATCH_BUTTON_L2;
        const bool toggle_shift = shift && !shift_previous;
        shift_previous = shift;
        const int old_type = page.type;
        if (edges & MSX_RUNNER_BUTTON_SELECT) {
            page.type = old_type == MSX_WATCH_UI_MENU ? MSX_WATCH_UI_NONE : MSX_WATCH_UI_MENU;
            menu = Menu::root;
            selected = 0;
        } else if (edges & MSX_RUNNER_BUTTON_START)
            page.type = old_type == MSX_WATCH_UI_KEYBOARD ? MSX_WATCH_UI_NONE : MSX_WATCH_UI_KEYBOARD;
        else if (old_type == MSX_WATCH_UI_MENU) {
            if (edges & MSX_RUNNER_BUTTON_B) {
                if (menu == Menu::root) page.type = MSX_WATCH_UI_NONE;
                else { selected = 0; menu = Menu::root; }
            } else if (move && page.total) {
                if (direction & 1) selected = (selected + page.total - 1) % page.total;
                if (direction & 2) selected = (selected + 1) % page.total;
            } else if (edges & MSX_RUNNER_BUTTON_A) select_item(m);
        } else if (old_type == MSX_WATCH_UI_KEYBOARD) {
            if (edges & MSX_RUNNER_BUTTON_B) page.type = MSX_WATCH_UI_NONE;
            else if (toggle_shift) modifiers ^= MSX_WATCH_UI_SHIFT;
            else if (move) {
                if (direction & 1) row = (row + 5) % 6;
                if (direction & 2) row = (row + 1) % 6;
                if (direction & 4) column = (column + 11) % 12;
                if (direction & 8) column = (column + 1) % 12;
            } else if (!key_frames && (edges & MSX_RUNNER_BUTTON_A)) press_key(m);
        }
        if (page.type != MSX_WATCH_UI_NONE || old_type != page.type) publish();
        const bool playing = page.type == MSX_WATCH_UI_NONE;
        rs_msx_joystick(m, 0, playing && old_type == page.type ? buttons & 63 : 0);
        if (key_frames) return true;
        return playing || old_type == MSX_WATCH_UI_KEYBOARD;
    }

    void finish_key(rs_msx *m)
    {
        if (key_frames && --key_frames == 4)
            for (unsigned i = 0; i < 16; ++i) rs_msx_key_row(m, i, 0);
    }
};

void task(void *)
{
    rs_msx_config config = rs_msx_defaults(static_cast<rs_msx_model>(machine_model));
    void *ui_storage = heap_caps_malloc(sizeof(WatchUI), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *ui = ui_storage ? new (ui_storage) WatchUI{} : nullptr;
    rs_msx *m = rs_msx_create(&config);
    int16_t audio[RS_MSX_MAX_FRAME_SAMPLES];
    if (ui && m && mount_game(m)) {
        uint64_t deadline = esp_timer_get_time();
        unsigned render_budget = 100, frame_us = 16688;
        int64_t report_start = esp_timer_get_time();
        uint64_t core_us = 0;
        unsigned frames = 0, rendered = 0, samples = 0;
        while (!stop_requested.load()) {
            portENTER_CRITICAL(&shared_lock);
            const Request req = queued;
            queued.action = Action::none;
            portEXIT_CRITICAL(&shared_lock);
            apply_request(m, req);
            const bool advance = ui->update(m, gamepad.load());
            const bool overlay = ui->page.type != MSX_WATCH_UI_NONE;
            render_budget += render_percent.load();
            const bool render = render_budget >= 100;
            if (render) render_budget %= 100;
            uint16_t *video = render || overlay ? acquire_frame_callback(callback_context) : nullptr;
            if (advance) {
                const uint64_t start = esp_timer_get_time();
                const size_t count = rs_msx_frame(m, video ? video + Width * TopBorder : nullptr, audio);
                if (!overlay) {
                    core_us += esp_timer_get_time() - start;
                    ++frames;
                    rendered += video != nullptr;
                    samples += count;
                }
                frame_us = (uint64_t)rs_msx_frame_cycles(m) * 1000000 / 3579545;
                ui->finish_key(m);
                if (!overlay && audio_callback) audio_callback(audio, count, RS_MSX_AUDIO_RATE, callback_context);
            }
            if (video) frame_callback(video, Width, Height, callback_context);
            deadline += frame_us;
            const uint64_t now = esp_timer_get_time();
            if (now - report_start >= 5000000) {
                if (frames)
                    ESP_LOGI(Tag, "core timing: %u frames (%u rendered) in %llu ms, avg=%llu us, samples=%u",
                             frames, rendered, (now - report_start) / 1000, core_us / frames, samples);
                report_start = now;
                core_us = 0;
                frames = rendered = samples = 0;
            }
            if (deadline > now + 999) vTaskDelay(pdMS_TO_TICKS((deadline - now) / 1000));
            else if (deadline + frame_us < now) { deadline = now; vTaskDelay(1); }
            else taskYIELD();
        }
    } else ESP_LOGE(Tag, "Could not start MSX%u. Check BIOS, media, and PSRAM.", machine_model);
    rs_msx_destroy(m);
    if (ui) ui->~WatchUI();
    free(ui_storage);
    portENTER_CRITICAL(&shared_lock);
    shared_ui = {};
    queued = {};
    portEXIT_CRITICAL(&shared_lock);
    finished.store(true);
    running.store(false);
    vTaskDelete(nullptr);
}

void set_input(uint32_t buttons)
{
    uint32_t filtered = watch_controls_filter(buttons);
    if (!(buttons & 15)) filtered |= buttons & WATCH_BUTTON_L2;
    gamepad.store(filtered);
}
} /* namespace */

extern "C" void msx_watch_ui_read(msx_watch_ui_t *out)
{
    portENTER_CRITICAL(&shared_lock);
    *out = shared_ui;
    portEXIT_CRITICAL(&shared_lock);
}

extern "C" esp_err_t msx_runner_start(const char *path, msx_runner_frame_cb_t frame, msx_runner_audio_cb_t audio,
                                       void *context, unsigned model, msx_runner_acquire_cb_t acquire_frame)
{
    if (!path || strlen(path) >= sizeof(media_path) || model < 1 || model > 3 || !frame || !acquire_frame)
        return ESP_ERR_INVALID_ARG;
    if (running.exchange(true)) return ESP_ERR_INVALID_STATE;
    strlcpy(media_path, path, sizeof(media_path));
    machine_model = model;
    frame_callback = frame;
    acquire_frame_callback = acquire_frame;
    audio_callback = audio;
    callback_context = context;
    gamepad.store(0);
    serial_until.store(0);
    stop_requested.store(false);
    finished.store(false);
    if (xTaskCreatePinnedToCore(task, "retros3_msx", 8192, nullptr, 6, nullptr, 1) == pdPASS) return ESP_OK;
    running.store(false);
    return ESP_ERR_NO_MEM;
}

extern "C" void msx_runner_stop(void) { stop_requested.store(true); }
extern "C" bool msx_runner_is_running(void) { return running.load(); }
extern "C" bool msx_runner_take_finished(void) { return finished.exchange(false); }
extern "C" void msx_runner_set_buttons(uint32_t buttons)
{
    if (esp_timer_get_time() >= serial_until.load()) set_input(buttons);
}
extern "C" void msx_runner_set_serial_buttons(uint32_t buttons)
{
    serial_until.store(esp_timer_get_time() + 300000);
    set_input(buttons);
}
extern "C" void msx_runner_set_render_percent(unsigned percent)
{
    render_percent.store(percent > 100 ? 100 : percent ? percent : 1);
}
extern "C" esp_err_t msx_runner_state_request(bool save, int slot)
{
    if (slot < 1 || slot > 3) return ESP_ERR_INVALID_ARG;
    return enqueue(Request{save ? Action::save : Action::load, slot, {}}) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
extern "C" esp_err_t msx_runner_disk_request(int drive, const char *filename)
{
    Request req{};
    if (!disk_request(req, drive, filename)) return ESP_ERR_INVALID_ARG;
    return enqueue(req) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
