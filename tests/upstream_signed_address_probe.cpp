/*
 * Probe the unmodified upstream emulator's signed-byte address behavior.
 * Build this against mupc-emu's operations.c and headers. For c:d = 0x0080,
 * the upstream expression (registers[2] << 8) | registers[3] sign-extends the
 * int8_t low byte and stores 0xff80 in jump_address instead of 0x0080.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

extern "C" {
#include "main.h"
#include "operations.h"
}

int8_t memory[0x10000];
int8_t registers[4];
int8_t stack[0x10000];
uint16_t pc, sc, jump_address;
bool carry_f, jump_f;

int main(void) {
  registers[2] = 0;
  registers[3] = (int8_t)0x80;
  operations[8](0, 0); /* unconditional SJM/JMP using c:d */
  printf("c:d=0080 produced jump_address=%04x\n", jump_address);
  return jump_address == 0xff80 ? 0 : 1;
}
