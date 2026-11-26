/* SPDX-License-Identifier: MIT */
#ifndef RETROS3_Z80_H
#define RETROS3_Z80_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t af, bc, de, hl, af2, bc2, de2, hl2;
    uint16_t ix, iy, pc, sp;
    uint8_t i, r, im, iff1, iff2, halted, ei_delay, prefix;
} rs_z80;

typedef struct {
    void *context;
    uint8_t (*read)(void *, uint16_t);
    void (*write)(void *, uint16_t, uint8_t);
    uint8_t (*input)(void *, uint16_t);
    void (*output)(void *, uint16_t, uint8_t);
} rs_z80_bus;

/* The MSX interrupt bus floats high: IM 0 executes RST 38h. */
unsigned rs_z80_step(rs_z80 *cpu, const rs_z80_bus *bus, bool irq);

#endif
