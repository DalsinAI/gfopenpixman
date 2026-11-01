#!/bin/sh
# Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
# PixmanTest for AmigaOS 3.x (68040; the harness itself uses the FPU for its timing sums).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STOVE=${STOVE:-"$HOME/AmiCradle/stoves/os32-gcc16"}
CC=${CC:-"$STOVE/prefix/bin/m68k-amigaos-gcc"}
mkdir -p "$HERE/out/tests"
"$CC" -m68040 -m68881 -O2 -fno-delete-null-pointer-checks -Wall -Wextra -Werror -noixemul \
  -I"$HERE/include" -I"$HERE/out/include" \
  -o "$HERE/out/tests/PixmanTest" "$HERE/tests/pixmansmoke.c"
echo "$HERE/out/tests/PixmanTest ($(wc -c < "$HERE/out/tests/PixmanTest") bytes)"
