#!/usr/bin/env python3
"""Real LX7 ELF: scaled RGB565 rows, keys, SD sectors and PCM through gm0."""
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
root=Path(__file__).resolve().parents[2]
gcc=Path(os.environ.get('AKA_GCC',Path.home()/'.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gcc'))
if not gcc.exists():
    print('SKIP: ESP32-S3 GCC missing');raise SystemExit(0)
env=dict(os.environ,SDL_VIDEODRIVER='dummy',SDL_AUDIODRIVER='dummy',EMU_NOPACE='1')
def run(*args):
    result=subprocess.run([str(a) for a in args],capture_output=True,text=True,env=env,timeout=60)
    assert result.returncode==0,result.stdout+result.stderr
    assert 'AKA:' not in result.stderr,result.stderr
    return result
with tempfile.TemporaryDirectory(prefix='aka-machine-') as tmp:
    tmp=Path(tmp);elf=tmp/'test.elf';card=tmp/'card.img';out=tmp/'written.img';shot=tmp/'screen.ppm';wav=tmp/'audio.wav'
    card.write_bytes(bytes(128*512))
    run(gcc,'-O2','-nostdlib','-mtext-section-literals','-T',root/'tests/aka/link.ld',root/'tests/aka/firmware.c',root/'tests/aka/stubs.c','-o',elf)
    run(sys.argv[1],elf,card,'--frames',6,'--shot',shot,'--out-img',out,'--wav',wav)
    pixels=shot.read_bytes().split(b'\n255\n')[1]
    assert len(pixels)==320*240*3
    def px(x,y):return pixels[(y*320+x)*3:(y*320+x+1)*3]
    def rgb565(v):return bytes((((v>>11)&31)<<3,((v>>5)&63)<<2,(v&31)<<3))
    assert px(0,0)==rgb565(0xfffd),px(0,0)
    assert px(2,0)==rgb565(1650),px(2,0)
    assert px(4,0)==rgb565(1650),px(4,0)
    assert px(6,0)==rgb565(3900),px(6,0)
    assert px(8,0)==rgb565(85),px(8,0)
    assert px(12,0)==rgb565(1650),px(12,0)
    assert px(10,10)==bytes((248,0,0)),px(10,10)
    assert px(100,10)==bytes((0,252,0)),px(100,10)
    assert px(100,90)==bytes((248,252,248)),px(100,90)
    assert px(100,200)==bytes((0,0,248)),px(100,200)
    assert out.read_bytes()[5*512:5*512+3]==b'AKA'
    assert len(set(wav.read_bytes()[44:]))>3,'PCM must reach SDL/WAV'
    env['EMU_INPUT']='1:R:5'
    run(sys.argv[1],elf,card,'--frames',6,'--shot',shot)
    pixels=shot.read_bytes().split(b'\n255\n')[1]
    assert px(10,10)==bytes((0,0,248)),px(10,10)
    assert px(0,0)==rgb565(0xfefd),px(0,0)
    assert px(2,0)==rgb565(3299),px(2,0)
    assert px(4,0)==rgb565(1650),px(4,0)
    assert px(12,0)==rgb565(3299),px(12,0)
    for key,raw in [('U',0xfdfd),('D',0xfbfd),('L',0xf7fd),
                    ('A',0x7ffd),('B',0xdffd),('M',0xfff9),('H',0xffff),
                    ('C',0xbffd),('X',0xeffd),('Q',0xff7d),('E',0xffbd)]:
        env['EMU_INPUT']=f'1:{key}:5'
        run(sys.argv[1],elf,card,'--frames',6,'--shot',shot)
        pixels=shot.read_bytes().split(b'\n255\n')[1]
        assert px(0,0)==rgb565(raw),(key,px(0,0),rgb565(raw))
        if key=='U': assert px(4,0)==rgb565(3300),px(4,0)
        if key=='L': assert px(2,0)==rgb565(0),px(2,0)
    # Build an ESP32-S3 application image from this same linked ELF. Its
    # checksum is verified before any segment is copied into guest memory.
    data=elf.read_bytes()
    phoff=struct.unpack_from('<I',data,28)[0]
    phent,phnum=struct.unpack_from('<HH',data,42)
    segments=[]
    for i in range(phnum):
        typ,off,vaddr,_,filesz,_,_,_=struct.unpack_from('<IIIIIIII',data,phoff+i*phent)
        if typ==1 and filesz: segments.append((vaddr,data[off:off+filesz]))
    assert segments
    header=bytearray(24)
    header[0]=0xe9;header[1]=len(segments)
    struct.pack_into('<I',header,4,struct.unpack_from('<I',data,24)[0])
    struct.pack_into('<H',header,12,9)
    image=bytearray(header)
    checksum=0xef
    for addr,body in segments:
        image+=struct.pack('<II',addr,len(body))+body
        for byte in body: checksum^=byte
    image+=bytes((15-len(image)%16))
    image.append(checksum)
    binary=tmp/'test.bin';binary.write_bytes(image)
    nm=gcc.with_name(gcc.name.removesuffix('gcc')+'nm')
    syms=tmp/'firmware.sym';syms.write_text(run(nm,'-n',elf).stdout)
    env.pop('EMU_INPUT',None)
    run(sys.argv[1],binary,card,'--sym',syms,'--frames',6,'--shot',shot)
    pixels=shot.read_bytes().split(b'\n255\n')[1]
    assert px(0,0)==rgb565(0xfffd),px(0,0)
    damaged=tmp/'damaged.bin';damaged_image=bytearray(image)
    damaged_image[32]^=1;damaged.write_bytes(damaged_image)
    result=subprocess.run([str(sys.argv[1]),str(damaged),'--sym',str(syms),'--frames','1'],env=env,capture_output=True,text=True,timeout=10)
    assert result.returncode!=0 and 'checksum mismatch' in result.stderr,result.stderr
    missing=tmp/'missing.bin';missing.write_bytes(image[:-1])
    result=subprocess.run([str(sys.argv[1]),str(missing),'--sym',str(syms),'--frames','1'],env=env,capture_output=True,text=True,timeout=10)
    assert result.returncode!=0 and 'truncated ESP image' in result.stderr,result.stderr
    # Truncated ELF must be rejected, not started as another console.
    bad=tmp/'bad.elf';bad.write_bytes(elf.read_bytes()[:60])
    result=subprocess.run([str(sys.argv[1]),str(bad),'--frames','1'],env=env,capture_output=True,text=True,timeout=10)
    assert result.returncode!=0,result.stderr
print('AKA: native ELF, display, input, SD, PCM and invalid-image rejection passed')
