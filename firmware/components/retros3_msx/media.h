/* SPDX-License-Identifier: MIT */
#ifndef RETROS3_MEDIA_H
#define RETROS3_MEDIA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t offset;
    uint16_t remaining;
    uint8_t command, status, track, sector, data, side, drive, irq;
    uint8_t cylinder[2], direction;
} rs_fdc;

typedef struct {
    uint32_t offset, pulses, elapsed;
    uint8_t level, bit, repeats;
} rs_tape;

typedef struct {
    const uint8_t *bytes;
    size_t size;
    unsigned cylinders, sides, sectors;
} rs_disk;

bool rs_disk_geometry(rs_disk *disk);
uint8_t rs_fdc_read(rs_fdc *f, const rs_disk disks[2], unsigned port);
void rs_fdc_write(rs_fdc *f, const rs_disk disks[2], unsigned port, uint8_t data);
bool rs_cas_marker(const uint8_t *bytes, size_t size, size_t offset);
void rs_tape_step(rs_tape *t, const uint8_t *bytes, size_t size, unsigned cycles);

#endif
