# MuPC CPU and compiler architecture

## Instruction byte

Every ordinary instruction is one byte:

```
  7 6 5 4  3 2  1 0
 +-------+----+----+
 | opcode| op1| op2|
 +-------+----+----+
```

The high nibble selects one of sixteen primary operations. Each operand is a 2-bit register number: `a=0`, `b=1`, `c=2`, `d=3`. For grouped operations, `op1` selects the sub-operation and `op2` selects the register. Thus a grouped instruction is encoded as `(opcode << 4) | (subop << 2) | register`. The assembler accepts both operands where applicable; registers unused by an instruction should be omitted.

`ldi` and `adi` read the next byte in program memory as an immediate value and advance the program counter over it. Write that payload as a separate `db` directive, for example `ldi a` followed by `db 65`. Labels are zero-size symbols. An assembly `:label` reference expands to the four bytes for `ldi c`, its high-byte payload, `ldi d`, and its low-byte payload.

## Instruction set

| Opcode | Mnemonic(s) | Operation |
|---:|---|---|
| `0x0` | `mov r1,r2` | Copy `r2` to `r1` |
| `0x1` | `add r1,r2` | `r1 = r1 + r2`; updates carry (upstream behavior is unreliable for compiler use) |
| `0x2` | `sub r1,r2` | `r1 = r1 - r2`; updates carry (unreliable for compiler use) |
| `0x3` | `and r1,r2` | Bitwise AND |
| `0x4` | `or r1,r2` | Bitwise OR |
| `0x5` | `xor r1,r2` | Bitwise XOR |
| `0x6` | `shr r1,r2` | Shift `r1` right by the count in `r2`; with the upstream signed `int8_t` registers, negative values shift arithmetically on common C compilers |
| `0x7` | `inc r`, `dec r`, `adc r`, `sbc r` | Increment, decrement, add carry, subtract carry; subops `0..3` |
| `0x8` | `jmp`, `jz r`, `jnz r`, `jc r` | Jump to address in `c:d`; unconditional, if register zero/nonzero, or if carry; subops `0..3` |
| `0x9` | `je r1,r2` | Jump to `c:d` when registers equal |
| `0xa` | `jg r1,r2` | Jump to `c:d` when register 1 is greater (the upstream emulator compares its signed 8-bit registers) |
| `0xb` | `jl r1,r2` | Jump to `c:d` when register 1 is less (signed 8-bit comparison in the upstream emulator) |
| `0xc` | `rdm r`, `wrm r`, `pop r`, `push r` | Read/write memory at `c:d`, pop/push hardware stack; subops `0..3` |
| `0xd` | `in r`, `out r` | `IN` and `OUT`; subops `0..1`. Both consume a following channel byte, which the current emulator ignores. |
| `0xe` | `ldo r`, `rnd r`, `sdw`, `rbt` | Load one, random byte, shut down, reboot; `rbt` is currently a no-op; subops `0..3` |
| `0xf` | `ldi r`, `adi r` | Load immediate byte or add immediate byte; subops `0..1`. Subops `2` and `3` are no-ops and consume no payload. |

The `je`, `jg`, and `jl` operations compare register bytes and use the shared jump address in `c:d`. They do not encode an address operand. The reference emulator stores registers as signed bytes, so `jg` and `jl` behave as signed comparisons. For unsigned relational comparisons, the compiler XORs both operands with `0x80`, then uses the signed comparison operations. `shr` uses the C right-shift of signed register values, which is arithmetic for negative values on common compilers; the compiler masks unsigned results to provide logical shifts for `uint`. The CPU has only a carry flag, no zero or sign flags. This design is one reason the compiler must stage values and addresses in RAM.


## Flags, I/O, randomness, and stack

There is one global carry flag. In the table, `signed(v)` means reinterpret byte `v` as an `int8_t` value. The reference implementation updates carry with these expressions after storing the 8-bit result:

| Operation | Reference carry update |
|---|---|
| `add x,y` | `signed(x) < signed(y)` after `x += y` wraps to a byte |
| `sub x,y` | `signed(x) > signed(y)` after `x -= y` wraps to a byte |
| `inc x` | True exactly when the result byte is zero |
| `dec x` | `signed(result) < 0`; this is not a reliable borrow test |
| `adc x` | Add the old carry, then set carry to `signed(result) < signed(memory[pc])` |
| `sbc x` | Subtract the old carry, then set carry to `signed(result) < 1` |
| `and`, `or`, `xor`, `shr` | Clear carry |

`mov`, `je`/`jg`/`jl`, jumps, memory operations, I/O, miscellaneous operations, and `ldi`/`adi` preserve carry. `adc` and `sbc` consume the old carry before replacing it. These formulas come from the emulator implementation, not conventional unsigned carry/borrow rules; general compiler code must not assume they implement ordinary arithmetic flags.

`IN` and `OUT` first advance the program counter and read one channel byte. The current emulator ignores the channel. `OUT` prints the selected register as one character with `printf("%c", ...)`. `IN` calls `scanf("%s", &registers[r])`, so it treats the register address as a string buffer and may overwrite adjacent register storage; it is unsafe for arbitrary input. The grouped encodings with sub-operation `2` or `3` still consume this channel byte and then do nothing.

`RND` writes `rand() % 256` and the upstream emulator does not seed the pseudo-random generator. `RBT` is a TODO no-op. `LDO` loads one, and `SDW` shuts the emulator down. The emulator’s hardware stack is a separate 65,536-byte array addressed by a 16-bit stack counter: `push` writes then increments, and `pop` decrements then reads. There are no overflow or underflow checks, and the counter wraps.

## Registers and address word

There are exactly four 8-bit registers. `a` and `b` are the general-purpose registers. `c` is the high byte and `d` the low byte of the 16-bit address word. `rdm` reads `memory[(c << 8) | d]` into its operand register; `wrm` writes its register to that address. Jump operations also use `c:d`. The address word is therefore shared mutable state, not two extra general-purpose registers.

The CPU has byte arithmetic and a 16-bit address space. Compiler code forms addresses by loading both address bytes, then uses `rdm`/`wrm`. High/low byte composition must treat register bytes as unsigned. The upstream emulator has paths where signed `int8_t` promotion corrupts addresses whose high or low byte is at least `0x80`; the test runner normalizes the affected composition in a temporary emulator copy.

## Compiler storage strategy

The architecture has no native stack-frame or call instruction. The compiler emits control transfers using labels and `c:d`, while variables, temporary expression values, and call-frame data are assigned fixed RAM locations. Before a call, the caller saves the callee’s current frame slots on the byte hardware stack, copies arguments into its parameter slots, pushes the return address, and jumps to the function. The callee returns through its epilogue; the caller then pops and restores the saved slots. This permits recursion without assuming extra registers. Each recursive activation of the same function reuses the same RAM addresses for locals. A pointer into one activation can therefore refer to slots subsequently overwritten by a deeper activation; the hardware stack saves slot contents but does not relocate those objects. Uninitialized locals begin with their image’s initial RAM byte and are not cleared on each call. The emulator’s push/pop hardware stack is a byte stack and is not a substitute for the compiler’s complete activation records.

The generated code reserves RAM for globals and compiler state. Avoid hard-coding RAM addresses in programs or inline assembly; use compiler-managed source variables instead. Values crossing operations are spilled to RAM. Integer-valued results, including `sizeof`, occupy one byte; a computed size above 255 wraps according to the 8-bit result type. The one-byte result follows from the language type model.

The `_inst(opcode, a, b)` intrinsic emits one encoded instruction byte. A mnemonic string uses the assembler's opcode table; a numeric opcode supplies the high nibble directly. Variable pointers in its operands are copied through the general-purpose registers, with the first pointer written back after the instruction. See the [language guide](language.md#built-ins) for operand rules, grouped mnemonics, and immediate payloads.

The `_sub_inst(opcode, a)` intrinsic emits one grouped sub-operation with register `a` bound to the byte at the supplied pointer. The compiler loads the pointed-to byte before the instruction and stores register `a` back afterward, preserving the other register and `c:d` state around address calculation. Grouped op names include `inc`/`dec`/`adc`/`sbc`, `jmp`/`jz`/`jnz`/`jc`, `rdm`/`wrm`/`pop`/`push`, `in`/`out`, `ldo`/`rnd`/`sdw`/`rbt`, and `ldi`/`adi`; operations without a register operand (`jmp`, `sdw`, and `rbt`) cannot be used by this intrinsic. The CPU encoding and numeric opcode forms are detailed in the [language guide](language.md#built-ins).

## Assembly syntax

The embedded assembler follows the same basic syntax as the upstream [`mupc-asm`](https://github.com/DanielDe8/mupc-asm), with the CPU operation definitions from [`mupc-emu`](https://github.com/danielde8/mupc-emu):

```asm
.entry
    ldi a
    db 65
    out a
    db 0       ; I/O instruction consumes this channel byte
    sdw
```

- Instructions are case-insensitive; registers are `a` through `d`.
- `.label` defines a label; `:label` loads its 16-bit address into `c:d`.
- `db` emits one byte or a quoted byte string; `dw` emits a 16-bit big-endian word; pointer objects in compiler-managed RAM store low byte first, high byte second.
- Numeric values accept decimal, `0x` hexadecimal, and `0b` binary notation.
- `org N` sets one origin offset for labels and references without adding bytes to the flat binary. Load the output at address `N`; a second `org` is an error.
- `;` begins a comment through end of line.

The assembler resolves labels in a two-pass process and rejects undefined or duplicate labels. It allows digits after the first character in labels, unlike upstream `mupc-asm`, whose lexer stops a label before digits. Its assembler escapes are `\n`, `\t`, `\r`, `\b`, `\0`, `\\`, `\'`, and `\"`. The upstream string lexer mishandles escapes: a string beginning with a backslash is rejected, while later backslashes are left undecoded; its character lexer supports only `\n`, `\t`, `\r`, and `\b`. The C++ assembler decodes its listed escapes consistently. It also emits both bytes of `dw`, applies the single `org N` origin to all addresses without padding the flat image, and calculates the high byte of a label reference with a shift by eight bits. The upstream assembler reserves two bytes for `dw` but emits one and computes the high byte by dividing the address high-byte mask by 255. Inline assembly is inserted into generated code and shares the program’s symbol namespace. Source `_origin N;` emits the equivalent `org N` directive and no reset-address jump.
