#!/bin/sh
# Copyright (c) 2026 AmigaChrome Team. MIT licence (LICENSE).
# pixman.library 2.1's tests on the host: tests/pixmantest.c, big-endian on a
# 68040 (m68k-linux-gnu-gcc and qemu-m68k), against upstream Pixman 0.46.4
# built here from its release tarball for the test only (tarballs/, as
# fetch-sources.sh fetches it). Pixman is never linked into the library.
#   tests/run.sh [CASES]      (default 3000)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
OUT=${OUT:-"$ROOT/out/tests-host"}
CC=${CC_M68K:-m68k-linux-gnu-gcc}
TARBALL="$ROOT/tarballs/pixman-0.46.4.tar.gz"
WORK="$ROOT/work/pixman-0.46.4"
command -v "$CC" >/dev/null && command -v qemu-m68k >/dev/null || { echo "needs m68k-linux-gnu-gcc and qemu-m68k" >&2; exit 2; }
[ -f "$TARBALL" ] || { echo "no $TARBALL: put the release tarball there (fetch-sources.sh fetches it)" >&2; exit 2; }
echo "d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c  $TARBALL" | sha256sum -c - >/dev/null
[ -d "$WORK/pixman" ] || { mkdir -p "$ROOT/work"; tar xzf "$TARBALL" -C "$ROOT/work"; }
mkdir -p "$OUT/pixman"
sed -e 's/@PIXMAN_VERSION_MAJOR@/0/; s/@PIXMAN_VERSION_MINOR@/46/; s/@PIXMAN_VERSION_MICRO@/4/' \
  "$WORK/pixman/pixman-version.h.in" > "$OUT/pixman/pixman-version.h"
cp "$ROOT/config/pixman/pixman-config.h" "$OUT/pixman/"
CF="-O2 -m68040 -fno-delete-null-pointer-checks"
# upstream Pixman, the reference
UP=""
for s in pixman pixman-access pixman-access-accessors pixman-arm pixman-bits-image pixman-combine32 \
         pixman-combine-float pixman-conical-gradient pixman-edge pixman-edge-accessors pixman-fast-path \
         pixman-filter pixman-glyph pixman-general pixman-gradient-walker pixman-image pixman-implementation \
         pixman-linear-gradient pixman-matrix pixman-mips pixman-noop pixman-ppc pixman-radial-gradient \
         pixman-region16 pixman-region32 pixman-region64f pixman-riscv pixman-solid-fill pixman-timer \
         pixman-trap pixman-utils pixman-x86; do
  [ "$OUT/pixman/$s.o" -nt "$WORK/pixman/$s.c" ] ||
    $CC $CF -DHAVE_CONFIG_H -DPIXMAN_NO_TLS=1 -DNDEBUG -I"$OUT/pixman" -I"$WORK/pixman" -c "$WORK/pixman/$s.c" -o "$OUT/pixman/$s.o"
  UP="$UP $OUT/pixman/$s.o"
done
V="$ROOT/vendor/openamigartg/library/opengpu"
# shellcheck disable=SC2086
$CC $CF -static -Wall -Wextra -Werror -Wno-unused-parameter -I"$WORK/pixman" -I"$OUT/pixman" \
  -o "$OUT/pixmantest" "$HERE/pixmantest.c" "$ROOT/library/px_core.c" "$ROOT/library/px_region.c" \
  "$V/ogpu_build.c" "$V/ogpu_core.c" $UP -lm
qemu-m68k -cpu m68040 "$OUT/pixmantest" "${1:-3000}"
