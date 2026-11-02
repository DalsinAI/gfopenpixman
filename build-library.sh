#!/bin/sh
# Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
# pixman.library 2.1 for AmigaOS 3.x: a thin layer over GFOpenGPU (DESIGN.md).
# No upstream Pixman and no floating point: built for the 68040 with
# -msoft-float, so it also runs on an LC040 or EC040; the build fails if an
# FPU instruction or anything undefined gets in. Bare (no startup code or C
# library), as gfopenrtg's libraries are. Needs gfopengpu.library with
# GFOpenGPU stream v1.4 at run time.
#   ./build-library.sh             (STOVE= the os32-gcc16 stove, OUT= default out/)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STOVE=${STOVE:-"$HOME/AmiCradle/stoves/os32-gcc16"}
P="$STOVE/prefix"
CC=${CC:-"$P/bin/m68k-amigaos-gcc"}
NM=${NM:-"$P/bin/m68k-amigaos-nm"}
OBJDUMP=${OBJDUMP:-"$P/bin/m68k-amigaos-objdump"}
OUT=${OUT:-"$HERE/out"}
V="$HERE/vendor/openamigartg"
mkdir -p "$OUT/lib" "$OUT/include/libraries" "$OUT/include/proto" "$OUT/include/inline"
cp "$HERE/include/libraries/pixman.h" "$OUT/include/libraries/"
cp "$HERE/include/proto/pixman.h" "$OUT/include/proto/"
cp "$HERE/include/inline/pixman.h" "$OUT/include/inline/"

FLAGS="-m68040 -msoft-float -mnobitfield -O2 -fno-delete-null-pointer-checks -fomit-frame-pointer -fno-toplevel-reorder
  -fno-builtin -fno-tree-loop-distribute-patterns -Wall -Wextra -Werror -Wno-unused-parameter -nostartfiles -nostdlib"
# shellcheck disable=SC2086
"$CC" $FLAGS -I"$HERE/include" -Wl,-Map,"$OUT/lib/pixman.library.map" -o "$OUT/lib/pixman.library" \
  "$HERE/library/pixman_lib.c" "$HERE/library/px_core.c" "$HERE/library/px_region.c" "$V/library/opengpu/ogpu_build.c" -lgcc
und=$("$NM" -u "$OUT/lib/pixman.library" || true)
[ -z "$und" ] || { echo "undefined in pixman.library: $und" >&2; exit 1; }
# the mnemonic is objdump's third tab-separated field; the 68881's all start with f
if "$OBJDUMP" -d "$OUT/lib/pixman.library" | awk -F '\t' 'NF >= 3 && $3 ~ /^f[a-z]/ { found = 1 } END { exit !found }'; then
  echo "pixman.library has FPU instructions" >&2; exit 1
fi
echo "$OUT/lib/pixman.library ($(wc -c < "$OUT/lib/pixman.library") bytes, no FPU instructions)"
