#include "display_bench.h"
#include "watch_theme.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define W 384
#define H 342
typedef struct {int x,y,w,h;} box_t;
static box_t previous[3];
static const uint16_t colors[]={0xf800,0x07e0,0xffff};
static void pixel(uint16_t *p,int x,int y,uint16_t color)
{ if(x>=0&&x<W&&y>=0&&y<H) p[x*H+H-1-y]=color; }
static uint16_t background(int x,int y)
{ return y>=80&&y<260&&((x%32)==0||(y%32)==0)? 0x2104:0; }
static void box(uint16_t *p,box_t b,uint16_t color,bool erase)
{
    for(int y=b.y;y<b.y+b.h;++y) for(int x=b.x;x<b.x+b.w;++x)
        pixel(p,x,y,erase? background(x,y):color);
}
static void text(uint16_t *p,const char *value,int y,uint16_t color)
{
    const lv_font_t *font=watch_font();
    int x=(W-(int)strlen(value)*32)/2;
    for(;*value;++value,x+=32) {
        lv_font_glyph_dsc_t d={0};
        if(!lv_font_get_glyph_dsc(font,&d,(unsigned char)*value,0)) continue;
        /* lv_font_conv 1.x fonts lack LVGL's newer static_bitmap flag.
         * Request the raw compiled bitmap through the font callback; the
         * generic bitmap helper would instead expand it to an A8 draw buffer. */
        d.req_raw_bitmap=1;
        const uint8_t *bitmap=d.resolved_font->get_glyph_bitmap(&d,NULL);
        if(bitmap&&d.format==LV_FONT_GLYPH_FORMAT_A1) {
            for(int gy=0;gy<d.box_h;++gy) for(int gx=0;gx<d.box_w;++gx) {
                int bit=gy*d.box_w+gx;
                if(bitmap[bit/8]&(0x80>>(bit%8)))
                    pixel(p,x+d.ofs_x+gx,y+font->line_height-font->base_line-d.box_h-d.ofs_y+gy,color);
            }
        }
        lv_font_glyph_release_draw_data(&d);
    }
}
static bench_region_t region(box_t b)
{
    int x=(H-b.y-b.h)&~1, y=b.x&~1;
    int endx=(H-b.y+1)&~1, endy=(b.x+b.w+1)&~1;
    return (bench_region_t){x,y,endx-x,endy-y};
}
void display_bench_reset(uint16_t *pixels,int mode)
{
    for(int x=0;x<W;++x) for(int y=0;y<H;++y) pixel(pixels,x,y,background(x,y));
    memset(previous,0,sizeof(previous));
    text(pixels,mode==3?"TARGET TE":"TARGET 60",0,0xffe0);
    text(pixels,"ACT --.-",38,0xffff);
    text(pixels,mode==0? "1 FULL":mode==1? "2 SMALL":mode==2? "3 LVGL":"4 SYNC",266,0x07ff);
    text(pixels,"TAP: NEXT",304,0xffff);
}
int display_bench_frame(uint16_t *pixels,uint64_t elapsed_us,bool status,
                        unsigned fps10,bench_region_t regions[4])
{
    box_t next[3]={
        {(int)((elapsed_us*180/1000000)%352),92,32,32},
        {144,150+(int)((elapsed_us*100/1000000)%72),32,32},
        {(int)((elapsed_us*240/1000000)%376),174,8,64}
    };
    for(int i=0;i<3;++i) box(pixels,previous[i],0,true);
    for(int i=0;i<3;++i) {
        box(pixels,next[i],colors[i],false);
        int x0=previous[i].w? (previous[i].x<next[i].x?previous[i].x:next[i].x):next[i].x;
        int y0=previous[i].h? (previous[i].y<next[i].y?previous[i].y:next[i].y):next[i].y;
        int x1=previous[i].x+previous[i].w>next[i].x+next[i].w?previous[i].x+previous[i].w:next[i].x+next[i].w;
        int y1=previous[i].y+previous[i].h>next[i].y+next[i].h?previous[i].y+previous[i].h:next[i].y+next[i].h;
        regions[i]=region((box_t){x0,y0,x1-x0,y1-y0});previous[i]=next[i];
    }
    if(status) {
        box(pixels,(box_t){0,38,W,38},0,false);
        char value[24];snprintf(value,sizeof(value),"ACT %u.%u",fps10/10,fps10%10);
        text(pixels,value,38,0xffff);
        regions[3]=region((box_t){0,38,W,38});
    }
    return status?4:3;
}
