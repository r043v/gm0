#!/usr/bin/env python3
"""Generate a cartridge header in a build directory, never in tracked sources."""
from pathlib import Path
import sys
import json
data = Path(sys.argv[1]).read_bytes()
if not 0 < len(data) <= 49152:
    raise SystemExit('SG-1000 cartridge must contain 1–49152 bytes')
rows = [','.join(f'0x{b:02x}' for b in data[i:i+16]) for i in range(0,len(data),16)]
Path(sys.argv[2]).write_text('static const unsigned char sg_embedded_rom[] = {\n'+',\n'.join(rows)+'\n};\nstatic const char sg_embedded_name[] = '+json.dumps(Path(sys.argv[1]).name)+';\n')
