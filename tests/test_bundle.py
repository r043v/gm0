#!/usr/bin/env python3
"""Exercise embedded firmware, SD-card ZIPs and command-line overrides."""
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parents[1]
env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', EMU_NOPACE='1')

def run(*args):
    return subprocess.run(list(map(str, args)), check=True, env=env, capture_output=True, text=True)

with tempfile.TemporaryDirectory(prefix='gm0-bundle-') as temporary:
    work = Path(temporary)
    meta = work / 'meta.bin'
    meta.write_bytes(struct.pack('<IIH', 0x20008000, 0x4009, 0xe7fe))
    pokitto = work / 'pokitto.pop'
    pokitto.write_bytes(struct.pack('<II', 1, 4) + b'test' + struct.pack('<IIH', 0x10008000, 9, 0xe7fe) + bytes(32))
    archive = work / 'card.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as out:
        out.writestr('game/meta.bin', meta.read_bytes())
        out.writestr('game/data.txt', 'SD-card asset')
    build = work / 'build'
    for game in (meta, pokitto, archive):
        run('cmake', '-G', 'Ninja', '-S', root, '-B', build, '-DGM0_BUNDLE=' + str(game))
        run('cmake', '--build', build)
        executable = build / 'bin/gm0'
        embedded = work / 'embedded.ppm'
        explicit = work / 'explicit.ppm'
        result = run(executable, '--frames', '2', '--shot', embedded)
        assert 'started without a firmware' not in result.stderr
        assert '[bench]' in result.stderr
        if game == archive:
            run(executable, meta, '--frames', '2', '--shot', explicit)
        else:
            run(executable, game, '--frames', '2', '--shot', explicit)
        assert embedded.read_bytes() == explicit.read_bytes(), game
        override = run(executable, pokitto, '--frames', '2')
        assert 'Pokitto: SP=10008000' in override.stderr, override.stderr
        print(game.suffix + ': embedded launch and explicit override OK')
