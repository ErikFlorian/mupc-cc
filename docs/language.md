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

Integer operations ultimately use 8-bit CPU arithmetic. Results stored in integer objects keep their low byte. Ordinary integer literals used as values must fit in 8 bits; larger literal values are accepted as pointer-cast addresses, as array indices, and by `origin`. Pointer values are 16-bit addresses. An implicit signed `int`-to-pointer conversion sign-extends the byte; `uint`-to-pointer conversion zero-extends it. Explicit pointer-to-integer casts keep the low byte. `char` is an alias for `int`; character literals provide one byte and do not introduce a separate character type. There are no floats, long integers, unions, bit-fields, or standard library types.

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
#define TRACE_VALUE(x) putchar(x)
#else
#define TRACE_VALUE(x)
#endif
```

The preprocessor is intentionally small. It does not implement token pasting (`##`), stringification (`#` inside macros), variadic macros, or the complete C preprocessor grammar. Conditional expressions support integer arithmetic, comparisons, bitwise and logical operators, and `defined(NAME)`.

## Origin and inline assembly

`origin N;` sets the C program body origin. At origin zero the program begins at address zero without a bootstrap jump. For a nonzero origin, generated startup code at address zero jumps to the body; the origin must be at least five so the bootstrap jump fits, and the full image must fit in the 16-bit address space.

Inline assembly accepts a string or a block. Use the string form for example `asm("ldi a\ndb 65\nout a\ndb 0");`. Block form preserves token spacing and line breaks. Assembly mnemonics are case-insensitive; registers are `a`, `b`, `c`, `d`. Assembly labels are `.name`, references are `:name`, data directives are `db` and `dw`, and `org` changes the assembly cursor. A semicolon starts an assembly comment.

```c
asm {
    ldi a
    db 65
    out a
    db 0
}
```

Inline assembly is raw. It can change all registers, the `c:d` address word, stack state, and control flow, so it must preserve any state needed by surrounding generated code. `asm("out a\ndb 0")` is only meaningful if `a` contains the desired byte at that point. The compiler does not perform register-clobber analysis or preserve registers around inline assembly; keep values in source variables when crossing an assembly block. Do not write to compiler-owned scratch or frame memory.

## Built-ins

`putchar(value)` emits `out` for the low byte of its argument. `halt()` emits the CPU shutdown operation. These are the compiler’s built-in intrinsics and need no headers. There is no general I/O library, allocator, or linker-provided runtime. Local objects and compiler temporaries have fixed RAM slots. Their storage is initialized from the image once; an uninitialized local is not re-zeroed on each function entry. A recursive re-entry into the same function reuses the same local addresses, so a pointer to an automatic object can observe the newer activation while recursion is in progress. The hardware stack saves frame bytes during calls, but it does not make those object addresses distinct. Deep recursion is limited by the emulator stack capacity.
