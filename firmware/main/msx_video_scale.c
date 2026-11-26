#include "msx_video_scale.h"

static void scale_rotate(const uint16_t *source, uint16_t *portrait, int swap)
{
    /* Four source pixels expand to a 3x3 block. Read each source pixel once
     * and repeat stores instead of calculating and loading all nine pixels.
     * Small tiles avoid cache-set conflicts between PSRAM source/destination.
     * Reversed source rows preserve the established counter-clockwise view. */
    for (int tx = 0; tx < 256; tx += 8) {
        for (int ty = 0; ty < 228; ty += 8) {
            int end_y = ty + 8 < 228 ? ty + 8 : 228;
            for (int x = tx; x < tx + 8; x += 2) {
                uint16_t *row = portrait + (x / 2) * 3 * 342 + (ty / 2) * 3;
                const uint16_t *src = source + (227 - ty) * 256 + x;
                for (int y = ty; y < end_y; y += 2) {
                    uint16_t a = src[0], b = src[-256];
                    uint16_t c = src[1], d = src[-255];
                    if (swap) {
                        a=__builtin_bswap16(a); b=__builtin_bswap16(b);
                        c=__builtin_bswap16(c); d=__builtin_bswap16(d);
                    }
                    row[0] = a; row[1] = b; row[2] = b;
                    row[342] = a; row[343] = b; row[344] = b;
                    row[684] = c; row[685] = d; row[686] = d;
                    if (y + 2 < end_y) src -= 512;
                    row += 3;
                }
            }
        }
    }
}

/* Stream just the requested portrait rows into internal DMA RAM. The 12-pixel
 * tiles keep strided source reads local; no expanded PSRAM image is needed. */
void msx_video_scale_rotate_rows(const uint16_t *source, uint16_t *rows, int first, int count)
{
    if (first < 0 || count < 0 || first + count > 384) return;
    for (int tile = 0; tile < 342; tile += 12) {
        int end = tile + 12 < 342 ? tile + 12 : 342;
        for (int row = 0; row < count; ++row) {
            int source_x = ((first + row) * 2) / 3;
            const uint16_t *src = source + (227 - (tile / 3) * 2) * 256 + source_x;
            uint16_t *dst = rows + row * 342 + tile;
            for (int column = tile; column < end; column += 3) {
                uint16_t a = src[0], b = src[-256];
                dst[0] = a; dst[1] = b; dst[2] = b;
                dst += 3;
                if (column + 3 < end) src -= 512;
            }
        }
    }
}

void msx_video_scale_rotate(const uint16_t *source, uint16_t *portrait)
{ scale_rotate(source,portrait,0); }
void msx_video_scale_rotate_be(const uint16_t *source, uint16_t *portrait)
{ scale_rotate(source,portrait,1); }
