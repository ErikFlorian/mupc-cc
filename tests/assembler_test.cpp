#include "mupc/assembler.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const std::string &description) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
  }
}

void same(const std::vector<std::uint8_t> &got,
          const std::vector<std::uint8_t> &expected,
          const std::string &description) {
  if (got == expected)
    return;
  ++failures;
  std::cerr << "FAIL: " << description << " (got";
  for (auto b : got)
    std::cerr << " " << std::hex << static_cast<unsigned>(b);
  std::cerr << "; expected";
  for (auto b : expected)
    std::cerr << " " << std::hex << static_cast<unsigned>(b);
  std::cerr << std::dec << ")\n";
}

void rejects(const std::string &source, const std::string &description) {
  try {
    (void)mupc::assemble(source);
    check(false, description + " should fail");
  } catch (const std::exception &) {
  }
}

unsigned reg(const char *name) {
  if (name[0] == 'a')
    return 0;
  if (name[0] == 'b')
    return 1;
  if (name[0] == 'c')
    return 2;
  return 3;
}

void instruction_encodings() {
  struct Op {
    const char *name;
    unsigned opcode;
  };
  constexpr Op regular[] = {
      {"mov", 0x0}, {"add", 0x1}, {"sub", 0x2}, {"and", 0x3},
      {"or", 0x4},  {"xor", 0x5}, {"shr", 0x6}, {"idc", 0x7},
      {"sjm", 0x8}, {"je", 0x9},  {"jg", 0xa},  {"jl", 0xb},
      {"mst", 0xc}, {"io", 0xd},  {"msc", 0xe}, {"imm", 0xf},
  };
  constexpr const char *regs[] = {"a", "b", "c", "d"};
  for (const auto &op : regular) {
    for (const auto *r1 : regs) {
      for (const auto *r2 : regs) {
        const std::string source = std::string(op.name) + " " + r1 + "," + r2;
        const auto got = mupc::assemble(source).bytes;
        const auto expected = static_cast<std::uint8_t>(
            (op.opcode << 4) | (reg(r1) << 2) | reg(r2));
        same(got, {expected}, source);
      }
    }
  }

  constexpr Op supers[] = {
      {"inc", 0x70}, {"dec", 0x74}, {"adc", 0x78}, {"sbc", 0x7c},
      {"jmp", 0x80}, {"jz", 0x84},  {"jnz", 0x88}, {"jc", 0x8c},
      {"rdm", 0xc0}, {"wrm", 0xc4}, {"pop", 0xc8}, {"push", 0xcc},
      {"in", 0xd0},  {"out", 0xd4}, {"ldo", 0xe0}, {"rnd", 0xe4},
      {"sdw", 0xe8}, {"rbt", 0xec}, {"ldi", 0xf0}, {"adi", 0xf4},
  };
  for (const auto &op : supers) {
    for (unsigned target = 0; target < 4; ++target) {
      const std::string source = std::string(op.name) + " " + regs[target];
      same(mupc::assemble(source).bytes,
           {static_cast<std::uint8_t>(op.opcode | target)}, source);
    }
  }

  constexpr Op registerSubs[] = {
      {"inc", 0x70}, {"dec", 0x74}, {"adc", 0x78}, {"sbc", 0x7c},
      {"jz", 0x84},  {"jnz", 0x88}, {"jc", 0x8c},  {"rdm", 0xc0},
      {"wrm", 0xc4}, {"pop", 0xc8}, {"push", 0xcc}, {"in", 0xd0},
      {"out", 0xd4}, {"ldo", 0xe0}, {"rnd", 0xe4}, {"ldi", 0xf0},
      {"adi", 0xf4},
  };
  for (const auto &op : registerSubs) {
    const auto got = mupc::encodeSubInstruction(op.name, 0);
    same({got}, {static_cast<std::uint8_t>(op.opcode)},
         std::string("sub-operation encoding for ") + op.name);
    same({mupc::encodeSubInstruction(op.name, 3)},
         {static_cast<std::uint8_t>(op.opcode | 3)},
         std::string("sub-operation register field for ") + op.name);
  }
  bool rejected = false;
  try {
    (void)mupc::encodeSubInstruction("add", 0);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, "primary operation is rejected by sub-operation encoder");

  same(mupc::assemble("MOV d,c\nINC b\nJNZ d\n").bytes, {0x0e, 0x71, 0x8b},
       "mixed case and multi-line instructions");
  same(mupc::assemble("ldi a\ndb 65\nadi b\ndb 2\n").bytes, {0xf0, 65, 0xf5, 2},
       "immediate operations consume a separate data byte");
}

void data_and_origins() {
  same(mupc::assemble("db 0\ndb 255\ndb 0b10101010\ndb 0xff\n").bytes,
       {0x00, 0xff, 0xaa, 0xff}, "decimal, binary, and hexadecimal db");
  same(mupc::assemble("db 'A'\ndb '\\n'\ndb \"Hi\\0!\"\n").bytes,
       {0x41, 0x0a, 0x48, 0x69, 0x00, 0x21}, "character/string escapes");
  same(mupc::assemble("dw 0x1234\ndw 0b101\n").bytes, {0x12, 0x34, 0x00, 0x05},
       "16-bit words");

  const auto image = mupc::assemble("org 4\n.start\ndb 0x11\n.end\ndb 0x22\n");
  same(image.bytes, {0x11, 0x22}, "org does not add bytes to the flat image");
  check(image.loadAddress == 4, "org sets the flat image load address");
  check(image.symbols.at("start") == 4, "label after org has absolute address");
  check(image.symbols.at("end") == 5, "org offsets subsequent labels");
}

void labels_and_fixups() {
  const auto image =
      mupc::assemble("org 0xfffa\n:target\n.target\nsdw\n");
  same({image.bytes[0], image.bytes[1], image.bytes[2], image.bytes[3]},
       {0xf2, 0xff, 0xf3, 0xfe},
       "high-page label fixup includes address page ff");
  check(image.symbols.at("target") == 0xfffe, "upper-page symbol address");
  check(image.bytes.size() == 5,
        "upper-page origin does not pad the flat image");
  check(image.bytes[4] == 0xe8, "sdw follows the origin in the flat image");
  check(image.loadAddress == 0xfffa, "high-page org sets the load address");

  const auto digit_name = mupc::assemble(":__tmp_42\n.__tmp_42\ndb 7\n");
  same(digit_name.bytes, {0xf2, 0x00, 0xf3, 0x04, 7},
       "generated labels allow digits and underscores");
  check(digit_name.symbols.at("__tmp_42") == 4, "generated label symbol value");
}

void bad_inputs() {
  rejects("db 256\n", "byte data overflow");
  rejects("db 0x100\n", "hex byte overflow");
  rejects("dw 65536\n", "word data overflow");
  rejects("org 65536\n", "origin overflow");
  rejects("org 2\ndb 1\norg 3\n", "repeated org");
  rejects(".again\n.again\n", "duplicate labels");
  rejects(":missing\n", "undefined labels");
  rejects("mov a,e\n", "invalid register");
  rejects("ldi a,65\n", "immediate operands must be separate data bytes");
  rejects("db 'ab'\n", "multi-byte character literal");
  rejects("org 65535\ndb 1\ndb 2\n", "image address-space overflow");
  rejects("org 65535\ndb 1\n.past_end\n", "label beyond address-space end");
}
} // namespace

int main() {
  instruction_encodings();
  data_and_origins();
  labels_and_fixups();
  bad_inputs();
  if (failures) {
    std::cerr << failures << " assembler test(s) failed\n";
    return 1;
  }
  std::cout << "Assembler tests passed\n";
  return 0;
}
