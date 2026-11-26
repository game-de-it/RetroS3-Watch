#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct { int x,y,width,height; } bench_region_t;
/* Native 342x384 portrait storage, presented as the MSX 384x342 landscape view. */
void display_bench_reset(uint16_t *pixels, int mode);
int display_bench_frame(uint16_t *pixels, uint64_t elapsed_us, bool status,
                        unsigned fps10, bench_region_t regions[4]);
