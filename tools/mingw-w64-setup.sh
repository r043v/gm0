#!/bin/sh
# Installe, sans root, la chaîne de compilation croisée MinGW-w64 (x86_64) et
# les bibliothèques Windows de gm0 : SDL2 (archive officielle) et zlib
# (compilée depuis ses sources).  Lancé par `make win-setup`.
#
#   GM0_MINGW_DIR    préfixe d'installation (défaut : ~/.local/mingw-w64)
#   GM0_MINGW_CACHE  dossier des archives téléchargées (défaut : ~/.cache/gm0-mingw-dl)
#   GM0_MINGW_KEEP   1 : garde les archives après l'installation (défaut : supprimées)
#
# Disposition : $PREFIX/usr/ (paquets Arch extra : gcc, binutils, crt, headers,
# winpthreads), $PREFIX/deps/sdl2 (lien vers SDL2-<version>) et $PREFIX/deps/zlib.
# Ce qui n'a pas d'usage pour gm0 (cible 32 bits, C++, Fortran, Ada,
# documentation) n'est pas extrait : environ 1 Go de moins sur le disque.
# Les paquets Arch sont résolus sur le miroir (une seule version y est publiée) ;
# SDL2 et zlib sont épinglées ci-dessous.
set -eu

SDL2_VER=2.32.10
ZLIB_VER=1.3.2
ARCH_MIRROR=https://geo.mirror.pkgbuild.com/extra/os/x86_64
PREFIX=${GM0_MINGW_DIR:-$HOME/.local/mingw-w64}
CACHE=${GM0_MINGW_CACHE:-$HOME/.cache/gm0-mingw-dl}
KEEP=${GM0_MINGW_KEEP:-0}

# chemins relatifs à usr/ non utilisés par gm0 (motifs shell, sans espace)
UNUSED="
i686-w64-mingw32
lib/gcc/i686-w64-mingw32
bin/i686-w64-mingw32-*
bin/x86_64-w64-mingw32-c++
bin/x86_64-w64-mingw32-g++
bin/x86_64-w64-mingw32-gfortran
bin/x86_64-w64-mingw32-gnat*
lib/gcc/x86_64-w64-mingw32/*/cc1plus
lib/gcc/x86_64-w64-mingw32/*/cc1obj
lib/gcc/x86_64-w64-mingw32/*/cc1objplus
lib/gcc/x86_64-w64-mingw32/*/g++-mapper-server
lib/gcc/x86_64-w64-mingw32/*/f951
lib/gcc/x86_64-w64-mingw32/*/gnat1
lib/gcc/x86_64-w64-mingw32/*/finclude
lib/gcc/x86_64-w64-mingw32/*/adainclude
lib/gcc/x86_64-w64-mingw32/*/adalib
lib/gcc/x86_64-w64-mingw32/*/ada_target_properties
lib/gcc/x86_64-w64-mingw32/*/libcaf_*
lib/gcc/x86_64-w64-mingw32/*/libgfortran*
lib/gcc/x86_64-w64-mingw32/*/libgnat*
x86_64-w64-mingw32/include/c++
x86_64-w64-mingw32/lib/libstdc++*
x86_64-w64-mingw32/lib/libsupc++*
x86_64-w64-mingw32/lib/libgfortran*
x86_64-w64-mingw32/lib/libgnat*
share/man
share/info
share/doc
share/licenses
"

say() { printf '==> %s\n' "$*"; }
die() { printf 'erreur : %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "outil manquant : $1"; }

# télécharge URL dans DEST (fichier partiel écarté en cas d'échec)
fetch() {
    say "téléchargement $(basename "$2")"
    curl -sSfL -o "$2.part" "$1" && mv "$2.part" "$2" || { rm -f "$2.part"; die "échec : $1"; }
}

# nom de fichier du paquet $1 (ex. mingw-w64-gcc) sur le miroir Arch
arch_file() {
    listing=$(curl -sSfL "$ARCH_MIRROR/") || die "miroir Arch injoignable : $ARCH_MIRROR"
    printf '%s\n' "$listing" | grep -oE "$1-[0-9][^\"<> ]*\.pkg\.tar\.zst" | sort -u | head -n 1
}

# extrait un paquet Arch dans $PREFIX, sans les parties inutiles
extract_arch() {
    set -- --zstd --wildcards "$@"
    set -f
    for p in $UNUSED; do set -- "$@" "--exclude=usr/$p" "--exclude=usr/$p/*"; done
    set +f
    tar "$@" -xf "$archive" -C "$PREFIX"
}

# supprime ce qui est inutile dans une installation existante (même liste)
strip_arch() {
    cd "$PREFIX/usr"
    for p in $UNUSED; do rm -rf $p; done
    cd "$PREFIX"
}

need curl; need tar; need zstd; need make
mkdir -p "$PREFIX" "$CACHE"
GCC="$PREFIX/usr/bin/x86_64-w64-mingw32-gcc"

# 1. chaîne de compilation (paquets Arch)
if [ ! -x "$GCC" ]; then
    for name in mingw-w64-binutils mingw-w64-crt mingw-w64-gcc mingw-w64-headers mingw-w64-winpthreads; do
        file=$(arch_file "$name")
        [ -n "$file" ] || die "paquet introuvable sur le miroir : $name"
        [ -f "$CACHE/$file" ] || fetch "$ARCH_MIRROR/$file" "$CACHE/$file"
        archive="$CACHE/$file"
        say "extraction $file"
        extract_arch
    done
fi
strip_arch

# 2. SDL2 (archive officielle MinGW)
SDL="$PREFIX/deps/SDL2-$SDL2_VER"
if [ ! -d "$SDL" ]; then
    f="SDL2-devel-$SDL2_VER-mingw.tar.gz"
    [ -f "$CACHE/$f" ] || fetch "https://www.libsdl.org/release/$f" "$CACHE/$f"
    say "extraction SDL2 $SDL2_VER"
    mkdir -p "$PREFIX/deps"
    tar -xzf "$CACHE/$f" -C "$PREFIX/deps"
fi
(cd "$SDL" && rm -rf i686-w64-mingw32 docs test)
ln -sfn "SDL2-$SDL2_VER" "$PREFIX/deps/sdl2"

# 3. zlib, compilée avec la chaîne MinGW
ZLIBD="$PREFIX/deps/zlib"
if [ ! -f "$ZLIBD/lib/libz.a" ]; then
    f="zlib-$ZLIB_VER.tar.gz"
    [ -f "$CACHE/$f" ] || fetch "https://zlib.net/$f" "$CACHE/$f"
    tmp=$(mktemp -d "${TMPDIR:-/tmp}/gm0-zlib.XXXXXX")
    trap 'rm -rf "$tmp"' EXIT
    tar -xzf "$CACHE/$f" -C "$tmp"
    say "compilation zlib $ZLIB_VER"
    (cd "$tmp/zlib-$ZLIB_VER" && PATH="$PREFIX/usr/bin:$PATH" \
        make -f win32/Makefile.gcc PREFIX=x86_64-w64-mingw32- libz.a zlib1.dll >/dev/null)
    mkdir -p "$ZLIBD/include" "$ZLIBD/lib" "$ZLIBD/bin"
    cp "$tmp/zlib-$ZLIB_VER/zlib.h" "$tmp/zlib-$ZLIB_VER/zconf.h" "$ZLIBD/include/"
    cp "$tmp/zlib-$ZLIB_VER/libz.a" "$ZLIBD/lib/"
    cp "$tmp/zlib-$ZLIB_VER/zlib1.dll" "$ZLIBD/bin/"
    rm -rf "$tmp"; trap - EXIT
fi

# 4. vérification : le compilateur produit un exécutable PE et les bibliothèques sont là
say "vérification"
vt=$(mktemp -d "${TMPDIR:-/tmp}/gm0-check.XXXXXX")
printf 'int main(void) { return 0; }\n' > "$vt/t.c"
"$GCC" "$vt/t.c" -o "$vt/t.exe" || die "le compilateur croisé ne fonctionne pas"
rm -rf "$vt"
for f in "$PREFIX/deps/sdl2/x86_64-w64-mingw32/lib/cmake/SDL2/sdl2-config.cmake" \
         "$PREFIX/deps/sdl2/x86_64-w64-mingw32/bin/SDL2.dll" \
         "$ZLIBD/include/zlib.h" "$ZLIBD/lib/libz.a" "$ZLIBD/bin/zlib1.dll"; do
    [ -f "$f" ] || die "fichier manquant après installation : $f"
done

# 5. nettoyage des archives téléchargées
if [ "$KEEP" != 1 ]; then
    rm -f "$CACHE"/mingw-w64-*.pkg.tar.zst "$CACHE"/SDL2-devel-*-mingw.tar.gz "$CACHE"/zlib-*.tar.gz
    rmdir "$CACHE" 2>/dev/null || true
fi
say "installé dans $PREFIX ($(du -sh "$PREFIX" | cut -f1)) : make win"
