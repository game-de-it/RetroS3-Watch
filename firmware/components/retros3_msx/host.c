/* SPDX-License-Identifier: MIT */
#include "retros3_msx.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    const char *bios, *sub, *logo, *diskrom, *cart, *disk, *disk_b, *swap_a, *cas;
    const char *load, *save, *ppm, *keys;
    unsigned model, frames, key_frame, swap_frame, ram, width;
    rs_cart_mapper mapper;
    bool pal, jp;
} options;

static bool number(const char *v, unsigned *out)
{
    char *end;
    errno = 0;
    unsigned long n = strtoul(v, &end, 10);
    if (errno || end == v || *end || n > 1000000) return false;
    *out = (unsigned)n;
    return true;
}

static bool parse(options *o, int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const char *k = argv[i];
        if (!strcmp(k, "--pal")) { o->pal = true; continue; }
        if (!strcmp(k, "--jp")) { o->jp = true; continue; }
        if (++i >= argc) return false;
        const char *v = argv[i];
        struct { const char *name; const char **value; } strings[] = {
            {"--bios",&o->bios}, {"--sub",&o->sub}, {"--logo",&o->logo}, {"--disk-rom",&o->diskrom},
            {"--rom",&o->cart}, {"--disk",&o->disk}, {"--disk-b",&o->disk_b}, {"--swap-a",&o->swap_a},
            {"--cas",&o->cas}, {"--load",&o->load}, {"--save",&o->save}, {"--ppm",&o->ppm}, {"--type",&o->keys},
        };
        struct { const char *name; unsigned *value; } numbers[] = {
            {"--model",&o->model}, {"--frames",&o->frames}, {"--type-frame",&o->key_frame},
            {"--swap-frame",&o->swap_frame}, {"--ram",&o->ram}, {"--width",&o->width},
        };
        bool found = false;
        for (size_t n = 0; n < sizeof(strings) / sizeof(strings[0]); ++n) {
            if (!strcmp(k, strings[n].name)) { *strings[n].value = v; found = true; break; }
        }
        for (size_t n = 0; !found && n < sizeof(numbers) / sizeof(numbers[0]); ++n) {
            if (!strcmp(k, numbers[n].name)) { if (!number(v, numbers[n].value)) return false; found = true; }
        }
        if (!found && !strcmp(k, "--mapper")) {
            static const char *const names[] = {"linear","ascii8","ascii16","konami","scc"};
            for (size_t n = 0; n < sizeof(names) / sizeof(names[0]); ++n)
                if (!strcmp(v, names[n])) { o->mapper = (rs_cart_mapper)n; found = true; break; }
        }
        if (!found) return false;
    }
    return o->bios && o->model >= 1 && o->model <= 3 && o->frames &&
           (!(o->disk || o->disk_b || o->swap_a) || o->diskrom) &&
           (!o->swap_a || o->swap_frame < o->frames);
}

static void *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    long length = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    void *buffer = NULL;
    if (length > 0 && length <= 8 * 1024 * 1024L && !fseek(f, 0, SEEK_SET)) {
        buffer = malloc((size_t)length);
        if (buffer && fread(buffer, 1, (size_t)length, f) != (size_t)length) {
            free(buffer);
            buffer = NULL;
        }
    }
    fclose(f);
    if (buffer) *size = (size_t)length;
    return buffer;
}

typedef enum { BIOS, SUB, LOGO, DISKROM, CART, DISKA, DISKB, CAS, STATE } input_type;

static bool mount(rs_msx *m, const options *o, input_type type, const char *path)
{
    if (!path) return true;
    size_t size = 0;
    void *data = read_file(path, &size);
    bool ok = false;
    if (data) {
        switch (type) {
        case BIOS: ok = rs_msx_mount_rom(m, 0, 0, 0, data, size); break;
        case SUB: ok = rs_msx_mount_rom(m, 3, 1, 0, data, size); break;
        case LOGO: ok = rs_msx_mount_rom(m, o->model == 1 ? 2 : 3, o->model == 1 ? 0 : 1, 1, data, size); break;
        case DISKROM: ok = rs_msx_mount_disk_rom(m, o->model == 1 ? 2 : 3, o->model == 1 ? 0 : 2, data, size); break;
        case CART: ok = rs_msx_mount_cart(m, 1, o->mapper, data, size); break;
        case DISKA: case DISKB: ok = rs_msx_mount_disk(m, type == DISKB, data, size); break;
        case CAS: ok = rs_msx_mount_cas(m, data, size); break;
        case STATE: ok = rs_msx_load(m, data, size); break;
        }
    }
    free(data);
    if (!ok) fprintf(stderr, "Cannot load %s\n", path);
    return ok;
}

static bool ppm(const char *path, const uint16_t *video, unsigned width)
{
    if (!path) return true;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fprintf(f, "P6\n%u %u\n255\n", width, RS_MSX_HEIGHT) > 0;
    uint8_t rgb[512 * 3];
    for (unsigned y = 0; ok && y < RS_MSX_HEIGHT; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            uint16_t p = *video++;
            rgb[x * 3] = (p >> 11) * 255 / 31;
            rgb[x * 3 + 1] = ((p >> 5) & 63) * 255 / 63;
            rgb[x * 3 + 2] = (p & 31) * 255 / 31;
        }
        ok = fwrite(rgb, 3, width, f) == width;
    }
    return fclose(f) == 0 && ok;
}

static bool save(rs_msx *m, const char *path)
{
    if (!path) return true;
    size_t size = rs_msx_state_size(m);
    void *buffer = malloc(size);
    bool ok = buffer && rs_msx_save(m, buffer, size);
    FILE *f = ok ? fopen(path, "wb") : NULL;
    if (f) {
        ok = fwrite(buffer, 1, size, f) == size;
        ok = fclose(f) == 0 && ok;
    } else ok = false;
    free(buffer);
    return ok;
}

static void type_key(rs_msx *m, char c, bool jp)
{
    static const char unshifted[] = "0123456789-=\\[];'`,./ ";
    static const char shifted[] = ")!@#$%^&*(_+|{}:\"~<>? ";
    static const char japanese[] = " !\"#$%&'()=~|{}`*<>? ";
    unsigned index = 0, row = 0, bit = 0;
    bool shift = false;
    if (c >= 'A' && c <= 'Z') index = (unsigned)(c - 'A') + 22;
    else if (c >= 'a' && c <= 'z') index = (unsigned)(c - 'a') + 22;
    else if (c == ' ' || c == '\n' || c == '\r') {
        row = c == ' ' ? 8 : 7;
        bit = c == ' ' ? 0 : 7;
    } else {
        const char *p = strchr(unshifted, c);
        if (jp && c == '\'') { index = 7; shift = true; }
        else if (jp && c == '(') { index = 8; shift = true; }
        else if (jp && c == ')') { index = 9; shift = true; }
        else if (jp && c == ':') index = 16;
        else if (p) index = (unsigned)(p - unshifted);
        else {
            const char *map = jp ? japanese : shifted;
            p = strchr(map, c);
            if (!p) return;
            index = (unsigned)(p - map);
            shift = true;
        }
    }
    if (!row) { row = index / 8; bit = index % 8; }
    rs_msx_key_row(m, row, (uint8_t)(1u << bit));
    if (shift) rs_msx_key_row(m, 6, 1);
}

int main(int argc, char **argv)
{
    options o = {.model = 2, .frames = 300, .width = 512, .key_frame = 180};
    if (!parse(&o, argc, argv)) {
        fprintf(stderr, "Usage: retros3-msx --bios FILE [--model 1|2|3] [--sub FILE] [--logo FILE]\n"
                " [--rom FILE --mapper linear|ascii8|ascii16|konami|scc]\n"
                " [--disk-rom FILE --disk FILE [--disk-b FILE] [--swap-a FILE --swap-frame N]] [--cas FILE]\n"
                " [--pal] [--ram KiB] [--width 256|512] [--frames N] [--jp] [--type TEXT --type-frame N]\n"
                " [--save FILE] [--load FILE] [--ppm FILE]\n");
        return argc == 2 && !strcmp(argv[1], "--help") ? 0 : 2;
    }

    rs_msx_config config = rs_msx_defaults((rs_msx_model)o.model);
    if (o.ram) config.ram_kib = o.ram;
    config.video_width = o.width;
    config.pal = o.pal;
    rs_msx *m = rs_msx_create(&config);
    uint16_t *video = m ? malloc(o.width * RS_MSX_HEIGHT * sizeof(*video)) : NULL;
    bool ok = video && mount(m, &o, BIOS, o.bios) && mount(m, &o, SUB, o.sub) &&
              mount(m, &o, LOGO, o.logo) && mount(m, &o, DISKROM, o.diskrom) &&
              mount(m, &o, CART, o.cart) && mount(m, &o, DISKA, o.disk) &&
              mount(m, &o, DISKB, o.disk_b) && mount(m, &o, CAS, o.cas) &&
              rs_msx_reset(m) && mount(m, &o, STATE, o.load);
    clock_t start = clock();
    size_t text_len = o.keys ? strlen(o.keys) : 0;
    uint64_t audio_samples = 0;
    for (unsigned frame = 0; ok && frame < o.frames; ++frame) {
        if (o.swap_a && frame == o.swap_frame) ok = mount(m, &o, DISKA, o.swap_a);
        if (frame >= o.key_frame && frame - o.key_frame < text_len * 8) {
            unsigned elapsed = frame - o.key_frame;
            for (unsigned i = 0; i < 16; ++i) rs_msx_key_row(m, i, 0);
            if (elapsed % 8 < 4) type_key(m, o.keys[elapsed / 8], o.jp);
        }
        audio_samples += rs_msx_frame(m, video, NULL);
    }
    double seconds = (clock() - start) / (double)CLOCKS_PER_SEC;
    if (ok) {
        printf("model=%u frames=%u pc=%04X audio_samples=%llu host_seconds=%.3f\n", o.model, o.frames,
               rs_msx_pc(m), (unsigned long long)audio_samples, seconds);
        ok = ppm(o.ppm, video, o.width) && save(m, o.save);
    }
    free(video);
    rs_msx_destroy(m);
    if (!ok) fprintf(stderr, "Emulator run or output failed\n");
    return ok ? 0 : 1;
}
