"""Prove the optimized transform is pixel-identical to the accepted scaler."""
import ctypes, pathlib, random, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ScaleTest(unittest.TestCase):
    def test_every_pixel_and_guard(self):
        with tempfile.TemporaryDirectory() as directory:
            lib=pathlib.Path(directory)/'scale.dylib'
            subprocess.run(['cc','-std=c11','-O2','-shared','-fPIC',str(ROOT/'main/msx_video_scale.c'),'-o',str(lib)],check=True)
            fn=ctypes.CDLL(str(lib)).msx_video_scale_rotate
            ptr=ctypes.POINTER(ctypes.c_uint16);fn.argtypes=[ptr,ptr]
            rng=random.Random(7)
            for pattern in range(3):
                source=(ctypes.c_uint16*(256*228))(*[rng.randrange(65536) if pattern==0 else (i if pattern==1 else ~i)&65535 for i in range(256*228)])
                saved=bytes(source)
                output=(ctypes.c_uint16*(342*384+2))();output[0]=0x1234;output[-1]=0xabcd
                fn(source,ctypes.cast(ctypes.byref(output,2),ptr))
                expected=[source[((341-x)*2//3)*256+y*2//3] for y in range(384) for x in range(342)]
                self.assertEqual(list(output)[1:-1],expected)
                self.assertEqual((output[0],output[-1]),(0x1234,0xabcd))
                self.assertEqual(bytes(source),saved)
                be=ctypes.CDLL(str(lib)).msx_video_scale_rotate_be;be.argtypes=[ptr,ptr]
                be(source,ctypes.cast(ctypes.byref(output,2),ptr))
                self.assertEqual(list(output)[1:-1],[((v&255)<<8)|(v>>8) for v in expected])
                self.assertEqual((output[0],output[-1]),(0x1234,0xabcd))
                stream=ctypes.CDLL(str(lib)).msx_video_scale_rotate_rows
                stream.argtypes=[ptr,ptr,ctypes.c_int,ctypes.c_int]
                for chunk in (2, 17, 20, 384):
                    for first in range(0,384,chunk):
                        count=min(chunk,384-first)
                        strip=(ctypes.c_uint16*(342*count+2))();strip[0]=0x789a;strip[-1]=0xbcde
                        stream(source,ctypes.cast(ctypes.byref(strip,2),ptr),first,count)
                        self.assertEqual(list(strip)[1:-1],expected[first*342:(first+count)*342])
                        self.assertEqual((strip[0],strip[-1]),(0x789a,0xbcde))
if __name__=='__main__':unittest.main()
