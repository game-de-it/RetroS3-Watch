/* SPDX-License-Identifier: MIT */
#include "retros3_msx.h"
#include "media.h"
#include "vdp.h"
#include "z80.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t cpu_ram[65536];
static uint8_t cpu_read(void *unused, uint16_t a) { (void)unused; return cpu_ram[a]; }
static void cpu_write(void *unused, uint16_t a, uint8_t v) { (void)unused; cpu_ram[a] = v; }

static void z80_test(void)
{
    const uint8_t code[] = {
        0x31,0x00,0x90,      /* LD SP,9000h */
        0x3e,0x7f,0xc6,1,   /* LD A,7Fh; ADD A,1 */
        0xdd,0x21,0,0x80,   /* LD IX,8000h */
        0xdd,0x36,0xff,0x80,/* LD (IX-1),80h */
        0xdd,0xcb,0xff,0x06,/* RLC (IX-1) */
        0xfb,0x76           /* EI; HALT */
    };
    memcpy(cpu_ram, code, sizeof(code));
    rs_z80 c = {0};
    const rs_z80_bus bus = {NULL, cpu_read, cpu_write, cpu_read, cpu_write};
    assert(rs_z80_step(&c, &bus, true) == 10 && c.sp == 0x9000);
    assert(rs_z80_step(&c, &bus, true) == 7);
    assert(rs_z80_step(&c, &bus, true) == 7 && c.af == 0x8094);
    assert(rs_z80_step(&c, &bus, true) == 14 && c.ix == 0x8000);
    assert(rs_z80_step(&c, &bus, true) == 19 && cpu_ram[0x7fff] == 128);
    assert(rs_z80_step(&c, &bus, true) == 23 && cpu_ram[0x7fff] == 1 && (c.af & 1));
    assert(rs_z80_step(&c, &bus, true) == 4 && c.iff1);
    assert(rs_z80_step(&c, &bus, true) == 4 && c.halted);
    assert(rs_z80_step(&c, &bus, true) == 13 && c.pc == 0x38 && !c.halted);
    assert(cpu_ram[0x8ffe] == sizeof(code) && cpu_ram[0x8fff] == 0);
}

static void media_test(void)
{
    static uint8_t image[720 * 1024];
    const uint8_t cas[] = {0x1f,0xa6,0xde,0xba,0xcc,0x13,0x7d,0x74,0xa5};
    rs_disk disk[] = {{image, sizeof(image), 0, 0, 0}, {0}};
    assert(rs_disk_geometry(disk));
    assert(disk->sides == 2 && disk->cylinders == 80 && disk->sectors == 9);
    rs_fdc f = {0};
    image[512] = 0xa5;
    rs_fdc_write(&f, disk, 2, 2);
    rs_fdc_write(&f, disk, 0, 0x80);
    assert(!(rs_fdc_read(&f, disk, 7) & 128));
    assert(rs_fdc_read(&f, disk, 3) == 0xa5);
    for (unsigned i = 1; i < 512; ++i) assert(rs_fdc_read(&f, disk, 3) == 0);
    assert(!(rs_fdc_read(&f, disk, 7) & 64));
    assert(rs_cas_marker(cas, sizeof(cas), 0));
    assert(!rs_cas_marker(cas, sizeof(cas), 8));
    rs_tape t = {0};
    rs_tape_step(&t, cas, sizeof(cas), 746);
    assert(t.level == 1 && t.pulses == 15999);
    rs_tape_step(&t, cas, sizeof(cas), 746 * 15999);
    assert(t.level == 0 && t.pulses == 0 && t.offset == 8);
    rs_tape_step(&t, cas, sizeof(cas), 1491 * 2);
    assert(t.bit == 1 && t.level == 0);
}

static uint16_t video[256 * RS_MSX_HEIGHT], video2[256 * RS_MSX_HEIGHT];
static int16_t samples[RS_MSX_MAX_FRAME_SAMPLES], samples2[RS_MSX_MAX_FRAME_SAMPLES];

static void state_test(void)
{
    static uint8_t bios[32768];
    const uint8_t code[] = {
        0x3e,0xf0,0xd3,0xa8,             /* RAM at 8000h through FFFFh */
        0x3e,0,0xd3,0xa0,0x3e,75,0xd3,0xa1,    /* PSG tone A period */
        0x3e,7,0xd3,0xa0,0x3e,0x3e,0xd3,0xa1,  /* Enable tone A */
        0x3e,8,0xd3,0xa0,0x3e,15,0xd3,0xa1,    /* Set tone A amplitude */
        0x21,0x00,0x90,                  /* LD HL,9000h */
        0x34,0x7e,0xe6,0x0f,0xd3,0x99, /* Count and set backdrop */
        0x3e,0x87,0xd3,0x99,0x18,0xf4
    };
    memcpy(bios, code, sizeof(code));
    rs_msx_config config = rs_msx_defaults(RS_MSX2_PLUS);
    rs_msx *m = rs_msx_create(&config);
    assert(m && rs_msx_mount_rom(m, 0, 0, 0, bios, sizeof(bios)) && rs_msx_reset(m));
    assert(rs_msx_frame(m, NULL, NULL) > 500);
    size_t size = rs_msx_state_size(m);
    uint8_t *state = malloc(size), *after = malloc(size), *repeat = malloc(size);
    assert(state && after && repeat && rs_msx_save(m, state, size));
    size_t n = rs_msx_frame(m, video, samples);
    bool positive = false, negative = false;
    for (size_t i = 0; i < n; ++i) {
        positive |= samples[i] > 0;
        negative |= samples[i] < 0;
    }
    assert(positive && negative);
    assert(rs_msx_save(m, after, size) && rs_msx_load(m, state, size));
    assert(rs_msx_frame(m, video2, samples2) == n && rs_msx_save(m, repeat, size));
    assert(!memcmp(video, video2, sizeof(video)) && !memcmp(samples, samples2, n * sizeof(*samples)));
    assert(!memcmp(after, repeat, size));
    state[size - 1] ^= 1;
    assert(!rs_msx_load(m, state, size));
    assert(rs_msx_save(m, repeat, size) && !memcmp(after, repeat, size));
    state[size - 1] ^= 1;
    assert(!rs_msx_load(m, state, size - 1));
    static const uint8_t cas[] = {0x1f,0xa6,0xde,0xba,0xcc,0x13,0x7d,0x74,0xa5};
    assert(rs_msx_mount_cas(m, cas, sizeof(cas)) && !rs_msx_load(m, state, size));
    free(repeat);
    free(after);
    free(state);
    rs_msx_destroy(m);
}

static void vdp_test(void)
{
    static uint8_t ram[128 * 1024];
    rs_vdp v;
    rs_vdp_reset(&v, 3);
    rs_vdp_write(&v, ram, 3, 1, 0xff);
    rs_vdp_write(&v, ram, 3, 1, 0x7f);
    rs_vdp_write(&v, ram, 3, 0, 0xab);
    rs_vdp_write(&v, ram, 3, 0, 0xcd);
    assert(ram[0x3fff] == 0xab && ram[0] == 0xcd && v.reg[14] == 0);
    ram[0] = 0;
    rs_vdp_write(&v, ram, 3, 1, 8);
    rs_vdp_write(&v, ram, 3, 1, 0x8f);
    assert(rs_vdp_read(&v, ram, 3, 1) == 0);
    rs_vdp_write(&v, ram, 3, 1, 1);
    rs_vdp_write(&v, ram, 3, 1, 0x8f);
    assert(rs_vdp_read(&v, ram, 3, 1) == 4);
    v.reg[0] = 14; /* SCREEN 8 hardware mode */
    v.reg[1] = 64;
    v.reg[8] = 32 | 2;
    v.reg[25] = 8; /* YJK */
    ram[0] = 16 << 3;
    rs_vdp_scanline(&v, ram, 3, video, 256);
    assert(video[0] == (16 << 11 | 32 << 5 | 20));
    v.reg[0] = 6; /* SCREEN 5 */
    v.reg[40] = 4;
    v.reg[42] = 2;
    v.reg[44] = 0x55;
    rs_vdp_write(&v, ram, 3, 1, 0xc0); /* HMMV */
    rs_vdp_write(&v, ram, 3, 1, 0xae);
    assert(ram[0] == 0x55 && ram[1] == 0x55 && ram[128] == 0x55 && ram[129] == 0x55);
    ram[0] = 1;
    ram[1] = 2;
    ram[2] = 3;
    ram[3] = 4;
    v.reg[36] = 2;
    v.reg[40] = 6;
    v.reg[42] = 1;
    rs_vdp_write(&v, ram, 3, 1, 0xd0); /* HMMM, overlapping forward copy */
    rs_vdp_write(&v, ram, 3, 1, 0xae);
    assert(ram[0] == 1 && ram[1] == 1 && ram[2] == 1 && ram[3] == 1);
    ram[1] = 2;
    ram[2] = 3;
    ram[3] = 4;
    v.reg[32] = 4;
    v.reg[36] = 6;
    v.reg[45] = 4;
    rs_vdp_write(&v, ram, 3, 1, 0xd0); /* HMMM, overlapping backward copy */
    rs_vdp_write(&v, ram, 3, 1, 0xae);
    assert(ram[0] == 1 && ram[1] == 1 && ram[2] == 2 && ram[3] == 3);
    v.reg[36] = 253;
    v.reg[38] = 4;
    v.reg[45] = 0;
    rs_vdp_write(&v, ram, 3, 1, 0xc0); /* HMMV, stop at right edge */
    rs_vdp_write(&v, ram, 3, 1, 0xae);
    assert(ram[638] == 0x55 && ram[639] == 0x55 && ram[640] == 0);
}

static void sprite_address_test(void)
{
    static uint8_t ram[128 * 1024];
    rs_vdp v;
    rs_vdp_reset(&v, 2);
    v.reg[0] = 6;    /* SCREEN 5 */
    v.reg[1] = 0x42; /* Display enabled, 16x16 sprites */
    v.reg[5] = 0xef;
    v.reg[6] = 0x1f; /* Patterns at F800h */
    v.reg[11] = 1;   /* Attributes at F600h, colors at F400h */
    v.line = 41;
    ram[0xb600] = 216; /* The lower alias must not terminate the sprite list. */
    ram[0xf600] = 40;
    ram[0xf601] = 56;
    ram[0xf602] = 4;
    ram[0xf604] = 216;
    ram[0xf400] = 6;
    ram[0xf820] = 0x80;
    ram[0xf830] = 0x40;
    rs_vdp_scanline(&v, ram, 2, video, 256);
    assert(video[56] == v.palette[6] && video[65] == v.palette[6]);
    assert(video[57] == v.palette[0] && video[64] == v.palette[0]);
}

static void sprite_color_combine_test(void)
{
    static uint8_t ram[128 * 1024];
    static const uint8_t colors[] = {0x44, 1, 0x42, 4, 0x48};
    static const uint8_t bits[] = {0x08, 0x80, 0xc0, 0x40, 0xa0};
    rs_vdp v;
    rs_vdp_reset(&v, 2);
    v.reg[0] = 6;
    v.reg[1] = 0x40;
    v.reg[5] = 0xf7;
    v.reg[6] = 14;
    v.line = 41;
    for (unsigned i = 0; i < sizeof(colors); ++i) {
        ram[0x7a00 + i * 4] = 40;
        ram[0x7a01 + i * 4] = 56;
        ram[0x7a02 + i * 4] = i;
        ram[0x7800 + i * 16] = colors[i];
        ram[0x7000 + i * 8] = bits[i];
    }
    ram[0x7a14] = 216;
    rs_vdp_scanline(&v, ram, 2, video, 256);
    assert(video[56] == v.palette[3]); /* Overlapping dots in the first group. */
    assert(video[57] == v.palette[2]); /* Combine layer owns its uncovered dots. */
    assert(video[58] == v.palette[8]); /* Second group owns its uncovered dots. */
    assert(video[60] == v.palette[0]); /* Leading combine layer has no base. */
    assert(!(v.status[0] & 32));       /* Combine dots do not collide. */
}

static void mount_bios(rs_msx *m, const char *path, unsigned slot, unsigned sub, unsigned page)
{
    uint8_t bytes[32768];
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    assert(fgetc(f) == EOF && fclose(f) == 0);
    assert(rs_msx_mount_rom(m, slot, sub, page, bytes, n));
}

static void cbios_test(unsigned model, const char *bios, const char *sub)
{
    const uint8_t program[] = {
        'A','B',0x10,0x40,0,0,0,0,0,0,0,0,0,0,0,0,
        0xf3,0x3e,0x0f,0xd3,0x99,0x3e,0x87,0xd3,0x99, /* White backdrop */
        0x18,0xfe                                        /* JR to itself */
    };
    uint8_t cart[16384] = {0};
    memcpy(cart, program, sizeof(program));
    rs_msx_config config = rs_msx_defaults((rs_msx_model)model);
    rs_msx *m = rs_msx_create(&config);
    assert(m);
    mount_bios(m, bios, 0, 0, 0);
    if (sub) mount_bios(m, sub, 3, 1, 0);
    assert(rs_msx_mount_cart(m, 1, RS_CART_LINEAR, cart, sizeof(cart)));
    assert(rs_msx_reset(m));
    for (unsigned i = 0; i < 240; ++i) rs_msx_frame(m, video, NULL);
    printf("C-BIOS model %u: PC=%04X\n", model, rs_msx_pc(m));
    assert(rs_msx_pc(m) == 0x4019);
    assert(video[RS_MSX_HEIGHT * 256 - 1] != 0);
    rs_msx_destroy(m);
}

int main(int argc, char **argv)
{
    if (argc >= 3) cbios_test((unsigned)atoi(argv[1]), argv[2], argc == 4 ? argv[3] : NULL);
    else {
        z80_test();
        media_test();
        vdp_test();
        sprite_address_test();
        sprite_color_combine_test();
        state_test();
    }
    return 0;
}
