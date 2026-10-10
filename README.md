# mupc-cc — pan_devecka

`mupc-cc` is a C++17 compiler and assembler for the MuPC hobby 8-bit CPU. It compiles one C-like source translation unit directly to a flat memory image; there is no linker or standard library. The compiler is built around the machine’s four registers: `a` and `b` are the two general-purpose registers, while `c` and `d` form the shared 16-bit address word. Expression values and call-frame data are kept in RAM.

## Build and run

Build with the included Makefile and a C++17 compiler:

```sh
make
make test
```

`make` creates `build/mupc-cc`. `make test` runs the compiler integration checks and assembler checks. To compile and execute a program in the upstream emulator harness, see [Testing](docs/testing.md).

Compile a source file to a binary image:

```sh
build/mupc-cc examples/hello.mc -o hello.bin
```

The compiler derives `hello.bin` automatically when `-o` is omitted. You can also save generated assembly, write a symbol map, add include directories and command-line definitions, or select the source origin:

```sh
build/mupc-cc examples/hello.mc -S hello.asm --map hello.map
build/mupc-cc app.mc -I include -DDEBUG=1 --origin 0x0100 -o app.bin
```

Assemble MuPC assembly directly:

```sh
build/mupc-cc --assemble hello.asm -o hello.bin
```

Use `mupc-cc --help` for all options and `mupc-cc --version` for the version. `--origin ADDRESS` overrides the source `origin ADDRESS;` directive.

## Language features

The language supports:

- `int` and `char` as signed 8-bit values, and `uint` as an unsigned 8-bit value; there are no floating-point types.
- 16-bit pointers, address-of and dereference, pointer arithmetic, indexing, and pointer comparisons/differences.
- Fixed-size arrays and packed structs, with brace initializers and string literals.
- Global and local variables, functions, parameters, recursion, and `void` returns.
- Arithmetic, comparisons, bitwise and logical operators, assignments, increments, and `sizeof`.
- `if`/`else`, `while`, `do`/`while`, `for`, `break`, `continue`, labels, and `goto`.
- Decimal, `0b` binary, and `0x` hexadecimal integer literals; negative expressions; escaped string and character literals.
- Inline MuPC assembly, the `origin` directive, and `#include`, `#define`, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#else`, `#elif`, and `#endif`.

The preprocessor is intentionally smaller than the C preprocessor; see [docs/language.md](docs/language.md) for details and limitations. The built-ins are `putchar(value)` and `halt()`. `putchar` emits the low byte through the emulator’s output instruction, while `halt` shuts down the CPU. No headers are needed for these built-ins.

## Example

This program uses an array, pointer arithmetic, a struct, a loop, a function, and output. It compiles as-is without an include file:

```c
struct Pair { uint left; uint right; };

uint sum(uint *values, uint count) {
    uint total = 0;
    for (uint i = 0; i < count; i++) total += values[i];
    return total;
}

int main(void) {
    uint values[2] = { 65, 10 };
    struct Pair pair = { values[0], values[1] };
    uint *first = &pair.left;
    putchar(*first);             // prints A
    return sum(first, 2);        // return values are not printed
}
```

Other standalone examples are in `examples/`: `hello.mc`, `factorial.mc`, `arrays_structs.mc`, `macros.mc`, and `origin_asm.mc`. Each can be compiled with `build/mupc-cc examples/NAME.mc -o NAME.bin`.

## CPU and runtime model

An instruction byte is composed of a 4-bit opcode and two 2-bit register fields. Grouped operations use the first field as a sub-operation selector. The CPU’s full instruction set, exact register mapping, address behavior, and assembler syntax are documented in [docs/architecture.md](docs/architecture.md). See the upstream [`mupc-emu`](https://github.com/danielde8/mupc-emu) and [`mupc-asm`](https://github.com/DanielDe8/mupc-asm) projects.

The compiler emits its own call and return sequences because MuPC has no native call instruction. Function slots and expression temporaries use fixed RAM addresses. Recursive calls save frame bytes on the hardware stack but reuse the same RAM locations for that function’s locals; a pointer to one activation’s local can therefore refer to a deeper activation’s reused slot. See the [language guide](docs/language.md) for this and other target-specific limits.

`origin N;` puts a bootstrap jump at address zero and places the compiled program at `N`. Assembly `org N` is a separate assembler location directive. Arrays and structs are contiguous and have no padding. In compiler-managed RAM, pointers are stored low byte first; assembly `dw` emits a big-endian word.

## Upstream emulator address bug

The upstream emulator stores registers in signed 8-bit C values and has memory/jump address expressions that promote those bytes as signed integers. When address bytes have their high bit set, the resulting address can be wrong. This affects ordinary compiler output that reads/writes RAM or jumps through such addresses. The normalized test harness fixes address composition in a temporary copy of upstream `operations.c`; it does not patch or overwrite the upstream source tree. A small, safe-address smoke program is also run against the untouched upstream emulator. The exact procedure and limitation are in [docs/testing.md](docs/testing.md).

## Documentation

- [Language reference](docs/language.md)
- [CPU and instruction reference](docs/architecture.md)
- [Testing and upstream emulator notes](docs/testing.md)
