/* SPDX-License-Identifier: MIT */
#ifndef RETROS3_MSX_H
#define RETROS3_MSX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rs_msx rs_msx;
typedef enum { RS_MSX1 = 1, RS_MSX2, RS_MSX2_PLUS } rs_msx_model;
typedef enum {
    RS_CART_LINEAR, RS_CART_ASCII8, RS_CART_ASCII16, RS_CART_KONAMI, RS_CART_KONAMI_SCC
} rs_cart_mapper;

enum { RS_MSX_HEIGHT = 212, RS_MSX_AUDIO_RATE = 32000, RS_MSX_MAX_FRAME_SAMPLES = 640 };

typedef struct {
    rs_msx_model model;
    unsigned ram_kib;     /* 64, 128, 256, 512, 1024, 2048 or 4096. MSX1 uses 64. */
    unsigned video_width; /* RGB565: 256 (watch) or 512 (full horizontal resolution). */
    bool pal;             /* Initial timing, adjustable through VDP register 9 on MSX2+. */
    unsigned expanded_slots; /* Four primary slots, one bit per slot. */
    unsigned ram_slot, ram_subslot;
} rs_msx_config;

rs_msx_config rs_msx_defaults(rs_msx_model model);
rs_msx *rs_msx_create(const rs_msx_config *config);
void rs_msx_destroy(rs_msx *msx);

/* Mounts copy their input. A failed mount leaves the previous media intact. */
bool rs_msx_mount_rom(rs_msx *msx, unsigned slot, unsigned subslot, unsigned page,
                      const void *data, size_t size);
bool rs_msx_mount_disk_rom(rs_msx *msx, unsigned slot, unsigned subslot,
                           const void *data, size_t size);
bool rs_msx_mount_cart(rs_msx *msx, unsigned slot, rs_cart_mapper mapper,
                       const void *data, size_t size);
bool rs_msx_mount_cas(rs_msx *msx, const void *data, size_t size);
bool rs_msx_mount_disk(rs_msx *msx, unsigned drive, const void *data, size_t size);
void rs_msx_rewind_cas(rs_msx *msx);
bool rs_msx_reset(rs_msx *msx); /* Reset requires BIOS ROM at slot 0, page 0. */

void rs_msx_key_row(rs_msx *msx, unsigned row, uint8_t pressed);
void rs_msx_joystick(rs_msx *msx, unsigned port, uint8_t pressed); /* UDLR, A, B */
/* Runs one frame. Pass NULL for omitted video/audio. Caller owns the buffers. */
size_t rs_msx_frame(rs_msx *msx, uint16_t *video, int16_t *audio);
unsigned rs_msx_frame_cycles(const rs_msx *msx);
uint16_t rs_msx_pc(const rs_msx *msx);

/* v1 states contain CPU, device state, RAM and VRAM. Restore with the same
 * configuration and media mounted. The buffers must have exactly this size. */
size_t rs_msx_state_size(const rs_msx *msx);
bool rs_msx_save(const rs_msx *msx, void *buffer, size_t size);
bool rs_msx_load(rs_msx *msx, const void *buffer, size_t size);

#ifdef __cplusplus
}
#endif
#endif
