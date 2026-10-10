// Bounded runner for the upstream mupc-emu operations table.  The companion
// script compiles a temporary copy of operations.c with only SDW changed into
// a flag, so malformed compiler output cannot call exit() or spin forever.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>
extern "C" {
extern void (*operations[16])(uint8_t, uint8_t);
int8_t memory[0x10000];
int8_t registers[4];
int8_t stack[0x10000];
uint16_t pc = 0, sc = 0, jump_address = 0;
bool carry_f = false, jump_f = false;
extern bool upstream_halted;
}
int main(int argc, char **argv) {
  if (argc < 2 || (argc - 2) % 3 != 0) {
    std::cerr << "usage: upstream-run IMAGE [--expect ADDRESS VALUE]...\n";
    return 2;
  }
  std::memset(memory, 0, 0x10000);
  std::memset(registers, 0, 4);
  std::memset(stack, 0, 0x10000);
  pc = sc = jump_address = 0;
  carry_f = jump_f = upstream_halted = false;
  FILE *f = std::fopen(argv[1], "rb");
  if (!f) {
    std::perror(argv[1]);
    return 2;
  }
  for (int i = 0, c; i < 0x10000 && (c = std::fgetc(f)) != EOF; ++i)
    memory[i] = (uint8_t)c;
  std::fclose(f);
  for (uint32_t step = 0; step < 2000000 && !upstream_halted; ++step) {
    const uint8_t ins = (uint8_t)memory[pc];
    operations[(ins >> 4) & 15]((ins >> 2) & 3, ins & 3);
    if (upstream_halted)
      break;
    if (jump_f) {
      pc = jump_address;
      jump_f = false;
    } else
      ++pc;
  }
  if (!upstream_halted) {
    std::cerr << "upstream emulator did not halt (pc=" << pc << ")\n";
    return 3;
  }
  for (int i = 2; i < argc; i += 3) {
    if (std::string(argv[i]) != "--expect") {
      std::cerr << "unknown option: " << argv[i] << "\n";
      return 2;
    }
    char *end = nullptr;
    unsigned long address = std::strtoul(argv[i + 1], &end, 0);
    if (!end || *end || address > 65535) {
      std::cerr << "invalid expected address\n";
      return 2;
    }
    long expected = std::strtol(argv[i + 2], &end, 0);
    if (!end || *end || expected < -128 || expected > 255) {
      std::cerr << "invalid expected byte\n";
      return 2;
    }
    if ((uint8_t)memory[address] != (uint8_t)expected) {
      std::cerr << "memory[0x" << std::hex << address << "] expected 0x"
                << (uint8_t)expected << " got 0x" << (uint8_t)memory[address]
                << "\n";
      return 4;
    }
  }
  return 0;
}
