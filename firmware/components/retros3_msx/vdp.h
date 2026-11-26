/* SPDX-License-Identifier: MIT */
#ifndef RETROS3_VDP_H
#define RETROS3_VDP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t address;
    uint32_t command_index;
    uint16_t palette[16];
    uint16_t line;
    uint8_t reg[64], status[10];
    uint8_t data, first, control_pending, palette_pending;
} rs_vdp;

void rs_vdp_reset(rs_vdp *v, unsigned model);
uint8_t rs_vdp_read(rs_vdp *v, uint8_t *ram, unsigned model, unsigned port);
void rs_vdp_write(rs_vdp *v, uint8_t *ram, unsigned model, unsigned port, uint8_t data);
unsigned rs_vdp_lines(const rs_vdp *v, unsigned model);
unsigned rs_vdp_height(const rs_vdp *v, unsigned model);
static inline bool rs_vdp_irq(const rs_vdp *v)
{
    return ((v->reg[1] & 32) && (v->status[0] & 128)) ||
           ((v->reg[0] & 16) && (v->status[1] & 1));
}
void rs_vdp_scanline(rs_vdp *v, uint8_t *ram, unsigned model, uint16_t *pixels, unsigned width);

#endif
