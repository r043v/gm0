#!/usr/bin/env python3
"""Régénère esp8266_rom_syms.h depuis eagle.rom.addr.v6.ld (PlatformIO)."""
import os, re, sys
p = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser(
    '~/.platformio/packages/framework-arduinoespressif8266/tools/sdk/ld/eagle.rom.addr.v6.ld')
syms = {}
for l in open(p):
    m = re.match(r'\s*PROVIDE\s*\(\s*(\w+)\s*=\s*(0x[0-9a-fA-F]+)\s*\)\s*;', l)
    if m: syms[m.group(1)] = int(m.group(2), 16)
out = ['/* Adresses des fonctions de la ROM de l\'ESP8266 (eagle.rom.addr.v6.ld du SDK Espressif :',
       ' * noms et adresses seulement, aucun code de la ROM).  Généré par tests/esp/gen_rom_syms.py. */',
       '#ifndef ESP8266_ROM_SYMS_H', '#define ESP8266_ROM_SYMS_H',
       'static const struct { uint32_t addr; const char *name; } xt_rom_syms[] = {']
for n, a in sorted(syms.items(), key=lambda kv: (kv[1], kv[0])): out.append('    {0x%08x, "%s"},' % (a, n))
out += ['};', '#define XT_ROM_SYMS (sizeof xt_rom_syms / sizeof xt_rom_syms[0])', '#endif', '']
open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'esp8266_rom_syms.h'), 'w').write('\n'.join(out))
