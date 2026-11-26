"""Every modified benchmark pixel must lie inside an aligned transmitted region."""
import ctypes,pathlib,random,re,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Region(ctypes.Structure):_fields_=[('x',ctypes.c_int),('y',ctypes.c_int),('width',ctypes.c_int),('height',ctypes.c_int)]
class BenchTest(unittest.TestCase):
 def test_dirty_coverage_and_bounds(self):
  with tempfile.TemporaryDirectory() as directory:
   d=pathlib.Path(directory)
   (d/'lvgl.h').write_text('''#pragma once
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#define LV_FONT_GLYPH_FORMAT_A1 1
typedef struct lv_font lv_font_t;
typedef struct {const lv_font_t *resolved_font;int format,box_w,box_h,ofs_x,ofs_y,index;bool req_raw_bitmap;} lv_font_glyph_dsc_t;
struct lv_font {int line_height,base_line;const void *(*get_glyph_bitmap)(lv_font_glyph_dsc_t*,void*);};
typedef void lv_obj_t;
bool lv_font_get_glyph_dsc(const lv_font_t*,lv_font_glyph_dsc_t*,uint32_t,uint32_t);
void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t*);
''')
   (d/'esp_err.h').write_text('typedef int esp_err_t;\n')
   font_source=(ROOT/'main/fonts/msx_ascii_32.c').read_text()
   raw=re.search(r'const uint8_t glyph_bitmap\[\] = \{(.*?)\};',font_source,re.S)[1]
   raw=re.sub(r'/\*.*?\*/','',raw,flags=re.S)
   data=[int(v,16) for v in re.findall(r'0x([0-9a-fA-F]+)',raw)]
   glyphs=re.findall(r'\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), \.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}',font_source)
   tables='static const unsigned char bits[]={'+','.join(map(str,data))+'};\n'
   tables+='static const int glyphs[][6]={'+','.join('{'+','.join(g)+'}' for g in glyphs)+'};\n'
   (d/'stubs.c').write_text('#include "lvgl.h"\n'+tables+'''
static const void *bitmap(lv_font_glyph_dsc_t*d,void*b){(void)b;return d->req_raw_bitmap? bits+glyphs[d->index][0]:0;}
static const lv_font_t font={36,4,bitmap};
const lv_font_t *watch_font(void){return &font;}
bool lv_font_get_glyph_dsc(const lv_font_t*f,lv_font_glyph_dsc_t*d,uint32_t c,uint32_t n){
 (void)n;if(c<32||c>126)return false;int id=(int)c-31;const int*g=glyphs[id];
 *d=(lv_font_glyph_dsc_t){.resolved_font=f,.format=1,.box_w=g[2],.box_h=g[3],.ofs_x=g[4],.ofs_y=g[5],.index=id};return true;
}
void lv_font_glyph_release_draw_data(lv_font_glyph_dsc_t*d){(void)d;}
''')
   lib=d/'bench.dylib';subprocess.run(['cc','-shared','-fPIC','-O2','-I'+str(d),str(ROOT/'main/display_bench.c'),str(d/'stubs.c'),'-o',str(lib)],check=True)
   dll=ctypes.CDLL(str(lib));ptr=ctypes.POINTER(ctypes.c_uint16)
   dll.display_bench_reset.argtypes=[ptr,ctypes.c_int]
   dll.display_bench_frame.argtypes=[ptr,ctypes.c_uint64,ctypes.c_bool,ctypes.c_uint,ctypes.POINTER(Region)]
   pixels=(ctypes.c_uint16*(342*384+2))();pixels[0]=0x1234;pixels[-1]=0xabcd
   image=ctypes.cast(ctypes.byref(pixels,2),ptr);regions=(Region*4)()
   rng=random.Random(0)
   for mode in range(4):
    dll.display_bench_reset(image,mode)
    # The prior static-bitmap API yielded NULL for these converted fonts.
    # Exercise the real 1bpp tables and require visible title + mode pixels.
    self.assertGreater(sum(image[x*342+341-y]==0xffe0 for x in range(384) for y in range(36)),200)
    self.assertGreater(sum(image[x*342+341-y]==0x07ff for x in range(384) for y in range(266,302)),200)
    times=[0,16666,1000000,1999999,2000000]+[rng.randrange(10000000) for _ in range(25)]
    for t in times:
     before=list(pixels)[1:-1];n=dll.display_bench_frame(image,t,t%3==0,600,regions)
     covered=bytearray(342*384)
     for r in regions[:n]:
      self.assertEqual((r.x|r.y|r.width|r.height)&1,0)
      self.assertTrue(0<=r.x<342 and 0<=r.y<384)
      self.assertTrue(0<r.width<=342-r.x and 0<r.height<=384-r.y)
      for y in range(r.y,r.y+r.height):covered[y*342+r.x:y*342+r.x+r.width]=b'\1'*r.width
     for i,(a,b) in enumerate(zip(before,list(pixels)[1:-1])):
      if a!=b:self.assertTrue(covered[i],(mode,t,i))
     self.assertEqual((pixels[0],pixels[-1]),(0x1234,0xabcd))
if __name__=='__main__':unittest.main()
