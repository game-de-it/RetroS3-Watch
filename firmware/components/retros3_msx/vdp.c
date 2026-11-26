/* SPDX-License-Identifier: MIT */
#include "vdp.h"

#include <string.h>

static unsigned mode(const rs_vdp *v)
{
    return ((v->reg[0] & 14) << 1) | ((v->reg[1] >> 4) & 1) | ((v->reg[1] >> 2) & 2);
}

static unsigned ram_address(const rs_vdp *v, unsigned model, unsigned addr)
{
    unsigned m = mode(v);
    if (model == 1) return addr & 0x3fff;
    addr &= 0x1ffff;
    if (m == 20 || m == 28) return (addr >> 1) | ((addr & 1) << 16);
    return addr;
}

static uint16_t rgb3(unsigned r, unsigned g, unsigned b)
{
    return (r * 31 / 7 << 11) | (g * 63 / 7 << 5) | (b * 31 / 7);
}

static uint16_t direct(unsigned color)
{
    static const uint8_t blue[] = {0,2,4,7};
    return rgb3((color >> 2) & 7, color >> 5, blue[color & 3]);
}

void rs_vdp_reset(rs_vdp *v, unsigned model)
{
    static const uint16_t colors[] = {
        0000,0000,0161,0273,0117,0337,0511,0267,
        0711,0733,0661,0664,0141,0625,0555,0777
    };
    memset(v, 0, sizeof(*v));
    for (unsigned i = 0; i < 16; ++i)
        v->palette[i] = rgb3(colors[i] >> 6, (colors[i] >> 3) & 7, colors[i] & 7);
    v->status[0] = 31;
    v->status[1] = model == 3 ? 4 : 0;
    v->status[2] = 0x0c;
}

static void advance(rs_vdp *v, unsigned model)
{
    unsigned m = mode(v);
    if (model == 1 || m < 8) {
        /* Legacy display modes wrap the low 14 bits within the selected bank. */
        v->address = (v->address & 0x1c000) | ((v->address + 1) & 0x3fff);
    } else {
        v->address = (v->address + 1) & 0x1ffff;
        v->reg[14] = v->address >> 14;
    }
}

static unsigned bitmap_width(unsigned m) { return m == 16 || m == 20 ? 512 : 256; }
static unsigned bitmap_bits(unsigned m) { return m == 16 ? 2 : m == 28 ? 8 : 4; }

static unsigned pixel_address(const rs_vdp *v, unsigned x, unsigned y)
{
    unsigned m = mode(v), bpp = bitmap_bits(m);
    return ram_address(v, 2, ((y * bitmap_width(m) + (x & (bitmap_width(m) - 1))) * bpp) >> 3);
}

static unsigned pixel(const rs_vdp *v, const uint8_t *ram, unsigned x, unsigned y)
{
    unsigned bpp = bitmap_bits(mode(v));
    unsigned shift = 8 - bpp - ((x * bpp) & 7);
    return (ram[pixel_address(v, x, y)] >> shift) & ((1u << bpp) - 1);
}

static void plot(const rs_vdp *v, uint8_t *ram, unsigned x, unsigned y, unsigned value, unsigned op)
{
    unsigned bpp = bitmap_bits(mode(v)), mask = (1u << bpp) - 1;
    unsigned shift = 8 - bpp - ((x * bpp) & 7);
    unsigned addr = pixel_address(v, x, y), old = (ram[addr] >> shift) & mask;
    value &= mask;
    if ((op & 8) && !value) return;
    switch (op & 7) {
    case 0: break;
    case 1: value &= old; break;
    case 2: value |= old; break;
    case 3: value ^= old; break;
    case 4: value = ~value & mask; break;
    default: return;
    }
    ram[addr] = (ram[addr] & ~(mask << shift)) | (value << shift);
}

static unsigned coordinate(const rs_vdp *v, unsigned reg, unsigned mask)
{
    return (v->reg[reg] | (v->reg[reg + 1] << 8)) & mask;
}

static unsigned command_width(const rs_vdp *v, unsigned step)
{
    unsigned nx = coordinate(v, 40, 511);
    if (!nx) nx = 512;
    return (nx + step - 1) / step;
}

static void transfer(rs_vdp *v, uint8_t *ram, unsigned cmd)
{
    if (!(v->status[2] & 1)) return;
    unsigned step = cmd == 15 ? 8 / bitmap_bits(mode(v)) : 1;
    unsigned width = command_width(v, step), height = coordinate(v, 42, 1023);
    if (!height) height = 1024;
    int x = (int)(v->command_index % width * step);
    int y = (int)(v->command_index / width);
    if (v->reg[45] & 4) x = -x;
    if (v->reg[45] & 8) y = -y;
    unsigned cx = coordinate(v, cmd == 10 ? 32 : 36, 511) + x;
    unsigned cy = coordinate(v, cmd == 10 ? 34 : 38, 1023) + y;
    if (cmd == 10) v->status[7] = pixel(v, ram, cx, cy);
    else if (cmd == 11) plot(v, ram, cx, cy, v->reg[44], v->reg[46]);
    else ram[pixel_address(v, cx, cy)] = v->reg[44];
    if (++v->command_index >= width * height) v->status[2] &= ~0x81;
}

static unsigned bytes_until_edge(unsigned x, unsigned width, unsigned step, int direction)
{
    if (x >= width) return 0;
    return direction > 0 ? (width - x + step - 1) / step : x / step + 1;
}

static void bitmap_byte_row(uint8_t *dst, const uint8_t *src, unsigned count,
                            int direction, bool fill, uint8_t color)
{
    if (!count) return;
    int low = direction > 0 ? 0 : 1 - (int)count;
    if (fill) {
        memset(dst + low, color, count);
    } else if (dst != src) {
        /* Overlap is observable, since the VDP reads and writes in DIX order. */
        if (dst + low + count <= src + low || src + low + count <= dst + low)
            memcpy(dst + low, src + low, count);
        else
            for (unsigned i = 0; i < count; ++i) {
                int n = (int)i * direction;
                dst[n] = src[n];
            }
    }
}

static void command(rs_vdp *v, uint8_t *ram)
{
    unsigned cmd = v->reg[46] >> 4, m = mode(v), op = v->reg[46] & 15;
    v->status[2] &= ~0x81;
    if (m != 12 && m != 16 && m != 20 && m != 28) return;
    unsigned sx = coordinate(v, 32, 511), sy = coordinate(v, 34, 1023);
    unsigned dx = coordinate(v, 36, 511), dy = coordinate(v, 38, 1023);
    unsigned nx = coordinate(v, 40, 511), ny = coordinate(v, 42, 1023);
    unsigned color = v->reg[44];
    int ix = v->reg[45] & 4 ? -1 : 1, iy = v->reg[45] & 8 ? -1 : 1;
    switch (cmd) {
    case 4: v->status[7] = pixel(v, ram, sx, sy); return;
    case 5: plot(v, ram, dx, dy, color, op); return;
    case 6: {
        bool equal = !(v->reg[45] & 2);
        unsigned x = sx;
        for (; x < bitmap_width(m); x += ix)
            if ((pixel(v, ram, x, sy) == (color & ((1u << bitmap_bits(m)) - 1))) == equal) break;
        v->status[2] = (v->status[2] & ~0x10) | (x < bitmap_width(m) ? 0x10 : 0);
        v->status[8] = x;
        v->status[9] = 0xfe | ((x >> 8) & 1);
        return;
    }
    case 7: {
        int error = (int)(nx / 2);
        bool vertical = v->reg[45] & 1;
        for (unsigned i = 0; i <= nx; ++i) {
            plot(v, ram, dx, dy, color, op);
            if (vertical) dy += iy;
            else dx += ix;
            error -= (int)ny;
            if (error < 0) {
                if (vertical) dx += ix;
                else dy += iy;
                error += (int)nx;
            }
        }
        return;
    }
    case 10:
    case 11:
    case 15:
        v->command_index = 0;
        v->status[2] |= 0x81;
        if (cmd != 10) transfer(v, ram, cmd);
        return;
    case 8: case 9: case 12: case 13: case 14: break;
    default: return;
    }
    unsigned step = cmd >= 12 ? 8 / bitmap_bits(m) : 1;
    unsigned count = command_width(v, step);
    if (cmd == 14) count = (ix > 0 ? bitmap_width(m) - (dx & (bitmap_width(m) - 1)) :
                            (dx & (bitmap_width(m) - 1)) + 1) / step;
    if (cmd == 14) sx = dx;
    if (!ny) ny = 1024;
    if (cmd >= 12 && (m == 12 || m == 16)) {
        /* SCREEN 5/6 pack each scanline into 128 contiguous VRAM bytes. */
        unsigned dst_limit = bytes_until_edge(dx, bitmap_width(m), step, ix);
        if (count > dst_limit) count = dst_limit;
        if (cmd != 12) {
            unsigned src_limit = bytes_until_edge(sx, bitmap_width(m), step, ix);
            if (count > src_limit) count = src_limit;
        }
        if (!count) return;
        for (unsigned row = 0; row < ny && dy < 1024 && sy < 1024; ++row, dy += iy, sy += iy) {
            uint8_t *dst = ram + dy * 128 + dx / step;
            const uint8_t *src = cmd == 12 ? dst : ram + sy * 128 + sx / step;
            bitmap_byte_row(dst, src, count, ix, cmd == 12, color);
        }
        return;
    }
    for (unsigned row = 0; row < ny && dy < 1024 && sy < 1024; ++row, dy += iy, sy += iy) {
        unsigned tx = dx, fx = sx;
        for (unsigned col = 0; col < count && tx < bitmap_width(m) &&
             (cmd == 8 || cmd == 12 || fx < bitmap_width(m)); ++col, tx += ix * (int)step, fx += ix * (int)step) {
            if (cmd == 8 || cmd == 9)
                plot(v, ram, tx, dy, cmd == 8 ? color : pixel(v, ram, fx, sy), op);
            else ram[pixel_address(v, tx, dy)] = cmd == 12 ? color : ram[pixel_address(v, fx, sy)];
        }
    }
}

static void set_register(rs_vdp *v, uint8_t *ram, unsigned model, unsigned reg, uint8_t data)
{
    if (model == 1 && reg >= 8) return;
    if (model != 3 && reg >= 24 && reg < 32) return;
    v->reg[reg] = data;
    if (reg == 14) v->address = (v->address & 0x3fff) | ((data & 7) << 14);
    if (reg == 16) v->palette_pending = 0;
    if (reg == 46) command(v, ram);
    if (reg == 44) {
        unsigned cmd = v->reg[46] >> 4;
        if (cmd == 11 || cmd == 15) transfer(v, ram, cmd);
    }
}

uint8_t rs_vdp_read(rs_vdp *v, uint8_t *ram, unsigned model, unsigned port)
{
    v->control_pending = 0;
    if (port == 0) {
        uint8_t data = v->data;
        v->data = ram[ram_address(v, model, v->address)];
        advance(v, model);
        return data;
    }
    if (port != 1) return 255;
    unsigned n = model == 1 ? 0 : v->reg[15] & 15;
    if (n >= sizeof(v->status)) return 255;
    if (n == 7 && v->reg[46] >> 4 == 10) transfer(v, ram, 10);
    uint8_t result = v->status[n];
    if (n == 0) v->status[0] &= 31;
    if (n == 1) v->status[1] &= ~1;
    return result;
}

void rs_vdp_write(rs_vdp *v, uint8_t *ram, unsigned model, unsigned port, uint8_t data)
{
    if (port == 0) {
        v->control_pending = 0;
        ram[ram_address(v, model, v->address)] = data;
        v->data = data;
        advance(v, model);
    } else if (port == 1) {
        if (!v->control_pending) {
            v->first = data;
            v->control_pending = 1;
            return;
        }
        v->control_pending = 0;
        if (data & 128) {
            if (!(data & 64)) set_register(v, ram, model, data & (model == 1 ? 7 : 63), v->first);
        } else {
            v->address = ((model == 1 ? 0 : v->reg[14] & 7) << 14) | ((data & 63) << 8) | v->first;
            if (!(data & 64)) {
                v->data = ram[ram_address(v, model, v->address)];
                advance(v, model);
            }
        }
    } else if (model != 1 && port == 2) {
        if (v->palette_pending) {
            v->palette[v->reg[16] & 15] = rgb3((v->palette_pending >> 4) & 7, data & 7, v->palette_pending & 7);
            v->reg[16] = (v->reg[16] + 1) & 15;
            v->palette_pending = 0;
        } else v->palette_pending = data | 128;
    } else if (model != 1 && port == 3) {
        unsigned n = v->reg[17] & 63;
        if (n != 17) set_register(v, ram, model, n, data);
        if (!(v->reg[17] & 128)) v->reg[17] = (n + 1) & 63;
    }
}

unsigned rs_vdp_lines(const rs_vdp *v, unsigned model) { return model != 1 && (v->reg[9] & 2) ? 313 : 262; }
unsigned rs_vdp_height(const rs_vdp *v, unsigned model) { return model != 1 && (v->reg[9] & 128) ? 212 : 192; }

static unsigned pattern_color(const rs_vdp *v, const uint8_t *ram, unsigned m, unsigned x, unsigned y)
{
    unsigned name = (v->reg[2] & 0x7f) << 10;
    unsigned pattern = (v->reg[4] & 63) << 11;
    unsigned color = ((v->reg[10] & 7) << 14) | (v->reg[3] << 6);
    if (m == 1 || m == 9) {
        unsigned columns = m == 1 ? 40 : 80;
        if (x < 8 || x >= columns * 6 + 8) return v->reg[7] & 15;
        x -= 8;
        unsigned cell = y / 8 * columns + x / 6;
        unsigned ch = ram[((m == 9 ? name & ~0xfffu : name) + cell) & 0x1ffff];
        unsigned colors = v->reg[7];
        if (m == 9 && (v->reg[13] >> 4) && (ram[(color + (cell >> 3)) & 0x1ffff] & (128 >> (cell & 7))))
            colors = v->reg[12];
        unsigned bits = ram[(pattern + ch * 8 + (y & 7)) & 0x1ffff];
        return ((bits & (128 >> (x % 6))) ? colors >> 4 : colors) & 15;
    }
    unsigned ch = ram[(name + (y / 8) * 32 + x / 8) & 0x1ffff];
    if (m == 2) {
        unsigned pair = ram[(pattern + ch * 8 + ((y >> 2) & 7)) & 0x1ffff];
        return (x & 4) ? pair & 15 : pair >> 4;
    }
    unsigned row = ch * 8 + (y & 7);
    unsigned ca = color + ch / 8, pa = pattern + row;
    if (m == 4 || m == 8) {
        row |= (y & 0xc0) << 5;
        pa = (pattern & ~0x1fffu) | (row & ((pattern & 0x1fff) | 0x7ff));
        ca = (color & ~0x1fffu) | (row & ((color & 0x1fff) | 63));
    }
    unsigned colors = ram[ca & 0x1ffff], bits = ram[pa & 0x1ffff];
    return (bits & (128 >> (x & 7))) ? colors >> 4 : colors & 15;
}

static unsigned clamp5(int x) { return x < 0 ? 0 : x > 31 ? 31 : (unsigned)x; }
static int signed6(unsigned v) { return v < 32 ? (int)v : (int)v - 64; }

static uint16_t bitmap_color(const rs_vdp *v, const uint8_t *ram, unsigned model, unsigned m,
                             unsigned x, unsigned y, unsigned backdrop)
{
    unsigned page_mask = m == 20 || m == 28 ? 0x20 : 0x60;
    unsigned page = (v->reg[2] & page_mask) << 3;
    y += page;
    unsigned c = pixel(v, ram, x, y);
    if (m == 28) {
        if (model == 3 && (v->reg[25] & 8)) {
            if ((v->reg[25] & 16) && (c & 8)) return v->palette[c >> 4];
            unsigned group = x & ~3u;
            int k = signed6((pixel(v, ram, group, y) & 7) | ((pixel(v, ram, group + 1, y) & 7) << 3));
            int j = signed6((pixel(v, ram, group + 2, y) & 7) | ((pixel(v, ram, group + 3, y) & 7) << 3));
            int luma = (int)(c >> 3);
            return (clamp5(luma + j) << 11) | (clamp5(luma + k) * 63 / 31 << 5) |
                   clamp5((5 * luma - 2 * j - k) / 4);
        }
        return direct(c || (v->reg[8] & 32) ? c : backdrop);
    }
    return v->palette[c || (v->reg[8] & 32) ? c : backdrop & ((1u << bitmap_bits(m)) - 1)];
}

/* SCREEN 5 stores two adjacent palette indices in each contiguous VRAM byte. */
static void screen5_line(const rs_vdp *v, const uint8_t *ram, unsigned y, uint16_t *out)
{
    unsigned base = ((v->reg[2] & 0x60) << 10) | ((y & 255) << 7);
    const uint8_t *source = ram + base;
    uint16_t palette[16];
    memcpy(palette, v->palette, sizeof(palette));
    if (!(v->reg[8] & 32)) palette[0] = palette[v->reg[7] & 15];
    for (unsigned i = 0; i < 128; ++i) {
        uint8_t pair = source[i];
        out[i * 2] = palette[pair >> 4];
        out[i * 2 + 1] = palette[pair & 15];
    }
}

/* SCREEN 2/4 share one pattern byte and one color byte across each eight dots. */
static void graphic_tiles_line(const rs_vdp *v, const uint8_t *ram, unsigned y, uint16_t *out)
{
    unsigned name = ((v->reg[2] & 0x7f) << 10) + (y / 8) * 32;
    unsigned pattern = (v->reg[4] & 63) << 11;
    unsigned color = ((v->reg[10] & 7) << 14) | (v->reg[3] << 6);
    unsigned pattern_mask = (pattern & 0x1fff) | 0x7ff;
    unsigned color_mask = (color & 0x1fff) | 63;
    unsigned row = ((y & 0xc0) << 5) | (y & 7);
    uint16_t palette[16];
    memcpy(palette, v->palette, sizeof(palette));
    if (!(v->reg[8] & 32)) palette[0] = palette[v->reg[7] & 15];
    for (unsigned tile = 0; tile < 32; ++tile) {
        unsigned cell = row | (ram[(name + tile) & 0x1ffff] << 3);
        unsigned bits = ram[(pattern & ~0x1fffu) | (cell & pattern_mask)];
        unsigned pair = ram[(color & ~0x1fffu) | (cell & color_mask)];
        uint16_t fg = palette[pair >> 4], bg = palette[pair & 15];
        for (unsigned dot = 0; dot < 8; ++dot)
            *out++ = (bits & (128 >> dot)) ? fg : bg;
    }
}

static void sprites(rs_vdp *v, const uint8_t *ram, unsigned model, unsigned m, unsigned y,
                    uint16_t *out, unsigned width)
{
    unsigned size = v->reg[1] & 2 ? 16 : 8, mag = v->reg[1] & 1 ? 2 : 1;
    bool modern = model != 1 && m >= 8;
    /* V9938/V9958 use R5 bit 7 as VRAM address bit 14. TMS9918A has a
     * 14-bit VRAM bus and uses only R5 bits 6:0. */
    unsigned attrs = model == 1 ? (v->reg[5] & 127) << 7 :
                                  ((v->reg[11] & 3) << 15) | (v->reg[5] << 7);
    if (modern) attrs = (attrs & ~511u) | 512;
    unsigned patterns = (v->reg[6] & (model == 1 ? 7 : 63)) << 11;
    uint8_t colors[256] = {0};
    uint8_t occupied[256] = {0};
    unsigned count = 0, group = 0;
    unsigned scale = width == 512 ? 2 : 1;
    unsigned row_y = modern ? (y + v->reg[23]) & 255 : y;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned a = attrs + i * 4;
        unsigned top = ram[ram_address(v, model, a)];
        if (top == (modern ? 216u : 208u)) break;
        unsigned row = (row_y - top - 1) & 255;
        if (row >= size * mag) continue;
        if (++count > (modern ? 8u : 4u)) {
            if (!(v->status[0] & 64)) v->status[0] = (v->status[0] & 0xa0) | 64 | i;
            break;
        }
        int x = ram[ram_address(v, model, a + 1)];
        unsigned pattern = ram[ram_address(v, model, a + 2)];
        unsigned flags = ram[ram_address(v, model, modern ? attrs - 512 + i * 16 + row / mag : a + 3)];
        bool combine = modern && (flags & 64);
        /* A CC line belongs to the preceding ordinary sprite on this
         * scanline. It can supply dots where its group's base has no dot. */
        if (!combine) group = i + 1;
        else if (!group || !out) continue;
        bool collidable = !combine && (!modern || !(flags & 32));
        if (flags & 128) x -= 32;
        unsigned c = flags & 15;
        if (!c && !(v->reg[8] & 32)) continue;
        if (size == 16) pattern &= ~3u;
        unsigned addr = patterns + pattern * 8 + row / mag;
        unsigned bits = (unsigned)ram[ram_address(v, model, addr)] << 8;
        if (size == 16) bits |= ram[ram_address(v, model, addr + 16)];
        for (unsigned col = 0; col < size; ++col) {
            if (!(bits & (0x8000u >> col))) continue;
            for (unsigned k = 0; k < mag; ++k) {
                unsigned pos = x + col * mag + k;
                if (pos >= 256) continue;
                unsigned owner = occupied[pos] & 63;
                if (collidable && !(v->status[0] & 32)) {
                    if (occupied[pos] & 128) {
                        v->status[0] |= 32;
                        v->status[3] = pos + 12;
                        v->status[4] = (pos + 12) >> 8;
                        v->status[5] = y + 8;
                        v->status[6] = (y + 8) >> 8;
                    } else occupied[pos] |= 128;
                }
                if (out && (!owner || (combine && owner == group))) {
                    occupied[pos] = (occupied[pos] & 128) | group;
                    colors[pos] = owner ? colors[pos] | c : c;
                    out[pos * scale] = v->palette[colors[pos]];
                    if (scale == 2) out[pos * 2 + 1] = out[pos * 2];
                }
            }
        }
    }
}

void rs_vdp_scanline(rs_vdp *v, uint8_t *ram, unsigned model, uint16_t *out, unsigned width)
{
    unsigned y = v->line, h = rs_vdp_height(v, model), m = mode(v);
    if (model != 1 && y < 256 && ((y + v->reg[23]) & 255) == v->reg[19]) v->status[1] |= 1;
    if (y == h) {
        v->status[0] |= 128;
        v->status[2] |= 64;
    }
    if (y >= 212) return;
    unsigned backdrop = m == 28 ? v->reg[7] : v->reg[7] & 15;
    bool visible = y < h && (v->reg[1] & 64);
    bool wide = m == 9 || m == 16 || m == 20;
    bool bitmap = m == 12 || m == 16 || m == 20 || m == 28;
    bool mask_left = model == 3 && (v->reg[25] & 2);
    unsigned src_y = model == 1 ? y : (y + v->reg[23]) & 255;
    unsigned scroll = 0;
    if (model == 3 && bitmap) scroll = ((v->reg[26] & 63) * 8 - (v->reg[27] & 7)) & 511;
    if (out) {
        if (!visible) {
            uint16_t color = m == 28 ? direct(backdrop) : v->palette[backdrop];
            for (unsigned col = 0; col < width; ++col) out[col] = color;
        } else if (!mask_left && width == 256 && m == 12 && !(scroll & 255)) {
            screen5_line(v, ram, src_y, out);
        } else if (!mask_left && width == 256 && (m == 4 || m == 8)) {
            graphic_tiles_line(v, ram, src_y, out);
        } else {
            for (unsigned col = 0; col < width; ++col) {
                unsigned x = wide ? (width == 256 ? col * 2 : col) : (width == 512 ? col / 2 : col);
                if (mask_left && x < (wide ? 16u : 8u)) {
                    out[col] = m == 28 ? direct(backdrop) : v->palette[backdrop];
                    continue;
                }
                if (scroll) x = (x + scroll * (wide ? 2 : 1)) & (bitmap_width(m) - 1);
                if (bitmap) out[col] = bitmap_color(v, ram, model, m, x, src_y, backdrop);
                else {
                    unsigned c = pattern_color(v, ram, m, x, src_y);
                    out[col] = v->palette[c || (v->reg[8] & 32) ? c : backdrop];
                }
            }
        }
    }
    if (visible && m != 1 && m != 9 && !(v->reg[8] & 2)) sprites(v, ram, model, m, y, out, width);
}
