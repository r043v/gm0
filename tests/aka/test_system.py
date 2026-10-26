#!/usr/bin/env python3
"""Check gm0's optional full-system Aka launcher without requiring QEMU in CI."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

gm0 = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='gm0-aka-system-test-') as work:
    base = Path(work)
    system = base / 'aka-emulator'
    (system / 'bin' / 'pc-bios').mkdir(parents=True)
    (system / 'sample' / 'real-ota').mkdir(parents=True)
    for name in ('partitions.bin', 'bootloader.bin', 'launcher.bin'):
        (system / 'sample' / 'real-ota' / name).write_bytes(b'fixture')
    qemu = system / 'bin' / 'qemu-system-xtensa'
    qemu.write_text('#!/bin/sh\nexit 0\n')
    qemu.chmod(0o755)
    front = system / 'bin' / 'aka-front'
    front.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args=sys.argv[1:]
pathlib.Path(os.environ['GM0_AKA_CAPTURE']).write_text(json.dumps({'args':args,'cwd':os.getcwd()}))
pathlib.Path(args[args.index('--flash')+1]).write_bytes(b'flash')
''')
    front.chmod(0o755)
    game = base / 'game.bin'; game.write_bytes(b'fixture')
    sd = base / 'sd'; sd.mkdir()
    capture = base / 'capture.json'
    env = dict(os.environ, GM0_AKA_CAPTURE=str(capture))
    shot = base / 'shot.bmp'
    subprocess.run([gm0, game, '--aka-system', system, '--aka-sd', sd,
                    '--aka-screenshot', shot, '--aka-shot-delay', '35',
                    '--aka-press', '0x8000:1:2'], check=True, env=env,
                   capture_output=True, text=True, timeout=10)
    result = json.loads(capture.read_text())
    args = result['args']
    assert args[args.index('--game')+1] == str(game)
    assert args[args.index('--sd')+1] == str(sd)
    assert args[args.index('--screenshot')+1] == str(shot)
    assert args[args.index('--shot-delay')+1] == '35'
    assert args[args.index('--press')+1] == '0x8000:1:2'
    assert '--boot-game' in args
    assert not Path(result['cwd']).exists(), 'temporary flash directory must be removed'
    saved = base / 'persistent-flash.bin'
    subprocess.run([gm0, game, '--aka-system', system, '--aka-flash', saved,
                    '--aka-fresh'], check=True, env=env,
                   capture_output=True, text=True, timeout=10)
    args = json.loads(capture.read_text())['args']
    assert args[args.index('--flash')+1] == str(saved)
    assert '--fresh' in args and saved.read_bytes() == b'flash'
    qemu.write_text('#!/bin/sh\nexit 1\n')
    failed = subprocess.run([gm0, game, '--aka-system', system], env=env,
                            capture_output=True, text=True, timeout=10)
    assert failed.returncode != 0 and 'QEMU cannot start' in failed.stderr
print('AKA full-system launcher: arguments, flash lifetime and preflight passed')
