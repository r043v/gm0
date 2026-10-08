#!/usr/bin/env bash
# Tests unitaires du cœur (compilation de gm0.c dans chaque test).
#   tests/unit/run.sh
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/gm0-unit.XXXXXX")
trap 'rm -rf "$out"' EXIT
cc=${CC:-cc}
sdl=$(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2")
for t in "$here"/test_*.c; do
    name=$(basename "$t" .c)
    # -O0 : tests de logique, pas de performance ; warnings du cœur tolérés
    "$cc" -O0 -g -o "$out/$name" "$t" $sdl -lz -lm
    "$out/$name"
done

