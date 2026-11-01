#!/bin/sh
# Copyright (c) 2026 AmiCradle Team. MIT licence (LICENSE).
# PixBench for AmigaOS 3.x (68040; the harness itself uses the FPU for its timing sums).
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STOVE=${STOVE:-"$HOME/AmiCradle/stoves/os32-gcc16"}
CC=${CC:-"$STOVE/prefix/bin/m68k-amigaos-gcc"}
mkdir -p "$HERE/out/tests"
"$CC" -m68040 -m68881 -O2 -fno-delete-null-pointer-checks -Wall -Wextra -Werror -noixemul \
  -I"$HERE/include" -I"$HERE/out/include" \
  -o "$HERE/out/tests/PixBench" "$HERE/bench/pixbench.c"
echo "$HERE/out/tests/PixBench ($(wc -c < "$HERE/out/tests/PixBench") bytes)"
sha256sum "$HERE/out/tests/PixBench"
