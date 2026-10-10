#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UPSTREAM=${MUPC_EMU_DIR:-/tmp/mupc-emu}
OUT=${1:-"$ROOT/build/mupc-emu-fixed"}
test -f "$UPSTREAM/main.c" && test -f "$UPSTREAM/operations.c" || {
    echo "upstream main.c/operations.c missing: $UPSTREAM" >&2; exit 2;
}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$(dirname -- "$OUT")"
${CXX:-c++} -std=c++17 "$ROOT/tests/prepare_upstream.cpp" -o "$TMP/prepare-upstream"
"$TMP/prepare-upstream" "$UPSTREAM/operations.c" "$TMP/operations.c" --addresses-only
${CC:-cc} -std=c11 -I"$UPSTREAM" "$UPSTREAM/main.c" "$TMP/operations.c" -o "$OUT"
printf '%s\n' "$OUT"
