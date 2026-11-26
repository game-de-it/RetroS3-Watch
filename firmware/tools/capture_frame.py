#!/usr/bin/env python3
"""Read checksum-verified emulator-source snapshots, without writing SD slots."""
import argparse, binascii, pathlib, re, serial, struct, time
p=argparse.ArgumentParser();p.add_argument('--port',required=True,help='Serial port, for example /dev/cu.usbmodemXXXX or COM3');p.add_argument('--count',type=int,default=1);p.add_argument('--output',type=pathlib.Path,required=True);p.add_argument('--restore-slot',type=int);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
s=serial.Serial(a.port,115200,timeout=.1)
if a.restore_slot:
 s.write(f'state-load {a.restore_slot}\n'.encode());deadline=time.monotonic()+5;received=b''
 while time.monotonic()<deadline:
  received+=s.read(4096)
  if f'state load slot {a.restore_slot}: OK'.encode() in received:break
 else:raise RuntimeError('state restore not confirmed: '+received.decode(errors='replace')[-500:])
 print(next((l for l in received.decode(errors='replace').splitlines() if 'state load slot' in l),'State restored'),flush=True)
 time.sleep(.2)
for index in range(a.count):
 s.write(b'capture-frame\n');rows={};expected=None;width=height=0;deadline=time.monotonic()+20
 while time.monotonic()<deadline:
  line=s.readline().decode('ascii',errors='replace').strip()
  begin=re.fullmatch(r'CAPTURE BEGIN (\d+) (\d+) ([0-9a-f]{8})',line)
  row=re.fullmatch(r'CAPTURE ROW (\d+) ([0-9a-f]+)',line)
  end=re.fullmatch(r'CAPTURE END ([0-9a-f]{8})',line)
  if begin:
   width,height=int(begin[1]),int(begin[2]);expected=int(begin[3],16)
   assert (width,height) in ((256,228),(342,384)),(width,height)
  elif row and expected is not None:
   assert len(row[2])==width*4
   rows[int(row[1])]=[int(row[2][i:i+4],16) for i in range(0,width*4,4)]
  elif end and expected is not None:
   assert int(end[1],16)==expected
   break
  elif line.startswith('CAPTURE ERROR'):raise RuntimeError(line)
 else:raise RuntimeError('capture timeout')
 assert set(rows)==set(range(height)),f'missing rows: {len(rows)}'
 values=[v for y in range(height) for v in rows[y]];raw=struct.pack('<%dH'%len(values),*values)
 actual=binascii.crc32(raw)&0xffffffff
 # ESP ROM uses the same reflected CRC convention as zlib.
 assert actual==expected,f'CRC expected={expected:08x} actual={actual:08x}'
 stem=a.output/f'frame-{index:03d}'
 stem.with_suffix('.rgb565').write_bytes(raw)
 rgb=bytearray()
 for v in values:
  r=(v>>11)&31;g=(v>>5)&63;b=v&31
  rgb+=bytes(((r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)))
 stem.with_suffix('.ppm').write_bytes(f'P6\n{width} {height}\n255\n'.encode()+rgb)
 print(f'{stem}: CRC {actual:08x}',flush=True)
s.close()
