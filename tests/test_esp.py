#!/usr/bin/env python3
"""Machine ESPboy de gm0, de bout en bout, avec un firmware de test compilé pour
lx106 (tests/esp/fw_test.c) : écran, boutons, audio (sigma-delta + timer1),
sauvegardes en flash persistées d'une partie à l'autre.  Nécessite le
toolchain xtensa de PlatformIO ; sans lui le test est sauté."""
import glob, hashlib, os, subprocess, sys, tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
gm0 = Path(sys.argv[1]) if len(sys.argv) > 1 else root / 'build/bin/gm0'
gcc = glob.glob(os.path.expanduser('~/.platformio/packages/toolchain-xtensa/bin/xtensa-lx106-elf-gcc'))
if not gcc:
    print('SKIP : toolchain xtensa-lx106 absent'); sys.exit(0)
env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', EMU_NOPACE='1')
rom_ld = os.path.expanduser('~/.platformio/packages/framework-arduinoespressif8266/tools/sdk/ld/eagle.rom.addr.v6.ld')

def run(*a, **kw):
    return subprocess.run(list(map(str, a)), check=True, capture_output=True, text=True, env=env, **kw)

def ppm(path):
    data = Path(path).read_bytes()
    head, rest = data.split(b'\n255\n', 1)
    return rest

with tempfile.TemporaryDirectory(prefix='gm0-esp-') as tmp:
    tmp = Path(tmp)
    ld = tmp / 'link.ld'
    ld.write_text((root / 'tests/esp/link.ld').read_text())
    elf = tmp / 'fw.elf'
    run(gcc[0], '-O2', '-mlongcalls', '-mtext-section-literals', '-ffreestanding', '-nostdlib', '-T', ld,
        '-o', elf, root / 'tests/esp/fw_test.c', '-Wl,--entry=setup')
    shot = lambda n: tmp / n
    # première partie : rien de sauvegardé, repère rouge
    run(gm0, elf, '--frames', 90, '--shot', shot('a.ppm'), '--wav', shot('a.wav'))
    a = ppm(shot('a.ppm'))
    assert len(a) == 128 * 128 * 3, len(a)
    assert a[0:3] == bytes([248, 0, 0]), a[0:3].hex()        # repère rouge (0xf800)
    px = lambda img, x, y: img[(y * 128 + x) * 3:(y * 128 + x) * 3 + 3]
    assert px(a, 100, 100) == bytes([25 << 3, 50 << 2, 0]), px(a, 100, 100).hex()
    assert (tmp / 'fw.sav').exists(), 'la sauvegarde de la flash utilisateur doit être écrite'
    wav = shot('a.wav').read_bytes()
    assert len(wav) > 44 + 10000 and len(set(wav[44:])) > 2, 'audio : le sigma-delta mené par timer1 doit faire du son'
    # seconde partie : la sauvegarde est relue, repère vert
    run(gm0, elf, '--frames', 30, '--shot', shot('b.ppm'))
    b = ppm(shot('b.ppm'))
    assert b[0:3] == bytes([0, 252, 0]), b[0:3].hex()
    # touche « droite » (EMU_INPUT R) : couleurs inversées
    env['EMU_INPUT'] = '5:R:100'
    run(gm0, elf, '--frames', 30, '--shot', shot('c.ppm'))
    c = ppm(shot('c.ppm'))
    assert px(b, 100, 100) == bytes([25 << 3, 50 << 2, 31 << 3]) and px(c, 100, 100) == bytes([6 << 3, 13 << 2, 0]), (px(c, 100, 100).hex(), px(b, 100, 100).hex())
    # déterminisme
    env.pop('EMU_INPUT')
    run(gm0, elf, '--frames', 30, '--shot', shot('d.ppm'))
    assert hashlib.md5(ppm(shot('d.ppm'))).digest() == hashlib.md5(b).digest()
print('machine ESPboy : écran, boutons, audio, timer1, sauvegarde de la flash : ok')
