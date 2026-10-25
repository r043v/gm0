#!/usr/bin/env python3
"""Compare actual LX7 compiler output against the host, including window
spills deeper than physical AR64, FP arithmetic, comparisons and conversion."""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[2]
gcc=Path(os.environ.get('AKA_GCC',Path.home()/'.platformio/packages/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-gcc'))
if not gcc.exists():
    print('SKIP: ESP32-S3 GCC missing')
    raise SystemExit(0)
def run(*args):
    return subprocess.check_output([str(x) for x in args],text=True)
with tempfile.TemporaryDirectory(prefix='aka-cpu-') as tmp:
    tmp=Path(tmp)
    (tmp/'host.c').write_text('#include <stdio.h>\n#include <stdint.h>\nuint32_t compute(void);int main(void){printf("%08x",compute());}\n')
    run('cc','-O2','-ffp-contract=off',root/'tests/aka/window.c',tmp/'host.c','-o',tmp/'host')
    expected=run(tmp/'host').strip()
    run('cc','-O2',root/'tests/aka/runner.c','-lm','-o',tmp/'runner')
    for opt in ('-O0','-O1','-O2','-Os','-O3'):
        elf=tmp/'window.elf'
        run(gcc,opt,'-ffp-contract=off','-nostdlib','-mtext-section-literals','-T',root/'tests/aka/link.ld',root/'tests/aka/window.c','-o',elf)
        actual=run(tmp/'runner',elf)
        assert actual.split()[0]==expected,(opt,expected,actual)
        print(opt,expected,'windows + FP match host')
