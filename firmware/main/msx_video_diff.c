#include "msx_video_diff.h"
#include <string.h>

int msx_video_diff(const uint16_t *current,const uint16_t *previous,
                   msx_diff_region_t *regions,int capacity)
{
    if(!current||!previous||!regions||capacity<1) return -1;
    uint16_t dirty[15]={0};
    int changed=0;
    /* Walk contiguous scanlines. Tile-major 32-byte comparisons repeatedly
     * evicted PSRAM cache lines from both frames and cost several milliseconds. */
    for(int y=0;y<228;++y) {
        int ty=y/16;
        for(int tx=0;tx<16;++tx) {
            uint16_t bit=(uint16_t)(1U<<tx);
            if(dirty[ty]&bit) continue;
            int offset=y*256+tx*16;
            if(memcmp(current+offset,previous+offset,32)) {
                dirty[ty]|=bit;
                if(++changed>=96) return -1;
            }
        }
    }
    int used=0;
    for(int ty=0;ty<15;++ty) for(int tx=0;tx<16;++tx) {
        if(!(dirty[ty]&(1U<<tx))) continue;
        int ex=tx+1;
        while(ex<16&&(dirty[ty]&(1U<<ex))) ++ex;
        uint16_t bits=(uint16_t)(((1U<<ex)-1)^((1U<<tx)-1));
        int ey=ty+1;
        while(ey<15&&(dirty[ey]&bits)==bits) ++ey;
        if(used>=capacity) return -1;
        int sx=tx*16,sy=ty*16,endx=ex*16,endy=ey*16<228?ey*16:228;
        regions[used++]=(msx_diff_region_t){(228-endy)*3/2,sx*3/2,(endy-sy)*3/2,(endx-sx)*3/2};
        for(int y=ty;y<ey;++y) dirty[y]&=(uint16_t)~bits;
    }
    return used;
}
void msx_video_scale_region_rows(const uint16_t *source,uint16_t *output,
    msx_diff_region_t region,int first,int count)
{
    if(!source||!output||region.x<0||region.y<0||region.width<1||region.height<1||
       region.x+region.width>342||region.y+region.height>384||first<0||count<0||first+count>region.height) return;
    /* Tile boundaries are 24 pixels (6 for the bottom source tile), hence
     * divisible by three. Reuse the accepted full-stream 3:2 pattern and
     * cache-friendly 12-pixel blocks instead of dividing/loading per pixel. */
    if(region.x%3==0 && region.width%3==0) {
        for(int tile=0;tile<region.width;tile+=12) {
            int end=tile+12<region.width?tile+12:region.width;
            for(int row=0;row<count;++row) {
                int sx=(region.y+first+row)*2/3;
                const uint16_t *src=source+(227-(region.x+tile)/3*2)*256+sx;
                uint16_t *dst=output+row*region.width+tile;
                for(int column=tile;column<end;column+=3) {
                    uint16_t a=src[0],b=src[-256];
                    dst[0]=a;dst[1]=b;dst[2]=b;dst+=3;
                    if(column+3<end) src-=512;
                }
            }
        }
    } else for(int row=0;row<count;++row) {
        int source_x=(region.y+first+row)*2/3;
        for(int column=0;column<region.width;++column) {
            int source_y=(341-region.x-column)*2/3;
            output[row*region.width+column]=source[source_y*256+source_x];
        }
    }
}
