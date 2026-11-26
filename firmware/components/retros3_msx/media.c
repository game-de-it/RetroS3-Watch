/* SPDX-License-Identifier: MIT */
#include "media.h"

#include <string.h>

bool rs_disk_geometry(rs_disk *d)
{
    static const unsigned geometry[][3] = {
        {80,2,9}, {80,1,9}, {40,2,9}, {40,1,9},
        {80,2,8}, {80,1,8}, {40,2,8}, {40,1,8},
    };
    for (size_t i = 0; i < sizeof(geometry) / sizeof(geometry[0]); ++i) {
        unsigned tracks = geometry[i][0], sides = geometry[i][1], sectors = geometry[i][2];
        if (d->size != tracks * sides * sectors * 512) continue;
        /* Boot parameter blocks disambiguate 80x1 images from 40x2 images. */
        if (d->bytes[11] == 0 && d->bytes[12] == 2 && d->bytes[24] == sectors &&
            d->bytes[25] == 0 && d->bytes[27] == 0 &&
            (d->bytes[26] == 1 || d->bytes[26] == 2) && d->bytes[26] != sides) continue;
        d->cylinders = tracks;
        d->sides = sides;
        d->sectors = sectors;
        return true;
    }
    return false;
}

static const rs_disk *selected(const rs_fdc *f, const rs_disk disks[2])
{
    unsigned n = f->drive & 3;
    return n < 2 && disks[n].bytes ? &disks[n] : NULL;
}

static void finish(rs_fdc *f, uint8_t status)
{
    f->status = status;
    f->remaining = 0;
    f->irq = 1;
}

static void read_sector(rs_fdc *f, const rs_disk *d)
{
    unsigned side = f->side & 1, track = f->cylinder[f->drive & 1];
    if (!d) { finish(f, 0x80); return; }
    if (!f->sector || f->sector > d->sectors || track != f->track ||
        track >= d->cylinders || side >= d->sides) {
        finish(f, 0x10);
        return;
    }
    f->offset = ((track * d->sides + side) * d->sectors + f->sector - 1) * 512;
    f->remaining = 512;
    f->status = 3;
}

uint8_t rs_fdc_read(rs_fdc *f, const rs_disk disks[2], unsigned port)
{
    const rs_disk *d = selected(f, disks);
    switch (port) {
    case 0:
        f->irq = 0;
        return f->status | 0x40 | (d ? 0 : 0x80);
    case 1: return f->track;
    case 2: return f->sector;
    case 3:
        if (f->remaining && d && f->offset < d->size) {
            f->data = d->bytes[f->offset++];
            if (!--f->remaining) {
                if ((f->command & 16) && f->sector < d->sectors) {
                    ++f->sector;
                    read_sector(f, d);
                } else finish(f, 0);
            }
        }
        return f->data;
    case 4: return f->side | 0xfe;
    case 5: return f->drive | 0x38;
    case 7: return 0x3f | (f->remaining ? 0 : 0x80) | (f->irq ? 0 : 0x40);
    default: return 255;
    }
}

void rs_fdc_write(rs_fdc *f, const rs_disk disks[2], unsigned port, uint8_t data)
{
    switch (port) {
    case 1: f->track = data; return;
    case 2: f->sector = data; return;
    case 3: f->data = data; return;
    case 4: f->side = data & 1; return;
    case 5:
        if ((f->drive & 3) != (data & 3) && f->remaining) finish(f, 0x10);
        f->drive = data;
        return;
    case 0: break;
    default: return;
    }
    f->command = data;
    f->irq = 0;
    f->remaining = 0;
    unsigned cmd = data >> 4;
    const rs_disk *d = selected(f, disks);
    if (cmd < 8) {
        uint8_t *track = &f->cylinder[f->drive & 1];
        if (cmd == 0) *track = 0;
        else if (cmd == 1) *track = f->data;
        else {
            if (cmd >= 4) f->direction = cmd < 6;
            if (f->direction && *track < 255) ++*track;
            if (!f->direction && *track) --*track;
        }
        if (cmd < 2 || (cmd & 1)) f->track = *track;
        finish(f, (*track == 0 ? 4 : 0) | (d && *track < d->cylinders ? 0 : 0x10));
    } else if (cmd == 8 || cmd == 9) read_sector(f, d);
    else if (cmd == 13) {
        finish(f, 0);
        f->irq = !!(data & 15);
    } else if (cmd == 10 || cmd == 11 || cmd == 15) finish(f, 0x40);
    else finish(f, 0x10);
}

bool rs_cas_marker(const uint8_t *bytes, size_t size, size_t offset)
{
    static const uint8_t marker[] = {0x1f,0xa6,0xde,0xba,0xcc,0x13,0x7d,0x74};
    return offset <= size && size - offset >= sizeof(marker) &&
           !memcmp(bytes + offset, marker, sizeof(marker));
}

void rs_tape_step(rs_tape *t, const uint8_t *bytes, size_t size, unsigned cycles)
{
    t->elapsed += cycles;
    while (t->offset < size || t->pulses) {
        if (!t->pulses && !t->bit && !t->repeats && !(t->offset & 7) &&
            rs_cas_marker(bytes, size, t->offset)) {
            /* File headers use a longer leader than data blocks. */
            bool header = size - t->offset >= 18;
            if (header) {
                unsigned type = bytes[t->offset + 8];
                header = type == 0xd0 || type == 0xd3 || type == 0xea;
                for (unsigned i = 9; header && i < 18; ++i)
                    header = bytes[t->offset + i] == type;
            }
            /* A 16 KiB block may be preceded by a lengthy inter-slot copy
             * while the motor is still running. Leave time for that copy
             * and for TAPION to acquire the carrier. */
            size_t next_block = t->offset + 8 + 16384;
            bool large_block = next_block == size || rs_cas_marker(bytes, size, next_block);
            t->pulses = large_block ? 48000 : t->offset == 0 || header ? 16000 : 8000;
            t->offset += 8;
        }
        bool one = t->pulses || t->bit >= 9 ||
                   (t->bit && ((bytes[t->offset] >> (t->bit - 1)) & 1));
        unsigned half_period = one ? 746 : 1491;
        if (t->elapsed < half_period) return;
        t->elapsed -= half_period;
        t->level ^= 1;
        if (t->pulses) --t->pulses;
        else if (++t->repeats == (one ? 4 : 2)) {
            t->repeats = 0;
            if (++t->bit == 11) {
                t->bit = 0;
                ++t->offset;
                /* The BIOS measures between transitions. Keep a short mark
                 * after the last stop bit so the last byte can be decoded. */
                if (t->offset == size) t->pulses = 16;
            }
        }
    }
    t->level = t->elapsed = 0;
}
