#!/usr/bin/env python3
"""Agrège un profil EMU_PROF de gm0 par fonction (symboles de l'ELF).

usage : prof_report.py profil.txt firmware.elf [N]
Les cycles sont ceux du modèle de l'émulateur (Cortex-M0+ 48 MHz + cache
NVM) : une estimation fidèle au modèle, pas une mesure sur console.
"""
import bisect, glob, os, subprocess, sys

prof, elf = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
nm = next(iter(glob.glob(os.path.expanduser(
    "~/.platformio/packages/toolchain-gccarmnoneeabi/bin/arm-none-eabi-nm"))), "arm-none-eabi-nm")
syms = []
for line in subprocess.run([nm, "-n", "-C", elf], capture_output=True, text=True).stdout.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16) & ~1, p[2]))
addrs = [a for a, _ in syms]
tot, per = 0, {}
for line in open(prof):
    a, c = line.split()
    a, c = int(a, 16), int(c)
    i = bisect.bisect_right(addrs, a) - 1
    name = syms[i][1] if i >= 0 else "?"
    per[name] = per.get(name, 0) + c
    tot += c
print("total %d cycles (%.2f s à 48 MHz)" % (tot, tot / 48e6))
for name, c in sorted(per.items(), key=lambda x: -x[1])[:top]:
    print("%6.2f%%  %12d  %s" % (100.0 * c / tot, c, name))
