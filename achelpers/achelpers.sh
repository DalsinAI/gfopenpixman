#!/bin/sh
# AC090's native helpers for gfopenpixman's build: amicradle-guest's
# common/amiga/ac_helpers.c, ac_helpers.h, ac_helpers.sh and ac_magic.h at
# the commit in AMIGACHROME_GUEST_PINNED_COMMIT, built by that ac_helpers.sh
# into OUTDIR/libachelpers.a, one object for each function in OUTDIR/obj.
# The sources stay in OUTDIR/src, for -I.
# MIT, Copyright (c) 2026 AmiCradle Team.
#
#   AMIGACHROME_GUEST  a checkout of amicradle-guest that has the pinned
#                      commit (default: amicradle-guest, else guest, beside
#                      this repository)
#   CC, AR, CFLAGS     the compiler, archiver and flags of the build using them
#
# usage: achelpers/achelpers.sh OUTDIR
#        achelpers/achelpers.sh --have   (succeeds when the pinned commit is there)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
out=${1:?usage: achelpers.sh OUTDIR}
G=${AMIGACHROME_GUEST:-}
if [ -z "$G" ]; then
    G="$ROOT/../amicradle-guest"
    if [ ! -d "$G" ] && [ -d "$ROOT/../guest" ]; then G="$ROOT/../guest"; fi
fi
pin=$(tr -d ' \r\n' < "$ROOT/AMIGACHROME_GUEST_PINNED_COMMIT")
if [ "$out" = --have ]; then git -C "$G" cat-file -e "$pin^{commit}" 2>/dev/null; exit; fi
if ! git -C "$G" cat-file -e "$pin^{commit}" 2>/dev/null; then
    echo "amicradle-guest commit $pin (AMIGACHROME_GUEST_PINNED_COMMIT) is not in $G:"
    echo "fetch it there, set AMIGACHROME_GUEST to a checkout that has it, or build with AC_HELPERS=0"
    exit 2
fi
mkdir -p "$out/src"
for f in ac_magic.h ac_helpers.h ac_helpers.c ac_helpers.sh; do
    git -C "$G" show "$pin:common/amiga/$f" > "$out/src/$f"
done
sh "$out/src/ac_helpers.sh" "$out"
echo "libachelpers.a (amicradle-guest $(echo "$pin" | cut -c1-12)): $(wc -c < "$out/libachelpers.a") bytes"
