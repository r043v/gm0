#!/usr/bin/env python3
"""Real LX7 ELF: scaled RGB565 rows, keys, SD sectors and PCM through gm0."""
import os
from pathlib import Path
import subprocess
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
    # Truncated ELF must be rejected, not started as another console.
    bad=tmp/'bad.elf';bad.write_bytes(elf.read_bytes()[:60])
    result=subprocess.run([str(sys.argv[1]),str(bad),'--frames','1'],env=env,capture_output=True,text=True,timeout=10)
    assert result.returncode!=0,result.stderr
print('AKA: native ELF, display, input, SD, PCM and invalid-image rejection passed')
