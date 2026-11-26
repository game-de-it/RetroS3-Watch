#pragma once
#include <stdint.h>
typedef struct {int x,y,width,height;} msx_diff_region_t;
/* Returns -1 for a full refresh, 0 for identical frames, or a list of even,
 * portrait-coordinate regions. Previous must be the last presented frame. */
int msx_video_diff(const uint16_t *current,const uint16_t *previous,
                   msx_diff_region_t *regions,int capacity);
void msx_video_scale_region_rows(const uint16_t *source,uint16_t *output,
    msx_diff_region_t region,int first,int count);
