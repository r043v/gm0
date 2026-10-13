#!/usr/bin/env python3
"""Embed a user-provided game in a generated build-directory header."""
import json
from pathlib import Path
import sys
source = Path(sys.argv[1])
data = source.read_bytes()
if not data:
    raise SystemExit('Embedded game must not be empty')
rows = [','.join(f'0x{byte:02x}' for byte in data[i:i+16]) for i in range(0, len(data), 16)]
Path(sys.argv[2]).write_text('/* Generated game; keep outside tracked sources. */\n'
    'static const unsigned char gm0_bundle_data[] = {\n' + ',\n'.join(rows) + '\n};\n'
    'static const char gm0_bundle_name[] = ' + json.dumps(source.name) + ';\n')
