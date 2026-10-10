#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UPSTREAM=${MUPC_EMU_DIR:-/tmp/mupc-emu}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
test -f "$UPSTREAM/operations.c" || { echo "upstream operations.c missing: $UPSTREAM" >&2; exit 2; }
${CC:-cc} -std=c11 -I"$UPSTREAM" -c "$UPSTREAM/operations.c" -o "$TMP/operations.o"
${CXX:-c++} -std=c++17 -I"$UPSTREAM" "$TMP/operations.o" \
  "$ROOT/tests/upstream_signed_address_probe.cpp" -o "$TMP/address-probe"
"$TMP/address-probe"
