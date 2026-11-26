"""Applying dirty regions must exactly reproduce the accepted full transform."""
import ctypes,pathlib,random,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Region(ctypes.Structure):_fields_=[('x',ctypes.c_int),('y',ctypes.c_int),('width',ctypes.c_int),('height',ctypes.c_int)]
class DiffTest(unittest.TestCase):
 def test_exact_patch_and_fallback(self):
  with tempfile.TemporaryDirectory() as directory:
   lib=pathlib.Path(directory)/'diff.dylib'
   subprocess.run(['cc','-O2','-shared','-fPIC',str(ROOT/'main/msx_video_diff.c'),str(ROOT/'main/msx_video_scale.c'),'-o',str(lib)],check=True)
   dll=ctypes.CDLL(str(lib));ptr=ctypes.POINTER(ctypes.c_uint16)
   dll.msx_video_diff.argtypes=[ptr,ptr,ctypes.POINTER(Region),ctypes.c_int]
   dll.msx_video_scale_rotate.argtypes=[ptr,ptr]
   dll.msx_video_scale_region_rows.argtypes=[ptr,ptr,Region,ctypes.c_int,ctypes.c_int]
   rng=random.Random(4);source=(ctypes.c_uint16*(256*228))(*[rng.randrange(65536) for _ in range(256*228)])
   regions=(Region*24)();old=(ctypes.c_uint16*(342*384))();full=(ctypes.c_uint16*(342*384))()
   self.assertEqual(dll.msx_video_diff(source,source,regions,24),0)
   self.assertEqual(dll.msx_video_diff(source,None,regions,24),-1)
   for case in range(12):
    current=(ctypes.c_uint16*(256*228)).from_buffer_copy(source)
    for _ in range(8):
     x=rng.randrange(256);y=rng.randrange(228);w=rng.randrange(1,17);h=rng.randrange(1,17)
     for yy in range(y,min(y+h,228)):
      for xx in range(x,min(x+w,256)):current[yy*256+xx]^=0xffff
    # Include both extreme corners / the 4-row bottom source tile.
    current[0]^=1;current[-1]^=1
    count=dll.msx_video_diff(current,source,regions,24);self.assertGreater(count,0)
    dll.msx_video_scale_rotate(source,old);dll.msx_video_scale_rotate(current,full)
    for r in regions[:count]:
     self.assertEqual((r.x|r.y|r.width|r.height)&1,0)
     for first in range(0,r.height,20):
      rows=min(20,r.height-first);data=(ctypes.c_uint16*(r.width*rows+2))();data[0]=0x1234;data[-1]=0xabcd
      dll.msx_video_scale_region_rows(current,ctypes.cast(ctypes.byref(data,2),ptr),r,first,rows)
      for y in range(rows):
       for x in range(r.width):old[(r.y+first+y)*342+r.x+x]=data[1+y*r.width+x]
      self.assertEqual((data[0],data[-1]),(0x1234,0xabcd))
    self.assertEqual(bytes(old),bytes(full))
    for r in regions[:count]:
     data=(ctypes.c_uint16*(r.width*r.height))()
     dll.msx_video_scale_region_rows(current,data,r,0,r.height)
     expected=[full[(r.y+y)*342+r.x+x] for y in range(r.height) for x in range(r.width)]
     self.assertEqual(list(data),expected)
    self.assertEqual(dll.msx_video_diff(current,source,regions,1),-1)
   changed=(ctypes.c_uint16*(256*228))()
   self.assertEqual(dll.msx_video_diff(changed,source,regions,24),-1)
if __name__=='__main__':unittest.main()
