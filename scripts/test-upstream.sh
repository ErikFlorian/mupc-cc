#!/bin/sh
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
UPSTREAM=${MUPC_EMU_DIR:-/tmp/mupc-emu}
MODE=single
if [ "${1:-}" = --suite ]; then MODE=suite; DIR=${2:?usage: test-upstream.sh --suite IMAGE_DIR}; shift 2
else IMAGE=${1:?usage: test-upstream.sh compiler-image.bin}; shift; fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
test -f "$UPSTREAM/operations.c" || { echo "upstream operations.c missing: $UPSTREAM" >&2; exit 2; }
${CXX:-c++} -std=c++17 "$ROOT/tests/prepare_upstream.cpp" -o "$TMP/prepare-upstream"
"$TMP/prepare-upstream" "$UPSTREAM/operations.c" "$TMP/operations.c"
${CC:-cc} -std=c11 -I"$UPSTREAM" -c "$TMP/operations.c" -o "$TMP/operations.o"
${CXX:-c++} -std=c++17 "$ROOT/tests/upstream.cpp" "$TMP/operations.o" -o "$TMP/upstream-run"
"$TMP/prepare-upstream" "$UPSTREAM/operations.c" "$TMP/operations-fixed.c" --addresses-only
${CC:-cc} -std=c11 -I"$UPSTREAM" "$UPSTREAM/main.c" "$TMP/operations-fixed.c" -o "$TMP/upstream-fixed"
# Verify the untouched upstream executable too, using a tiny program that only
# loads immediate bytes, emits two characters, and shuts down (safe addresses).
${CC:-cc} -std=c11 -I"$UPSTREAM" "$UPSTREAM/main.c" "$UPSTREAM/operations.c" -o "$TMP/upstream-original"
printf '\360O\324\000\360K\324\000\350' > "$TMP/smoke.bin"
RAW=$(timeout 5 "$TMP/upstream-original" "$TMP/smoke.bin")
[ "$RAW" = OK ] || { printf 'raw upstream smoke expected <OK>, got <%s>\n' "$RAW" >&2; exit 4; }
if [ "$MODE" = suite ]; then
    test -f "$DIR/manifest" || { echo "suite manifest missing: $DIR/manifest" >&2; exit 2; }
    count=0
    while IFS= read -r name; do
        set --
        while read -r address value; do set -- "$@" --expect "$address" "$value"; done < "$DIR/$name.expect"
        OUTPUT=$("$TMP/upstream-run" "$DIR/$name.bin" "$@")
        EXPECTED=$(cat "$DIR/$name.output")
        [ "$OUTPUT" = "$EXPECTED" ] || { printf '%s: expected output <%s>, got <%s>\n' "$name" "$EXPECTED" "$OUTPUT" >&2; exit 4; }
        FIXED_OUTPUT=$(timeout 10 "$TMP/upstream-fixed" "$DIR/$name.bin")
        [ "$FIXED_OUTPUT" = "$EXPECTED" ] || { printf '%s (standalone): expected output <%s>, got <%s>\n' "$name" "$EXPECTED" "$FIXED_OUTPUT" >&2; exit 4; }
        count=$((count+1))
        echo "[upstream ok] $name (bounded and standalone)" >&2
    done < "$DIR/manifest"
    echo "upstream fixture suite passed ($count images, bounded and standalone); raw upstream output smoke passed" >&2
else
    OUTPUT=$("$TMP/upstream-run" "$IMAGE" "$@")
    if [ "${MUPC_EXPECT_OUTPUT+x}" = x ]; then
        [ "$OUTPUT" = "$MUPC_EXPECT_OUTPUT" ] || { printf 'expected output <%s>, got <%s>\n' "$MUPC_EXPECT_OUTPUT" "$OUTPUT" >&2; exit 4; }
    else
        printf '%s' "$OUTPUT"
    fi
    FIXED_OUTPUT=$(timeout 10 "$TMP/upstream-fixed" "$IMAGE")
    if [ "${MUPC_EXPECT_OUTPUT+x}" = x ]; then
        [ "$FIXED_OUTPUT" = "$MUPC_EXPECT_OUTPUT" ] || { printf 'standalone emulator expected output <%s>, got <%s>\n' "$MUPC_EXPECT_OUTPUT" "$FIXED_OUTPUT" >&2; exit 4; }
    fi
fi
