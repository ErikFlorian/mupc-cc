# mupc-cc language reference

This guide describes the C-like source accepted by `mupc-cc`. It is intentionally a compact systems language for MuPC, not full ISO C. All source files in one invocation form one program; there is no linker or separate compilation ABI.

## Types and storage

| Type | Size | Meaning |
|---|---:|---|
| `int`, `char` | 1 byte | Signed 8-bit integer (`-128..127` when interpreted as signed) |
| `uint` | 1 byte | Unsigned 8-bit integer (`0..255`) |
| `T *` | 2 bytes | 16-bit pointer/address, high byte then low byte in memory |
| `T[N]` | `N * sizeof(T)` | Contiguous array |
| `struct Name` | Sum of its fields | Packed fields, no alignment padding |
| `void` | 0 bytes | Function result / pointer target only |

Integer operations ultimately use 8-bit CPU arithmetic. Results stored in integer objects keep their low byte. Ordinary integer literals used as values must fit in 8 bits; larger literal values are accepted as pointer-cast addresses, as array indices, and by `_origin`. Pointer values are 16-bit addresses. An implicit signed `int`-to-pointer conversion sign-extends the byte; `uint`-to-pointer conversion zero-extends it. Explicit pointer-to-integer casts keep the low byte. `char` is an alias for `int`; character literals provide one byte and do not introduce a separate character type. There are no floats, long integers, unions, bit-fields, or standard library types.

Structs may contain fields and arrays of supported object types. Fields are packed in declaration order. Arrays and structs can be initialized with brace lists; omitted elements are zero-filled. A string literal is stored as its bytes followed by a zero byte. `sizeof` computes the byte size of a type or expression, then stores that result in a one-byte `uint`; sizes above 255 wrap modulo 256.

```c
struct Pixel { uint x; uint y; int shade; };
struct Pixel p = { 4, 9, -1 };
uint samples[4] = { 10, 20, 30, 40 };
uint partial[4] = { 7 };          // remaining elements are zero
char text[] = "mu" "pc";         // adjacent strings concatenate
char marker = '\x41';            // same byte as 'A' and '\101'
```

## Declarations and functions

Global variables, local variables, pointers, arrays, and comma-separated declarations are supported. Functions may return `int`, `uint`, pointer types, struct values, or `void`; parameter lists use ordinary declarations. Struct parameters pass by value, while array parameters decay to pointers. Recursion is supported through compiler-managed frames and the hardware byte stack. The parser accepts prototypes, but only function definitions produce code; the compiler gathers definitions before generating bodies, so a prior prototype is not required for a later defined function. Function pointers and parenthesized pointer-to-array declarators are unsupported; arrays of pointers are supported.

```c
uint factorial(uint n);

uint factorial(uint n) {
    if (n < 2) return 1;
    return n * factorial(n - 1);
}

int main(void) {
    uint result = factorial(5);
    return 0;
}
```

`main` may be declared `int main(void)`, `uint main(void)`, or `void main(void)`. The compiler emits the startup transfer for the entry point. A non-void function that reaches its closing brace returns zero. The program’s return value is not printed by the emulator.

## Expressions

The parser accepts decimal, hexadecimal (`0x`), and binary (`0b`) integer literals, character literals, and adjacent string literals (which concatenate). A literal from 0 through 127 has type `int`; 128 through 255 has type `uint`. Unary minus operates on the literal’s type, with `-128` recognized as a signed `int`; for example, `-129` starts with the unsigned literal `129` and wraps as an 8-bit unsigned operation. Integer suffixes such as `u` are not supported. Negative expressions include `-12`, `-0x2a`, and `-'A'`. Escapes include `\a`, `\b`, `\f`, `\n`, `\r`, `\t`, `\v`, `\\`, `\'`, `\"`, `\?`, one- or two-digit hexadecimal escapes (`\xHH`), and one- to three-digit octal escapes (`\ooo`). Character literals must decode to exactly one byte.

Supported expression operators include:

- Unary: `+ - ! ~ * & ++ -- sizeof`
- Multiplicative and additive: `* / % + -`
- Shifts and comparisons: `<< >> < > <= >= == !=`
- Bitwise/logical: `& ^ | && ||`
- Conditional: `condition ? yes : no`
- Assignment: `=`, `+=`, `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`, `>>=`
- Postfix: `++`, `--`

The usual precedence and associativity are used. Conditions treat zero as false and nonzero as true. `&&`, `||`, and the conditional operator evaluate only the selected operands. Signed division truncates toward zero, and the remainder has the dividend's sign; division by zero emits the CPU shutdown instruction. Mixed `int`/`uint` arithmetic and comparisons interpret both bytes as unsigned. Shift counts use their low three bits (modulo eight); `int` right shifts propagate the sign bit and `uint` right shifts insert zero bits. Results retain their low byte, including signed overflow.

Pointer indexing, dereference, address-of, `->`, and `.` are supported. Pointer addition/subtraction scales the integer offset by the pointed-to element size. Pointer difference is calculated from the 16-bit byte distance before division by the pointed-to element size; its final result is an 8-bit `int`, so the element count narrows to the signed byte range. Pointer-to-integer conversion requires an explicit cast and keeps the low address byte. Relational pointer comparisons use unsigned 16-bit addresses.

```c
uint data[3] = { 7, 8, 9 };
uint *p = data;
*(p + 1) = 42;
uint second = p[1];
```

## Statements

Supported statements include blocks, declarations, expression statements, `if`/`else`, `while`, `do`/`while`, `for`, `return`, `break`, `continue`, labels, and `goto`.

```c
uint i = 0;
while (i < 10) {
    if (i == 5) { i++; continue; }
    if (i == 8) break;
    i++;
}

again:
if (i) { i--; goto again; }
```

Labels have function scope. A `goto` must name a label in the same function. `break` and `continue` apply to the innermost loop.

## Preprocessor

Supported directives are `#include`, object-like and function-like `#define`, `#undef`, `#if`, `#ifdef`, `#ifndef`, `#else`, `#elif`, and `#endif`. Quoted includes first look beside the including file; `-I directory` adds include search paths. `-DNAME=value` defines a macro from the command line; `-DNAME` is exactly equivalent to `-DNAME=1`. Include cycles and excessive nesting are diagnosed.

```c
#define LIMIT 8
#define TWICE(x) ((x) + (x))

#ifdef TRACE
#define TRACE_VALUE(x) _putchar(x)
#else
#define TRACE_VALUE(x)
#endif
```

The preprocessor is intentionally small. It does not implement token pasting (`##`), stringification (`#` inside macros), variadic macros, or the complete C preprocessor grammar. Conditional expressions support integer arithmetic, comparisons, bitwise and logical operators, and `defined(NAME)`.

## Origin and inline assembly

`_origin N;` works like NASM's `org N`: it sets the address used for labels and references and adds no startup jump. The output is a flat binary whose first byte is the first instruction; load it at address `N` and begin execution there. `--origin ADDRESS` overrides the source directive. The full image must fit in the 16-bit address space.

Inline assembly uses `_asm` with either a string template or a block. The string form accepts GCC-style output and input operands:

```c
uint result;
uint value = 40;
_asm("mov a, %1\nadi a\ndb %2\nmov %0, a"
     : "=b"(result) : "a"(value), "i"(2) : "cc", "memory");
```

Outputs come first, followed by inputs; `%0`, `%1`, and so on refer to operands in that order. Use `=a`, `=b`, or `=r` for a write-only output and `+a`, `+b`, or `+r` for a read/write output. Inputs accept `a`, `b`, `r`, or `i`; `r` selects a free register from `a` and `b`, while `i` accepts a byte literal inserted into the template. Register operands and outputs must be one-byte integers, and outputs must be assignable lvalues. The optional clobber list accepts `a`, `b`, `c`, `d`, `cc`, and `memory`. The compiler does not preserve registers or analyze clobbers. `%%` inserts a literal percent sign and `%=` inserts a unique number.

The block form preserves token spacing and line breaks:

```c
_asm {
    ldi a
    db 65
    out a
    db 0
}
```

Inline assembly is raw. It can change all registers, the `c:d` address word, stack state, and control flow, so it must preserve any state needed by surrounding generated code. `_asm("out a\ndb 0")` is only meaningful if `a` contains the desired byte at that point. Keep values in source variables when crossing an assembly block. Do not write to compiler-owned scratch or frame memory.

## Built-ins

`_putchar(value)` emits `out` for the low byte of its argument. `_halt()` emits the CPU shutdown operation. `_inst(opcode, a, b)` emits an instruction byte directly and is useful for a single operation that would be verbose as an `_asm` block:

```c
uint result;
int main(void) {
    uint left = 40, right = 2;
    _inst("add", &left, &right); // loads a/b, adds, and stores a to left
    result = left;
    _halt();
}
```

`opcode` is either an assembler mnemonic string or a numeric primary opcode in `0..15`. For a two-register mnemonic, numeric `a` and `b` operands are register selectors `0..3` (`a=0`, `b=1`, `c=2`, `d=3`). A pointer operand must point to a one-byte integer: a pointer in argument `a` loads that value into register `a` before the instruction and writes register `a` back afterward; a pointer in argument `b` loads into register `b` and is input-only. The compiler preserves the other register and `c:d` state around these loads and stores. Pointer arguments are accepted as pointer expressions, including `&variable` and pointer variables.

For single-register mnemonics such as `inc`, the first operand selects the register, or binds it to `a` when it is a pointer. The second operand is zero for instructions without a payload; `ldi`, `adi`, `in`, and `out` use it as the following byte (for an immediate or I/O channel). `jmp`, `sdw`, and `rbt` require both operands to be zero. With a numeric opcode, `a` and `b` are raw two-bit instruction fields; the first field cannot be a pointer for grouped opcodes because it selects the sub-operation. The raw opcode mnemonics `idc`, `sjm`, `mst`, and `msc` also treat `a` as the sub-operation selector. The numeric form emits one byte, so use a concrete mnemonic string for instructions that need a following data byte. The raw opcode mnemonics `io` and `imm` also need a data byte and are rejected; use `in`/`out` or `ldi`/`adi`. `_inst` arguments must be literals or pointers, and pointer targets must be one-byte integers.

`_sub_inst(opcode, a)` emits a grouped sub-operation and requires `a` to point to a one-byte integer. The function loads that byte into register `a`, uses it as the sub-operation's register operand, and writes register `a` back through the pointer afterward:

```c
uint count = 41;
_sub_inst("inc", &count); // count becomes 42
```

The string form accepts these register-taking sub-operation mnemonics: `inc`, `dec`, `adc`, `sbc`, `jz`, `jnz`, `jc`, `rdm`, `wrm`, `pop`, `push`, `in`, `out`, `ldo`, `rnd`, `ldi`, and `adi`. `jmp`, `sdw`, and `rbt` do not have a register operand and are rejected. `rdm` and `wrm` use the CPU's current `c:d` address word; `push` and `pop` use the hardware stack. `in`, `out`, `ldi`, and `adi` consume a following byte, so `_sub_inst` appends a zero channel or immediate byte for them.

The numeric form accepts a grouped primary opcode from `0..15` (selecting sub-operation zero), a packed opcode selector from `16..63` (primary opcode multiplied by four, plus the sub-operation number), or an instruction-byte prefix from `64..255` whose low two register bits are zero. For example, `7` selects `inc`, `29` selects `dec`, and `0x74` also selects `dec`. The register operand is always `a`; the pointer argument and numeric opcode cannot override it. Unknown, ungrouped, or no-register operations are rejected. The pointer expression is evaluated while preserving the registers and `c:d` state the sub-operation sees.

These intrinsics need no headers. There is no general I/O library, allocator, or linker-provided runtime. Local objects and compiler temporaries have fixed RAM slots. Their storage is initialized from the image once; an uninitialized local is not re-zeroed on each function entry. A recursive re-entry into the same function reuses the same local addresses, so a pointer to an automatic object can observe the newer activation while recursion is in progress. The hardware stack saves frame bytes during calls, but it does not make those object addresses distinct. Deep recursion is limited by the emulator stack capacity.
