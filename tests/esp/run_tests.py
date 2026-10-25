#!/usr/bin/env python3
"""Cœur Xtensa de gm0 : programmes C compilés pour lx106 et exécutés sur
esp8266_xt.h, comparés au même code exécuté sur l'hôte.  Nécessite le
toolchain PlatformIO (toolchain-xtensa)."""
import glob, os, subprocess, sys, tempfile
from pathlib import Path
here = os.path.dirname(os.path.abspath(__file__))
tc = glob.glob(str(Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home()/'.platformio'))/'packages/toolchain-xtensa/bin/xtensa-lx106-elf-gcc'))
if not tc:
    print('SKIP : toolchain xtensa-lx106 absent'); sys.exit(0)
gcc = tc[0]
def run(*a): return subprocess.run(a, check=True, capture_output=True, text=True).stdout
rom_ld = str(Path(os.environ.get('PLATFORMIO_CORE_DIR', Path.home()/'.platformio'))/'packages/framework-arduinoespressif8266/tools/sdk/ld/eagle.rom.addr.v6.ld')
with tempfile.TemporaryDirectory() as tmp:
    # la ROM de la puce fournit les divisions, la virgule flottante et memcpy : la machine les émule
    with open(os.path.join(here, 'link.ld')) as f, open(os.path.join(tmp, 'link.ld'), 'w') as g:
        g.write(f.read() + '\nINCLUDE ' + rom_ld + '\n')
    runner = os.path.join(tmp, 'xt_run')
    run('cc', '-O2', '-Wall', '-o', runner, os.path.join(here, 'xt_run.c'))
    host = os.path.join(tmp, 'host')
    run('cc', '-O1', '-fno-strict-aliasing', '-o', host, os.path.join(here, 'prog.c'), os.path.join(here, 'host_main.c'))
    ref = run(host).strip()
    STAGES = 6
    for opt in ('-O0', '-O1', '-O2', '-Os', '-O3'):
        for extra in ([], ['-mno-serialize-volatile']):
            elf = os.path.join(tmp, 'p.elf')
            run(gcc, opt, '-mlongcalls', '-mtext-section-literals', '-ffreestanding', '-nostdlib', '-fno-strict-aliasing',
                *extra, '-T', os.path.join(tmp, 'link.ld'), '-o', elf,
                os.path.join(here, 'start.S'), os.path.join(here, 'prog.c'), '-lgcc')
            out = run(runner, elf).strip().split()[0]
            status = 'ok' if out == ref else 'DIFFÉRENT'
            print('%-4s %-24s %s (hôte %s, xtensa %s)' % (opt, ' '.join(extra), status, ref, out))
            if out != ref:
                for st in range(1, STAGES):
                    a = run(host, str(st)).strip(); b = run(runner, elf, str(st)).strip().split()[0]
                    print('   étape %d : hôte %s xtensa %s %s' % (st, a, b, '' if a == b else '<-- premier écart'))
                    if a != b:
                        if st == 1:
                            for op in range(0, 60):
                                a = run(host, str(100 + op)).strip(); b = run(runner, elf, str(100 + op)).strip().split()[0]
                                if a != b:
                                    print('   opération %d : hôte %s xtensa %s' % (op, a, b)); break
                        break
                sys.exit(1)
print('cœur Xtensa : empreintes identiques à l\'hôte')
