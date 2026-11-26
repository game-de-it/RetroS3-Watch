/* SPDX-License-Identifier: MIT */
#include "z80.h"

enum { C=1, N=2, P=4, X=8, H=16, Y=32, Z=64, S=128 };

static uint8_t hi(uint16_t v) { return v >> 8; }
static void set_hi(uint16_t *p, uint8_t v) { *p = (*p & 255) | (v << 8); }
static void set_lo(uint16_t *p, uint8_t v) { *p = (*p & 0xff00) | v; }
static uint8_t read8(const rs_z80_bus *b, uint16_t a) { return b->read(b->context, a); }
static void write8(const rs_z80_bus *b, uint16_t a, uint8_t v) { b->write(b->context, a, v); }
static uint8_t input(const rs_z80_bus *b, uint16_t a) { return b->input(b->context, a); }
static void output(const rs_z80_bus *b, uint16_t a, uint8_t v) { b->output(b->context, a, v); }
static uint16_t read16(const rs_z80_bus *b, uint16_t a)
{
    uint8_t lo = read8(b, a);
    return lo | (read8(b, a + 1) << 8);
}
static void write16(const rs_z80_bus *b, uint16_t a, uint16_t v)
{
    write8(b, a, v);
    write8(b, a + 1, hi(v));
}
static uint8_t next(rs_z80 *c, const rs_z80_bus *b) { return read8(b, c->pc++); }
static uint16_t immediate(rs_z80 *c, const rs_z80_bus *b)
{
    uint16_t v = read16(b, c->pc);
    c->pc += 2;
    return v;
}
static void refresh(rs_z80 *c) { c->r = (c->r & 128) | ((c->r + 1) & 127); }
static uint8_t fetch(rs_z80 *c, const rs_z80_bus *b)
{
    refresh(c);
    return next(c, b);
}
static uint16_t pop(rs_z80 *c, const rs_z80_bus *b)
{
    uint16_t v = read16(b, c->sp);
    c->sp += 2;
    return v;
}
static void push(rs_z80 *c, const rs_z80_bus *b, uint16_t v)
{
    write8(b, --c->sp, hi(v));
    write8(b, --c->sp, v);
}
static uint8_t sz(uint8_t v) { return (v & (S|Y|X)) | (v ? 0 : Z); }
static uint8_t parity(uint8_t v)
{
    v ^= v >> 4;
    return ((0x9669u >> (v & 15)) & 1) ? P : 0;
}
static uint8_t szp(uint8_t v) { return sz(v) | parity(v); }
static bool condition(const rs_z80 *c, unsigned y)
{
    static const uint8_t masks[] = {Z,C,P,S};
    return !!(c->af & masks[y >> 1]) == (bool)(y & 1);
}
static uint16_t *pair(rs_z80 *c, uint16_t *hl, unsigned p)
{
    switch (p) {
    case 0: return &c->bc;
    case 1: return &c->de;
    case 2: return hl;
    default: return &c->sp;
    }
}
static uint8_t reg(rs_z80 *c, uint16_t *hl, unsigned r)
{
    if (r == 7) return hi(c->af);
    uint16_t v = *pair(c, hl, r >> 1);
    return r & 1 ? v & 255 : hi(v);
}
static void set_reg(rs_z80 *c, uint16_t *hl, unsigned r, uint8_t v)
{
    uint16_t *p = r == 7 ? &c->af : pair(c, hl, r >> 1);
    if ((r & 1) && r != 7) set_lo(p, v);
    else set_hi(p, v);
}
static uint16_t address(rs_z80 *c, const rs_z80_bus *b, uint16_t *hl)
{
    return hl == &c->hl ? c->hl : *hl + (int8_t)next(c, b);
}
static void alu(rs_z80 *c, unsigned y, uint8_t v)
{
    uint8_t a = hi(c->af), f, r;
    unsigned carry = (y == 1 || y == 3) && (c->af & C);
    if (y < 4 || y == 7) {
        bool sub = y >= 2;
        unsigned wide = sub ? (unsigned)a - v - carry : (unsigned)a + v + carry;
        r = wide;
        f = sz(r) | ((wide >> 8) & C) | ((a ^ v ^ r) & H);
        if (sub) f |= N | (((a ^ v) & (a ^ r) & S) ? P : 0);
        else if ((~(a ^ v) & (a ^ r) & S)) f |= P;
        if (y == 7) {
            f = (f & ~(X|Y)) | (v & (X|Y));
            r = a;
        }
    } else {
        if (y == 4) r = a & v;
        else if (y == 5) r = a ^ v;
        else r = a | v;
        f = szp(r) | (y == 4 ? H : 0);
    }
    c->af = (r << 8) | f;
}
static void add16(rs_z80 *c, uint16_t *dest, uint16_t v, bool full, bool sub)
{
    unsigned a = *dest, carry = full && (c->af & C);
    unsigned wide = sub ? a - v - carry : a + v + carry;
    uint16_t r = wide;
    uint8_t f = (hi(r) & (X|Y)) | ((wide >> 16) & C) | (((a ^ v ^ r) >> 8) & H);
    if (full) {
        unsigned sign = sub ? (a ^ v) : ~(a ^ v);
        f |= (hi(r) & S) | (r ? 0 : Z) | (sub ? N : 0);
        if (sign & (a ^ r) & 0x8000) f |= P;
    } else f |= c->af & (S|Z|P);
    *dest = r;
    set_lo(&c->af, f);
}
static void accumulator(rs_z80 *c, unsigned y)
{
    uint8_t a = hi(c->af), f = c->af;
    if (y < 4) {
        unsigned old_c = f & C;
        unsigned bit = (y & 1) ? a & 1 : a >> 7;
        unsigned insert = y >= 2 ? old_c : bit;
        a = (y & 1) ? (a >> 1) | (insert << 7) : (a << 1) | insert;
        f = (f & (S|Z|P)) | (a & (X|Y)) | bit;
    } else if (y == 4) {
        unsigned correction = 0;
        if ((f & H) || (a & 15) > 9) correction |= 6;
        if ((f & C) || a > 0x99) correction |= 0x60;
        uint8_t r = (f & N) ? a - correction : a + correction;
        f = (f & N) | szp(r) | ((a ^ r) & H) | (correction >= 0x60 ? C : 0);
        a = r;
    } else if (y == 5) {
        a = ~a;
        f = (f & (S|Z|P|C)) | H | N | (a & (X|Y));
    } else {
        f = (f & (S|Z|P)) | (a & (X|Y)) |
            (y == 6 ? C : (f & C ? H : C));
    }
    c->af = (a << 8) | f;
}
static unsigned cb(rs_z80 *c, const rs_z80_bus *b, uint16_t *hl)
{
    bool indexed = hl != &c->hl;
    uint16_t addr = address(c, b, hl);
    unsigned op = indexed ? next(c, b) : fetch(c, b);
    unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7;
    bool memory = indexed || z == 6;
    uint8_t v = memory ? read8(b, addr) : reg(c, &c->hl, z);
    if (x == 1) {
        bool set = (v >> y) & 1;
        uint8_t xy = indexed ? hi(addr) : v;
        set_lo(&c->af, (c->af & C) | H | (xy & (X|Y)) |
               (set ? (y == 7 ? S : 0) : Z|P));
        return indexed ? 16 : memory ? 12 : 8;
    }
    if (x == 0) {
        unsigned carry = (y & 1) ? v & 1 : v >> 7;
        unsigned insert = y < 2 ? carry : y < 4 ? c->af & C : y == 6;
        if (y == 5) insert = v >> 7;
        v = (y & 1) ? (v >> 1) | (insert << 7) : (v << 1) | insert;
        set_lo(&c->af, szp(v) | carry);
    } else if (x == 2) v &= ~(1u << y);
    else v |= 1u << y;
    if (memory) write8(b, addr, v);
    if (z != 6) set_reg(c, &c->hl, z, v);
    return indexed ? 19 : memory ? 15 : 8;
}
static unsigned block(rs_z80 *c, const rs_z80_bus *b, unsigned y, unsigned z)
{
    int direction = y & 1 ? -1 : 1;
    bool again;
    uint8_t v, sum, f;
    if (z == 0) {
        v = read8(b, c->hl);
        write8(b, c->de, v);
        c->de += direction;
        again = --c->bc != 0;
        sum = hi(c->af) + v;
        f = (c->af & (S|Z|C)) | (again ? P : 0) | (sum & X) | ((sum & 2) << 4);
    } else if (z == 1) {
        v = read8(b, c->hl);
        uint8_t a = hi(c->af), r = a - v;
        unsigned half = (a ^ v ^ r) & H;
        sum = r - !!half;
        --c->bc;
        again = c->bc && r;
        f = (sz(r) & (S|Z)) | (c->af & C) | N | half |
            (c->bc ? P : 0) | (sum & X) | ((sum & 2) << 4);
    } else {
        if (z == 2) {
            v = input(b, c->bc);
            write8(b, c->hl, v);
        } else v = read8(b, c->hl);
        set_hi(&c->bc, hi(c->bc) - 1);
        if (z == 3) output(b, c->bc, v);
        uint8_t offset = z == 2 ? (uint8_t)(c->bc + direction) : (uint8_t)(c->hl + direction);
        unsigned total = v + offset;
        f = sz(hi(c->bc)) | (v & 128 ? N : 0) | (total > 255 ? H|C : 0) |
            parity((total & 7) ^ hi(c->bc));
        again = hi(c->bc) != 0;
    }
    c->hl += direction;
    set_lo(&c->af, f);
    if (y >= 6 && again) {
        c->pc -= 2;
        return 21;
    }
    return 16;
}
static unsigned ed(rs_z80 *c, const rs_z80_bus *b)
{
    unsigned op = fetch(c, b), y = (op >> 3) & 7, z = op & 7;
    if (op >= 0xa0 && y >= 4 && z < 4) return block(c, b, y, z);
    if ((op & 0xc0) != 0x40) return 8;
    uint16_t *p = pair(c, &c->hl, y >> 1);
    switch (z) {
    case 0: {
        uint8_t v = input(b, c->bc);
        if (y != 6) set_reg(c, &c->hl, y, v);
        set_lo(&c->af, (c->af & C) | szp(v));
        return 12;
    }
    case 1: output(b, c->bc, y == 6 ? 0 : reg(c, &c->hl, y)); return 12;
    case 2: add16(c, &c->hl, *p, true, !(y & 1)); return 15;
    case 3: {
        uint16_t addr = immediate(c, b);
        if (y & 1) *p = read16(b, addr);
        else write16(b, addr, *p);
        return 20;
    }
    case 4: {
        uint8_t a = hi(c->af);
        set_hi(&c->af, 0);
        alu(c, 2, a);
        return 8;
    }
    case 5: c->pc = pop(c, b); c->iff1 = c->iff2; return 14;
    case 6: {
        static const uint8_t modes[] = {0,0,1,2,0,0,1,2};
        c->im = modes[y];
        return 8;
    }
    default:
        if (y == 0) c->i = hi(c->af);
        else if (y == 1) c->r = hi(c->af);
        else if (y < 4) {
            uint8_t v = y == 2 ? c->i : c->r;
            c->af = (v << 8) | sz(v) | (c->af & C) | (c->iff2 ? P : 0);
        } else if (y < 6) {
            uint8_t a = hi(c->af), v = read8(b, c->hl), low = a & 15;
            a = (a & 0xf0) | (y == 4 ? v & 15 : v >> 4);
            write8(b, c->hl, y == 4 ? (low << 4) | (v >> 4) : (v << 4) | low);
            c->af = (a << 8) | szp(a) | (c->af & C);
            return 18;
        } else return 8;
        return 9;
    }
}
static unsigned base(rs_z80 *c, const rs_z80_bus *b, uint16_t *hl, unsigned op)
{
    unsigned x = op >> 6, y = (op >> 3) & 7, z = op & 7, p = y >> 1;
    uint16_t *rp = pair(c, hl, p);
    bool idx = hl != &c->hl;
    if (x == 1) {
        if (op == 0x76) { c->halted = 1; return 4; }
        if (z == 6 || y == 6) {
            uint16_t addr = address(c, b, hl);
            if (z == 6) set_reg(c, &c->hl, y, read8(b, addr));
            else write8(b, addr, reg(c, &c->hl, z));
            return idx ? 15 : 7;
        }
        set_reg(c, hl, y, reg(c, hl, z));
        return 4;
    }
    if (x == 2) {
        alu(c, y, z == 6 ? read8(b, address(c, b, hl)) : reg(c, hl, z));
        return z == 6 ? (idx ? 15 : 7) : 4;
    }
    if (x == 0) {
        switch (z) {
        case 0:
            if (y == 0) return 4;
            if (y == 1) {
                uint16_t v = c->af; c->af = c->af2; c->af2 = v;
                return 4;
            }
            {
                int8_t d = next(c, b);
                if (y == 2) set_hi(&c->bc, hi(c->bc) - 1);
                if (y == 3 || (y == 2 ? hi(c->bc) != 0 : condition(c, y - 4))) {
                    c->pc += d;
                    return y == 2 ? 13 : 12;
                }
                return y == 2 ? 8 : 7;
            }
        case 1:
            if (y & 1) { add16(c, hl, *rp, false, false); return 11; }
            *rp = immediate(c, b);
            return 10;
        case 2: {
            uint16_t addr = p < 2 ? *rp : immediate(c, b);
            if (p == 2) {
                if (y & 1) *hl = read16(b, addr);
                else write16(b, addr, *hl);
                return 16;
            }
            if (y & 1) set_hi(&c->af, read8(b, addr));
            else write8(b, addr, hi(c->af));
            return p == 3 ? 13 : 7;
        }
        case 3: *rp += y & 1 ? -1 : 1; return 6;
        case 4:
        case 5: {
            bool dec = z == 5;
            uint16_t addr = y == 6 ? address(c, b, hl) : 0;
            uint8_t v = y == 6 ? read8(b, addr) : reg(c, hl, y);
            uint8_t r = v + (dec ? -1 : 1);
            set_lo(&c->af, (c->af & C) | sz(r) | ((v ^ r) & H) |
                   (dec ? N : 0) | (v == (dec ? 0x80 : 0x7f) ? P : 0));
            if (y == 6) write8(b, addr, r);
            else set_reg(c, hl, y, r);
            return y == 6 ? (idx ? 19 : 11) : 4;
        }
        case 6:
            if (y == 6) {
                uint16_t addr = address(c, b, hl);
                write8(b, addr, next(c, b));
                return idx ? 15 : 10;
            }
            set_reg(c, hl, y, next(c, b));
            return 7;
        default: accumulator(c, y); return 4;
        }
    }
    switch (z) {
    case 0:
        if (condition(c, y)) { c->pc = pop(c, b); return 11; }
        return 5;
    case 1:
        if (!(y & 1)) { *(p == 3 ? &c->af : rp) = pop(c, b); return 10; }
        if (p == 0) { c->pc = pop(c, b); return 10; }
        if (p == 1) {
            uint16_t v = c->bc; c->bc = c->bc2; c->bc2 = v;
            v = c->de; c->de = c->de2; c->de2 = v;
            v = c->hl; c->hl = c->hl2; c->hl2 = v;
            return 4;
        }
        if (p == 2) { c->pc = *hl; return 4; }
        c->sp = *hl;
        return 6;
    case 2: {
        uint16_t addr = immediate(c, b);
        if (condition(c, y)) c->pc = addr;
        return 10;
    }
    case 3:
        switch (y) {
        case 0: c->pc = immediate(c, b); return 10;
        case 2:
        case 3: {
            uint16_t port = (c->af & 0xff00) | next(c, b);
            if (y == 2) output(b, port, hi(c->af));
            else set_hi(&c->af, input(b, port));
            return 11;
        }
        case 4: {
            uint16_t v = read16(b, c->sp);
            write8(b, c->sp + 1, hi(*hl));
            write8(b, c->sp, *hl);
            *hl = v;
            return 19;
        }
        case 5: { uint16_t v = c->de; c->de = c->hl; c->hl = v; return 4; }
        case 6: c->iff1 = c->iff2 = 0; return 4;
        case 7: c->iff1 = c->iff2 = 1; c->ei_delay = 1; return 4;
        default: return 4; /* CB is decoded before reaching base(). */
        }
    case 4:
    case 5:
        if (z == 5 && !(y & 1)) {
            push(c, b, p == 3 ? c->af : *rp);
            return 11;
        }
        {
            uint16_t addr = immediate(c, b);
            if (z == 5 || condition(c, y)) {
                push(c, b, c->pc);
                c->pc = addr;
                return 17;
            }
            return 10;
        }
    case 6: alu(c, y, next(c, b)); return 7;
    default: push(c, b, c->pc); c->pc = y * 8; return 11;
    }
}

unsigned rs_z80_step(rs_z80 *c, const rs_z80_bus *b, bool irq)
{
    if (irq && c->iff1 && !c->ei_delay && !c->prefix) {
        c->iff1 = c->iff2 = c->halted = 0;
        refresh(c);
        push(c, b, c->pc);
        c->pc = c->im == 2 ? read16(b, (c->i << 8) | 255) : 0x38;
        return c->im == 2 ? 19 : 13;
    }
    if (c->halted) { refresh(c); return 4; }
    unsigned cycles = 0;
    unsigned op = fetch(c, b);
    while (op == 0xdd || op == 0xfd) {
        c->prefix = op;
        cycles += 4;
        if (cycles == 64) return cycles; /* A stream of prefixes cannot stall a frame. */
        op = fetch(c, b);
    }
    uint16_t *hl = c->prefix == 0xdd ? &c->ix : c->prefix == 0xfd ? &c->iy : &c->hl;
    c->prefix = 0;
    c->ei_delay = 0;
    if (op == 0xcb) return cycles + cb(c, b, hl);
    if (op == 0xed) return cycles + ed(c, b);
    return cycles + base(c, b, hl, op);
}
