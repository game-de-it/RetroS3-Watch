#include "rom_filename.h"
#include <stdint.h>
#include <string.h>
#include <strings.h>

bool rom_filename_valid(const char *name)
{
    const size_t length = strlen(name);
    if (!length || length > 128 || name[0] == '.' || name[length-1] == ' ' || name[length-1] == '.') return false;
    const unsigned char *s = (const unsigned char *)name;
    for (size_t i = 0; i < length;) {
        uint32_t c = s[i++];
        if (c < 128) {
            if (c < 32 || c == 127 || strchr("<>:\"/\\|?*", (int)c)) return false;
        } else {
            unsigned continuation;
            uint32_t minimum;
            if (c >= 0xc2 && c <= 0xdf) { c &= 31; continuation = 1; minimum = 0x80; }
            else if (c >= 0xe0 && c <= 0xef) { c &= 15; continuation = 2; minimum = 0x800; }
            else if (c >= 0xf0 && c <= 0xf4) { c &= 7; continuation = 3; minimum = 0x10000; }
            else return false;
            if (continuation > length - i) return false;
            while (continuation--) {
                if ((s[i] & 0xc0) != 0x80) return false;
                c = (c << 6) | (s[i++] & 63);
            }
            if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
                (c >= 0x80 && c <= 0x9f)) return false;
        }
    }
    const char *extension = strrchr(name, '.');
    return extension &&
           (!strcasecmp(extension, ".rom") || !strcasecmp(extension, ".dsk") ||
            !strcasecmp(extension, ".cas"));
}
static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
bool rom_filename_decode_hex(const char *hex, char *name, size_t capacity)
{
    const size_t length = strlen(hex);
    if ((length & 1) || length/2 >= capacity) return false;
    for (size_t i = 0; i < length/2; ++i) {
        int hi=nibble(hex[i*2]), lo=nibble(hex[i*2+1]);
        if (hi < 0 || lo < 0 || !(hi|lo)) return false;
        name[i]=(char)((hi<<4)|lo);
    }
    name[length/2]=0;
    return rom_filename_valid(name);
}

void rom_filename_display(const char *name, char *output, size_t capacity)
{
    static const uint16_t pairs[][3] = {
#include "kana_composition.inc"
    };
    size_t used=0;
    const unsigned char *p=(const unsigned char *)name;
    if (!capacity) return;
    while (*p) {
        unsigned bytes=*p<0x80?1:(*p&0xe0)==0xc0?2:(*p&0xf0)==0xe0?3:4;
        unsigned char composed[3];
        unsigned consumed=bytes;
        const unsigned char *data=p;
        bool valid=true;
        for (unsigned i=1;i<bytes;++i) if (!p[i] || (p[i]&0xc0)!=0x80) {valid=false;break;}
        if (!valid) break;
        if (bytes==3 && p[3] && p[4] && p[5] && (p[3]&0xf0)==0xe0 &&
            (p[4]&0xc0)==0x80 && (p[5]&0xc0)==0x80) {
            const uint16_t first=((p[0]&15)<<12)|((p[1]&63)<<6)|(p[2]&63);
            const uint16_t second=((p[3]&15)<<12)|((p[4]&63)<<6)|(p[5]&63);
            for (size_t i=0;i<sizeof(pairs)/sizeof(pairs[0]);++i) {
                if (pairs[i][0]==first && pairs[i][1]==second) {
                    uint16_t value=pairs[i][2];
                    composed[0]=0xe0|(value>>12);composed[1]=0x80|((value>>6)&63);composed[2]=0x80|(value&63);
                    data=composed;consumed=6;break;
                }
            }
        }
        if (used+bytes>=capacity) break;
        memmove(output+used,data,bytes);
        if (bytes==1 && output[used]>='a' && output[used]<='z') output[used]-='a'-'A';
        used+=bytes;p+=consumed;
    }
    output[used]=0;
}
