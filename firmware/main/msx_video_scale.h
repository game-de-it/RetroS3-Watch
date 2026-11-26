#pragma once
#include <stdint.h>
/* Exact existing nearest-neighbour 256x228 -> rotated 342x384 transform.
 * Buffers must not overlap; all pixels including overscan are retained. */
void msx_video_scale_rotate(const uint16_t *source, uint16_t *portrait);

void msx_video_scale_rotate_rows(const uint16_t *source, uint16_t *rows, int first, int count);

/* Wire-order RGB565, for a single PSRAM-DMA LCD transaction. */
void msx_video_scale_rotate_be(const uint16_t *source, uint16_t *portrait);
