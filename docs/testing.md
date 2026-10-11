# Testing and upstream emulator

The harness runs compiler-produced images against both the untouched emulator (safe-address smoke only) and a normalized copy of the upstream MuPC operations. `make test-upstream` emits a set of compiler fixture images and runs each through the bounded operations runner and a standalone corrected emulator. The script also accepts a single image path:

```sh
scripts/test-upstream.sh build/example.bin
```

It expects the upstream source at `/tmp/mupc-emu` by default; set `MUPC_EMU_DIR` to another checkout. For the bounded runner, the harness builds `tests/prepare_upstream.cpp`, which edits only a temporary copy of `operations.c` to correct address-byte composition and turn shutdown into an observable flag; it then links this copy with `tests/upstream.cpp`. The runner bounds the instruction count, captures stdout, and checks expected memory bytes. The suite also builds a standalone emulator from the upstream `main.c` and a temporary address-corrected `operations.c`. To build that emulator for direct use, run:

```sh
MUPC_EXPECT_OUTPUT=OK scripts/test-upstream.sh build/hello.bin
scripts/test-upstream.sh build/result.bin --expect 0x0200 42
scripts/build-fixed-emu.sh build/mupc-emu-fixed
```

The reusable standalone executable built by `scripts/build-fixed-emu.sh` keeps upstream shutdown behavior and corrects only 16-bit address composition. The bounded runner checks expected global bytes; standalone runs compare captured output for every fixture.

The standalone suite runs with `make test` and covers compiler behavior plus assembler encoding and diagnostics. The upstream harness never modifies the supplied source tree. `make probe-upstream-address` runs a small direct probe against unmodified `operations.c` and confirms the signed-address bug.

## Upstream address behavior

The upstream emulator stores registers in signed 8-bit objects. Some memory-address expressions combine those bytes after integer promotion, so a byte with its high bit set can sign-extend and produce the wrong 16-bit address. The C++ helper `tests/prepare_upstream.cpp` makes two changes to a temporary copy of `operations.c`: it casts both address bytes to `uint8_t` before combining them, and it changes the shutdown call into a runner-visible _halt flag so the test process can observe termination without exiting. This corrected copy lets the emulator exercise normal compiler-generated memory and control-flow code.

`make probe-upstream-address` checks this bug against the unmodified source: with `c:d = 0x0080`, upstream `sjm_op` produces `jump_address = 0xff80`. An AddressSanitizer probe of `wrm` with the same address reports an out-of-bounds write. These behaviors were observed in the public [`mupc-emu` source](https://github.com/DanielDe8/mupc-emu), retrieved to `/tmp/mupc-emu`. Network restrictions prevented a Git clone, and the source snapshot has no commit metadata, so this documents the repository rather than a pinned revision.

This normalization is limited to the test harness. It does not modify an upstream checkout. Do not treat a passing run in the normalized harness as proof that general compiler output works on the untouched emulator: raw upstream runs can misbehave when compiler-generated pointers or jump addresses have high bytes `0x80..0xff`. The compiler also avoids using carry-dependent sequences as a correctness primitive because carry updates are not reliable in the reference implementation.

## Repository test workflow

Build with `make`; run the compiler and assembler suites with `make test`. `make test-upstream` first compiles and runs a minimal `OK` program on the untouched emulator, then runs compiler-generated fixtures against both the bounded operation runner and standalone address-corrected emulator with output and selected memory checks. The raw smoke only checks instruction fetch, immediate loads, character output, and shutdown; it uses no RAM references or nonzero load addresses. The fixtures cover preprocessing, output, signed division, shifts, comparisons and 16-bit pointers, short-circuit logic, nested loops, pointer/struct strides, recursion, pointer returns and `sizeof`, nonzero `_origin`, diagnostics, and an integrated language program. The bounded runner loads flat images at their recorded origin; the standalone harness prepends a test-only transfer to run nonzero-origin fixtures. To run an individual example against the normalized harness, set `MUPC_EXPECT_OUTPUT` to its expected output. The examples produce `AB6`, `6`, `Hi`, `@`, and `OK` respectively. Preserve compiler-produced assembly (`-S`) for debugging. Include high-page pointer cases when validating the normalized emulator path.

The requested temporary `git clone` could not be completed because DNS access to GitHub was unavailable. The local `/tmp/mupc-emu` source snapshot was used to inspect and exercise the reference operations instead. This is a limitation of the upstream validation provenance.

The source snapshots inspected on 2026-10-10 are fingerprinted in [upstream-sources.sha256](upstream-sources.sha256). Paths in that manifest are relative to `/tmp`; it records the files used, rather than a Git commit.

Final validation passed the compiler suite, six additional backend cases, all 336 primary/sub-operation register encodings plus assembler directive/error checks, and 15 compiled images through both upstream runners. The unmodified emulator output smoke and signed-address bug probe also passed. AddressSanitizer and UndefinedBehaviorSanitizer passed the in-tree suites with leak detection disabled because LeakSanitizer cannot run under this environment's tracing restrictions:

```sh
ASAN_OPTIONS=detect_leaks=0 make -j4 BUILD=build-sanitize \
  CXXFLAGS='-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fsanitize=address,undefined -fno-omit-frame-pointer' test
```

To reproduce upstream tests from a Git checkout on a host with network access:

```sh
git clone https://github.com/DanielDe8/mupc-emu.git /tmp/mupc-emu
make test-upstream
```
