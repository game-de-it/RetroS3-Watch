/* SPDX-License-Identifier: MIT */
#include "retros3_msx.h"
#include "media.h"
#include "vdp.h"
#include "z80.h"

#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

enum {
    CPU_HZ = 3579545, LINE_CYCLES = 228, VRAM_SIZE = 128 * 1024,
    MAX_ROM = 4 * 1024 * 1024, PAGE_SIZE = 16384, STATE_HEADER = 20
};

typedef struct { uint8_t *bytes; uint32_t size, checksum; } image;
typedef struct {
    uint32_t tone[3], noise, envelope, random;
    uint8_t reg[16], select, phase, env_step, env_attack, env_hold;
} psg_state;
typedef struct {
    uint32_t tick[5];
    uint8_t wave[128], freq[10], volume[5], enable;
} scc_state;
typedef struct {
    rs_z80 cpu;
    rs_vdp vdp;
    rs_fdc disk;
    rs_tape tape;
    psg_state psg;
    scc_state scc;
    uint32_t audio_clock, clock_cycles;
    uint16_t line_overrun, frame_cycles_div4;
    uint8_t primary, secondary[4], bank[4], cart_bank[4], ppi_c;
    uint8_t rtc[4][13], rtc_select, rtc_mode;
} machine_state;

_Static_assert(sizeof(machine_state) == 480, "v1 state header requires the documented layout");

struct rs_msx {
    rs_msx_config config;
    size_t ram_size;
    uint8_t *ram, *vram;
    image rom[4][4][4], cart, cas, disk_image[2];
    rs_disk drives[2];
    unsigned disk_slot, disk_subslot, cart_slot, linear_start;
    rs_cart_mapper mapper;
    uint8_t keys[16], joystick[2];
    machine_state state;
    uint8_t *read_page[4], *write_page[4];
    unsigned primary_page[4], secondary_page[4];
    bool ready;
};

static void *large_alloc(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(size);
#endif
}

static uint32_t crc(uint32_t value, const void *bytes, size_t size)
{
    static const uint32_t table[16] = {
        0,0x1db71064,0x3b6e20c8,0x26d930ac,0x76dc4190,0x6b6b51f4,0x4db26158,0x5005713c,
        0xedb88320,0xf00f9344,0xd6d6a3e8,0xcb61b38c,0x9b64c2b0,0x86d3d2d4,0xa00ae278,0xbdbdf21c
    };
    const uint8_t *p = bytes;
    while (size--) {
        value ^= *p++;
        value = (value >> 4) ^ table[value & 15];
        value = (value >> 4) ^ table[value & 15];
    }
    return value;
}

static bool replace_image(image *dest, const void *data, size_t size)
{
    if (!data && size) return false;
    uint8_t *copy = size ? large_alloc(size) : NULL;
    if (size && !copy) return false;
    if (size) memcpy(copy, data, size);
    free(dest->bytes);
    *dest = (image){copy, (uint32_t)size, crc(0, data, size)};
    return true;
}

rs_msx_config rs_msx_defaults(rs_msx_model model)
{
    return (rs_msx_config){model, model == RS_MSX1 ? 64u : 256u, 256, false,
                           model == RS_MSX1 ? 0u : 8u, 3, 0};
}

rs_msx *rs_msx_create(const rs_msx_config *config)
{
    if (!config || config->model < RS_MSX1 || config->model > RS_MSX2_PLUS ||
        config->ram_kib < 64 || config->ram_kib > 4096 ||
        (config->ram_kib & (config->ram_kib - 1)) ||
        (config->model == RS_MSX1 && config->ram_kib != 64) ||
        (config->video_width != 256 && config->video_width != 512) ||
        config->expanded_slots > 15 || config->ram_slot > 3 || config->ram_subslot > 3 ||
        (config->ram_subslot && !(config->expanded_slots & (1u << config->ram_slot)))) return NULL;

    rs_msx *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->config = *config;
    m->disk_slot = 4;
    m->ram_size = config->ram_kib * 1024;
    m->ram = large_alloc(m->ram_size);
    m->vram = large_alloc(VRAM_SIZE);
    if (!m->ram || !m->vram) { rs_msx_destroy(m); return NULL; }
    return m;
}

void rs_msx_destroy(rs_msx *m)
{
    if (!m) return;
    for (unsigned s = 0; s < 4; ++s)
        for (unsigned u = 0; u < 4; ++u)
            for (unsigned p = 0; p < 4; ++p) free(m->rom[s][u][p].bytes);
    free(m->ram);
    free(m->vram);
    free(m->cart.bytes);
    free(m->cas.bytes);
    for (unsigned i = 0; i < 2; ++i) free(m->disk_image[i].bytes);
    free(m);
}

static void map_pages(rs_msx *m)
{
    for (unsigned p = 0; p < 4; ++p) {
        unsigned s = (m->state.primary >> (p * 2)) & 3;
        unsigned u = m->config.expanded_slots & (1u << s) ? (m->state.secondary[s] >> (p * 2)) & 3 : 0;
        m->primary_page[p] = s;
        m->secondary_page[p] = u;
        m->write_page[p] = NULL;
        m->read_page[p] = m->rom[s][u][p].bytes;
        if (s == m->config.ram_slot && u == m->config.ram_subslot) {
            unsigned bank = m->config.model == RS_MSX1 ? p : m->state.bank[p];
            m->write_page[p] = m->ram + ((bank * PAGE_SIZE) & (m->ram_size - 1));
            m->read_page[p] = m->write_page[p];
        } else if (s == m->cart_slot && m->cart.bytes) {
            if (m->mapper != RS_CART_LINEAR) m->read_page[p] = NULL;
            else {
                unsigned address = p * PAGE_SIZE;
                unsigned start = m->linear_start, length = m->cart.size;
                if (length >= PAGE_SIZE && address >= start && address - start <= length - PAGE_SIZE)
                    m->read_page[p] = m->cart.bytes + address - start;
            }
        }
    }
}

static bool accessible_slot(const rs_msx *m, unsigned slot, unsigned sub)
{
    return slot < 4 && sub < 4 && (!sub || (m->config.expanded_slots & (1u << slot))) &&
           (slot != m->config.ram_slot || sub != m->config.ram_subslot) &&
           (!m->cart.bytes || slot != m->cart_slot);
}

bool rs_msx_mount_rom(rs_msx *m, unsigned slot, unsigned sub, unsigned page, const void *data, size_t size)
{
    if (!m || !accessible_slot(m, slot, sub) || page >= 4 || !data ||
        !size || (size % PAGE_SIZE) || size > (4 - page) * PAGE_SIZE) return false;
    image pages[4] = {0};
    unsigned count = (unsigned)(size / PAGE_SIZE);
    for (unsigned p = 0; p < count; ++p) {
        if (!replace_image(&pages[p], (const uint8_t *)data + p * PAGE_SIZE, PAGE_SIZE)) {
            for (unsigned i = 0; i < p; ++i) free(pages[i].bytes);
            return false;
        }
    }
    for (unsigned p = 0; p < count; ++p) {
        free(m->rom[slot][sub][page + p].bytes);
        m->rom[slot][sub][page + p] = pages[p];
    }
    map_pages(m);
    return true;
}

bool rs_msx_mount_disk_rom(rs_msx *m, unsigned slot, unsigned sub, const void *data, size_t size)
{
    if (size != PAGE_SIZE || !rs_msx_mount_rom(m, slot, sub, 1, data, size)) return false;
    m->disk_slot = slot;
    m->disk_subslot = sub;
    return true;
}

bool rs_msx_mount_cart(rs_msx *m, unsigned slot, rs_cart_mapper mapper, const void *data, size_t size)
{
    if (!m || !slot || slot >= 4 || (m->config.expanded_slots & (1u << slot)) ||
        slot == m->config.ram_slot || mapper < RS_CART_LINEAR || mapper > RS_CART_KONAMI_SCC ||
        size > MAX_ROM || (!data && size)) return false;
    if (size && (m->rom[slot][0][0].bytes || m->rom[slot][0][1].bytes ||
                 m->rom[slot][0][2].bytes || m->rom[slot][0][3].bytes)) return false;
    unsigned start = 0x4000;
    if (mapper == RS_CART_LINEAR && size) {
        if (size > 65536) return false;
        if (size > 32768) start = 0;
        const uint8_t *rom = data;
        if (size <= 16384 && size >= 4 && rom[0] == 'A' && rom[1] == 'B' &&
            ((rom[3] & 0xc0) == 0x80)) start = 0x8000;
    } else if (size && (size < 32768 || size % (mapper == RS_CART_ASCII16 ? 16384 : 8192))) return false;
    if (!replace_image(&m->cart, data, size)) return false;
    m->cart_slot = slot;
    m->mapper = mapper;
    m->linear_start = start;
    for (unsigned i = 0; i < 4; ++i) m->state.cart_bank[i] = mapper == RS_CART_ASCII16 ? i / 2 : i;
    memset(&m->state.scc, 0, sizeof(m->state.scc));
    map_pages(m);
    return true;
}

bool rs_msx_mount_cas(rs_msx *m, const void *data, size_t size)
{
    if (!m || size > MAX_ROM || (size && (!data || !rs_cas_marker(data, size, 0))) ||
        !replace_image(&m->cas, data, size)) return false;
    rs_msx_rewind_cas(m);
    return true;
}

bool rs_msx_mount_disk(rs_msx *m, unsigned drive, const void *data, size_t size)
{
    if (!m || drive >= 2 || (size && !data)) return false;
    rs_disk disk = {data, size, 0, 0, 0};
    if (size && !rs_disk_geometry(&disk)) return false;
    if (!replace_image(&m->disk_image[drive], data, size)) return false;
    disk.bytes = m->disk_image[drive].bytes;
    m->drives[drive] = disk;
    if ((m->state.disk.drive & 3) == drive) {
        m->state.disk.remaining = 0;
        m->state.disk.status = 0;
        m->state.disk.irq = 1;
    }
    return true;
}

void rs_msx_rewind_cas(rs_msx *m) { if (m) memset(&m->state.tape, 0, sizeof(m->state.tape)); }

bool rs_msx_reset(rs_msx *m)
{
    if (!m || !m->rom[0][0][0].bytes) return false;
    memset(m->ram, 0, m->ram_size);
    memset(m->vram, 0, VRAM_SIZE);
    memset(&m->state, 0, sizeof(m->state));
    memset(m->keys, 0, sizeof(m->keys));
    memset(m->joystick, 0, sizeof(m->joystick));
    m->state.cpu.sp = 0xf000;
    m->state.ppi_c = 0x10;
    m->state.psg.random = 0x1ffff;
    m->state.psg.reg[7] = 0xff;
    m->state.rtc_mode = 8;
    m->state.rtc[0][6] = 2;
    m->state.rtc[0][7] = m->state.rtc[0][9] = 1;
    m->state.rtc[1][10] = 1;
    m->state.disk.direction = 1;
    rs_vdp_reset(&m->state.vdp, m->config.model);
    if (m->config.pal) m->state.vdp.reg[9] = 2;
    for (unsigned i = 0; i < 4; ++i) {
        m->state.bank[i] = 3 - i;
        m->state.cart_bank[i] = m->mapper == RS_CART_ASCII16 ? i / 2 : i;
    }
    map_pages(m);
    m->ready = true;
    return true;
}

static bool scc_visible(const rs_msx *m, uint16_t addr)
{
    return m->mapper == RS_CART_KONAMI_SCC && (m->state.cart_bank[2] & 63) == 63 &&
           addr >= 0x9800 && addr < 0xa000;
}

static uint8_t cart_read(const rs_msx *m, uint16_t addr)
{
    if (m->mapper == RS_CART_LINEAR) {
        unsigned offset = (unsigned)addr - m->linear_start;
        if (offset < m->cart.size) return m->cart.bytes[offset];
        if (m->linear_start == 0x4000 && addr >= 0x8000 && addr < 0xc000 &&
            m->cart.size <= PAGE_SIZE && m->cart.size && !(m->cart.size & (m->cart.size - 1)))
            return m->cart.bytes[offset & (m->cart.size - 1)];
        return 255;
    }
    if (addr < 0x4000 || addr >= 0xc000) return 255;
    if (scc_visible(m, addr)) {
        unsigned reg = addr & 255;
        return reg < 128 ? m->state.scc.wave[reg] : 255;
    }
    unsigned segment = (addr - 0x4000) >> 13;
    unsigned bank = m->state.cart_bank[segment];
    if (m->mapper == RS_CART_ASCII16) bank = bank * 2 + (segment & 1);
    return m->cart.bytes[(bank * 8192 + (addr & 8191)) % m->cart.size];
}

static void cart_write(rs_msx *m, uint16_t addr, uint8_t data)
{
    unsigned bank = 4;
    switch (m->mapper) {
    case RS_CART_ASCII8:
        if (addr >= 0x6000 && addr < 0x8000) bank = (addr - 0x6000) >> 11;
        break;
    case RS_CART_ASCII16:
        if (addr >= 0x6000 && addr < 0x6800) m->state.cart_bank[0] = m->state.cart_bank[1] = data;
        if (addr >= 0x7000 && addr < 0x7800) m->state.cart_bank[2] = m->state.cart_bank[3] = data;
        break;
    case RS_CART_KONAMI:
        if (addr >= 0x6000 && addr < 0xc000) bank = (addr - 0x4000) >> 13;
        break;
    case RS_CART_KONAMI_SCC:
        if (scc_visible(m, addr)) {
            scc_state *s = &m->state.scc;
            unsigned reg = addr & 255;
            if (reg < 128) s->wave[reg] = data;
            else if (reg < 138) s->freq[reg - 128] = data;
            else if (reg < 143) s->volume[reg - 138] = data & 15;
            else if (reg == 143) s->enable = data & 31;
        } else if (addr >= 0x5000 && addr < 0xb800 && (addr & 0x1800) == 0x1000) bank = (addr - 0x4000) >> 13;
        break;
    default: break;
    }
    if (bank < 4) m->state.cart_bank[bank] = data;
}

static bool disk_address(const rs_msx *m, unsigned page, unsigned offset)
{
    return m->disk_slot < 4 && (page == 1 || page == 2) && offset >= 0x3ff8 &&
           m->primary_page[page] == m->disk_slot && m->secondary_page[page] == m->disk_subslot;
}

static uint8_t mem_read(void *context, uint16_t addr)
{
    rs_msx *m = context;
    unsigned page = addr >> 14, offset = addr & 0x3fff, slot = m->primary_page[page];
    if (addr == 0xffff && (m->config.expanded_slots & (1u << slot))) return ~m->state.secondary[slot];
    if (disk_address(m, page, offset)) return rs_fdc_read(&m->state.disk, m->drives, offset & 7);
    if (m->read_page[page]) return m->read_page[page][offset];
    if (m->cart.bytes && slot == m->cart_slot) return cart_read(m, addr);
    return 255;
}

static void mem_write(void *context, uint16_t addr, uint8_t data)
{
    rs_msx *m = context;
    unsigned page = addr >> 14, offset = addr & 0x3fff, slot = m->primary_page[page];
    if (addr == 0xffff && (m->config.expanded_slots & (1u << slot))) {
        m->state.secondary[slot] = data;
        map_pages(m);
    } else if (m->write_page[page]) m->write_page[page][offset] = data;
    else if (disk_address(m, page, offset)) rs_fdc_write(&m->state.disk, m->drives, offset & 7, data);
    else if (m->cart.bytes && slot == m->cart_slot) cart_write(m, addr, data);
}

static uint8_t port_read(void *context, uint16_t port)
{
    rs_msx *m = context;
    machine_state *s = &m->state;
    switch (port & 255) {
    case 0x98: case 0x99: return rs_vdp_read(&s->vdp, m->vram, m->config.model, port & 3);
    case 0xa2:
        if (s->psg.select == 14) {
            unsigned joy = (s->psg.reg[15] >> 6) & 1;
            return (0x7f & ~m->joystick[joy]) | (s->tape.level ? 128 : 0);
        }
        return s->psg.reg[s->psg.select];
    case 0xa8: return s->primary;
    case 0xa9: return ~m->keys[s->ppi_c & 15];
    case 0xaa: return s->ppi_c;
    case 0xb5:
        if (m->config.model == RS_MSX1) return 255;
        if (s->rtc_select == 13) return s->rtc_mode | 0xf0;
        if (s->rtc_select < 13) return s->rtc[s->rtc_mode & 3][s->rtc_select] | 0xf0;
        return 255;
    case 0xfc: case 0xfd: case 0xfe: case 0xff:
        if (m->config.model != RS_MSX1)
            return s->bank[port & 3] | (uint8_t)~(m->ram_size / PAGE_SIZE - 1);
        return 255;
    default: return 255;
    }
}

static void port_write(void *context, uint16_t port, uint8_t data)
{
    rs_msx *m = context;
    machine_state *s = &m->state;
    switch (port & 255) {
    case 0x98: case 0x99: case 0x9a: case 0x9b:
        rs_vdp_write(&s->vdp, m->vram, m->config.model, port & 3, data);
        break;
    case 0xa0: s->psg.select = data & 15; break;
    case 0xa1:
        s->psg.reg[s->psg.select] = data;
        if (s->psg.select == 13) {
            s->psg.env_step = 15;
            s->psg.env_attack = data & 4 ? 15 : 0;
            s->psg.env_hold = 0;
            s->psg.envelope = 0;
        }
        break;
    case 0xa8: s->primary = data; map_pages(m); break;
    case 0xaa: s->ppi_c = data; break;
    case 0xab:
        if (!(data & 128)) {
            unsigned bit = 1u << ((data >> 1) & 7);
            s->ppi_c = (s->ppi_c & ~bit) | (data & 1 ? bit : 0);
        }
        break;
    case 0xb4: s->rtc_select = data & 15; break;
    case 0xb5:
        if (m->config.model == RS_MSX1) break;
        if (s->rtc_select == 13) s->rtc_mode = data & 15;
        else if (s->rtc_select < 13) s->rtc[s->rtc_mode & 3][s->rtc_select] = data & 15;
        break;
    case 0xfc: case 0xfd: case 0xfe: case 0xff:
        if (m->config.model != RS_MSX1) {
            s->bank[port & 3] = data & (m->ram_size / PAGE_SIZE - 1);
            map_pages(m);
        }
        break;
    }
}

static int16_t sound(machine_state *s, unsigned cycles)
{
    static const int amplitude[16] = {0,39,56,79,112,159,225,318,450,637,902,1276,1805,2554,3616,5120};
    psg_state *p = &s->psg;
    unsigned period = (p->reg[6] & 31) * 32;
    if (!period) period = 32;
    p->noise += cycles;
    while (p->noise >= period) {
        p->noise -= period;
        p->random = (p->random >> 1) | (((p->random ^ (p->random >> 3)) & 1) << 16);
    }
    period = (p->reg[11] | (p->reg[12] << 8)) * 32;
    if (!period) period = 16;
    if (!p->env_hold) p->envelope += cycles;
    while (!p->env_hold && p->envelope >= period) {
        p->envelope -= period;
        if (p->env_step) --p->env_step;
        else if (!(p->reg[13] & 8)) { p->env_hold = 1; p->env_attack = 0; }
        else {
            if (p->reg[13] & 2) p->env_attack ^= 15;
            if (p->reg[13] & 1) p->env_hold = 1;
            else p->env_step = 15;
        }
    }
    int sample = 0;
    for (unsigned i = 0; i < 3; ++i) {
        period = ((p->reg[i * 2 + 1] & 15) * 256 + p->reg[i * 2]) * 16;
        if (!period) period = 16;
        p->tone[i] += cycles;
        if ((p->tone[i] / period) & 1) p->phase ^= 1u << i;
        p->tone[i] %= period;
        unsigned control = p->reg[i + 8];
        int level = amplitude[control & 16 ? (p->env_step ^ p->env_attack) & 15 : control & 15];
        bool tone = ((p->phase | p->reg[7]) >> i) & 1;
        bool noise = ((p->reg[7] >> (i + 3)) | p->random) & 1;
        sample += tone && noise ? level : -level;
    }
    for (unsigned i = 0; i < 5; ++i) {
        if (!(s->scc.enable & (1u << i))) continue;
        period = (s->scc.freq[i * 2] | ((s->scc.freq[i * 2 + 1] & 15) << 8)) + 1;
        s->scc.tick[i] = (s->scc.tick[i] + cycles) % (period * 32);
        sample += (int8_t)s->scc.wave[(i < 4 ? i : 3) * 32 + s->scc.tick[i] / period] * s->scc.volume[i];
    }
    return (int16_t)sample;
}

static unsigned get_bcd(const uint8_t *r, unsigned n) { return r[n] + r[n + 1] * 10; }
static void put_bcd(uint8_t *r, unsigned n, unsigned v) { r[n] = v % 10; r[n + 1] = v / 10; }

static void rtc_second(machine_state *s)
{
    static const unsigned days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (!(s->rtc_mode & 8)) return;
    uint8_t *r = s->rtc[0];
    unsigned second = get_bcd(r, 0) + 1, minute = get_bcd(r, 2), hour = get_bcd(r, 4);
    unsigned day = get_bcd(r, 7), month = get_bcd(r, 9), year = get_bcd(r, 11) % 100;
    if (second >= 60) { second = 0; ++minute; }
    if (minute >= 60) { minute = 0; ++hour; }
    if (hour >= 24) { hour = 0; ++day; r[6] = (r[6] + 1) % 7; }
    if (!month || month > 12) month = 1;
    if (!day || day > days[month - 1] + (month == 2 && !(year & 3))) { day = 1; ++month; }
    if (month > 12) { month = 1; year = (year + 1) % 100; }
    put_bcd(r, 0, second);
    put_bcd(r, 2, minute);
    put_bcd(r, 4, hour);
    put_bcd(r, 7, day);
    put_bcd(r, 9, month);
    put_bcd(r, 11, year);
}

void rs_msx_key_row(rs_msx *m, unsigned row, uint8_t pressed)
{
    if (m && row < 16) m->keys[row] = pressed;
}

void rs_msx_joystick(rs_msx *m, unsigned port, uint8_t pressed)
{
    if (m && port < 2) m->joystick[port] = pressed & 63;
}

size_t rs_msx_frame(rs_msx *m, uint16_t *video, int16_t *audio)
{
    if (!m || !m->ready) return 0;
    machine_state *s = &m->state;
    rs_vdp *v = &s->vdp;
    unsigned model = m->config.model, width = m->config.video_width;
    unsigned lines = model == RS_MSX1 && m->config.pal ? 313 : rs_vdp_lines(v, model);
    unsigned spent = s->line_overrun;
    unsigned count = 0, sample_cycles = 0;
    const rs_z80_bus bus = {m, mem_read, mem_write, port_read, port_write};
    v->status[2] &= ~64;
    v->status[2] ^= 2;
    for (unsigned line = 0; line < lines; ++line) {
        v->line = line;
        rs_vdp_scanline(v, m->vram, model, video && line < RS_MSX_HEIGHT ? video + line * width : NULL, width);
        while (spent < LINE_CYCLES) {
            unsigned cycles = rs_z80_step(&s->cpu, &bus, rs_vdp_irq(v));
            spent += cycles;
            if (m->cas.bytes && !(s->ppi_c & 16))
                rs_tape_step(&s->tape, m->cas.bytes, m->cas.size, cycles);
        }
        spent -= LINE_CYCLES;
        s->clock_cycles += LINE_CYCLES;
        if (s->clock_cycles >= CPU_HZ) {
            s->clock_cycles -= CPU_HZ;
            rtc_second(s);
        }
        unsigned remaining = LINE_CYCLES;
        while (remaining) {
            unsigned next_sample = (CPU_HZ - s->audio_clock + RS_MSX_AUDIO_RATE - 1) / RS_MSX_AUDIO_RATE;
            unsigned advance = next_sample < remaining ? next_sample : remaining;
            s->audio_clock += advance * RS_MSX_AUDIO_RATE;
            sample_cycles += advance;
            remaining -= advance;
            if (s->audio_clock >= CPU_HZ) {
                s->audio_clock -= CPU_HZ;
                int16_t sample = sound(s, sample_cycles);
                if (audio) audio[count] = sample;
                ++count;
                sample_cycles = 0;
            }
        }
    }
    if (sample_cycles) (void)sound(s, sample_cycles);
    s->line_overrun = spent;
    s->frame_cycles_div4 = lines * (LINE_CYCLES / 4);
    return count;
}

unsigned rs_msx_frame_cycles(const rs_msx *m) { return m ? m->state.frame_cycles_div4 * 4u : 0; }
uint16_t rs_msx_pc(const rs_msx *m) { return m ? m->state.cpu.pc : 0; }

static void put_u32(uint8_t *out, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i) out[i] = n >> (i * 8);
}

static uint32_t get_u32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t image_crc(uint32_t n, const image *img)
{
    uint8_t bytes[8];
    put_u32(bytes, img->size);
    put_u32(bytes + 4, img->checksum);
    return crc(n, bytes, sizeof(bytes));
}

static uint32_t machine_id(const rs_msx *m)
{
    const uint8_t profile[] = {m->config.model, m->config.expanded_slots, m->config.ram_slot,
        m->config.ram_subslot, m->config.pal, m->cart_slot, m->mapper, m->disk_slot, m->disk_subslot};
    uint32_t n = crc(0, profile, sizeof(profile));
    for (unsigned s = 0; s < 4; ++s)
        for (unsigned u = 0; u < 4; ++u)
            for (unsigned p = 0; p < 4; ++p) n = image_crc(n, &m->rom[s][u][p]);
    n = image_crc(n, &m->cart);
    n = image_crc(n, &m->cas);
    n = image_crc(n, &m->disk_image[0]);
    return image_crc(n, &m->disk_image[1]);
}

size_t rs_msx_state_size(const rs_msx *m)
{
    return m ? STATE_HEADER + sizeof(machine_state) + m->ram_size + VRAM_SIZE : 0;
}

bool rs_msx_save(const rs_msx *m, void *buffer, size_t size)
{
    if (!m || !m->ready || !buffer || size != rs_msx_state_size(m)) return false;
    uint8_t *p = buffer;
    memcpy(p, "RS3MSX\0\1", 8);
    put_u32(p + 8, (uint32_t)size);
    put_u32(p + 12, machine_id(m));
    memcpy(p + STATE_HEADER, &m->state, sizeof(m->state));
    memcpy(p + STATE_HEADER + sizeof(m->state), m->ram, m->ram_size);
    memcpy(p + STATE_HEADER + sizeof(m->state) + m->ram_size, m->vram, VRAM_SIZE);
    put_u32(p + 16, crc(0, p + STATE_HEADER, size - STATE_HEADER));
    return true;
}

bool rs_msx_load(rs_msx *m, const void *buffer, size_t size)
{
    if (!m || !m->ready || !buffer || size != rs_msx_state_size(m)) return false;
    const uint8_t *p = buffer;
    if (memcmp(p, "RS3MSX\0\1", 8) || get_u32(p + 8) != size || get_u32(p + 12) != machine_id(m) ||
        get_u32(p + 16) != crc(0, p + STATE_HEADER, size - STATE_HEADER)) return false;
    machine_state state;
    memcpy(&state, p + STATE_HEADER, sizeof(state));
    if (state.cpu.im > 2 || state.psg.select > 15 || state.tape.offset > m->cas.size ||
        state.tape.bit > 10 || state.tape.repeats > 3 || state.tape.elapsed > 1491 ||
        state.tape.pulses > 48000 || state.audio_clock >= CPU_HZ || state.clock_cycles >= CPU_HZ ||
        state.line_overrun >= 64 || state.psg.noise >= 1024 || state.psg.envelope > 0x200000 ||
        state.psg.tone[0] >= 65536 || state.psg.tone[1] >= 65536 || state.psg.tone[2] >= 65536)
        return false;
    unsigned drive = state.disk.drive & 3;
    if (state.disk.remaining && (drive > 1 || state.disk.offset > m->drives[drive].size ||
        state.disk.remaining > m->drives[drive].size - state.disk.offset)) return false;
    memcpy(m->ram, p + STATE_HEADER + sizeof(state), m->ram_size);
    memcpy(m->vram, p + STATE_HEADER + sizeof(state) + m->ram_size, VRAM_SIZE);
    m->state = state;
    map_pages(m);
    return true;
}
