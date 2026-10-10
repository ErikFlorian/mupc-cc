#include "mupc/compiler.hpp"
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

using namespace mupc;
namespace {
void check(bool ok, const std::string &what) {
  if (!ok)
    throw std::runtime_error(what);
}
const std::filesystem::path &temporaryRoot() {
  struct Files {
    std::filesystem::path path;
    Files() {
      auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      path = std::filesystem::temp_directory_path() /
             ("mupc-tests-" + std::to_string(stamp) + "-" +
              std::to_string(std::random_device{}()));
      std::filesystem::create_directories(path);
    }
    ~Files() {
      std::error_code ec;
      std::filesystem::remove_all(path, ec);
    }
  };
  static Files files;
  return files.path;
}
std::filesystem::path emittedDir() {
  const char *p = std::getenv("MUPC_TEST_IMAGE_DIR");
  return p && *p ? std::filesystem::path(p) : std::filesystem::path();
}
void emitImage(const std::string &name, const Image &im,
               const std::vector<std::pair<std::string, uint8_t>> &checks = {},
               const std::string &output = {}) {
  auto dir = emittedDir();
  if (dir.empty())
    return;
  std::filesystem::create_directories(dir);
  auto image = dir / (name + ".bin");
  {
    std::ofstream f(image, std::ios::binary);
    f.write((const char *)im.bytes.data(), (std::streamsize)im.bytes.size());
  }
  {
    std::ofstream f(dir / (name + ".expect"));
    for (const auto &c : checks) {
      auto it = im.symbols.find("__global_" + c.first);
      if (it == im.symbols.end())
        throw std::runtime_error("missing emitted expected symbol: " + c.first);
      f << "0x" << std::hex << it->second << " " << std::dec
        << unsigned(c.second) << "\n";
    }
  }
  {
    std::ofstream f(dir / (name + ".output"));
    f << output;
  }
  {
    std::ofstream f(dir / "manifest", std::ios::app);
    f << name << "\n";
  }
}
struct Machine {
  std::array<uint8_t, 65536> mem{};
  std::array<uint8_t, 4> r{};
  std::array<uint8_t, 65536> stack{};
  uint16_t pc = 0, sp = 0, target = 0;
  bool carry = false, halted = false;
  std::string output;
  static int8_t s8(uint8_t x) { return static_cast<int8_t>(x); }
  uint16_t address() const { return uint16_t((uint16_t(r[2]) << 8) | r[3]); }
  void run(const Image &im, uint32_t limit = 2000000) {
    check(im.bytes.size() <= mem.size(), "image exceeds 64K");
    std::copy(im.bytes.begin(), im.bytes.end(), mem.begin());
    for (uint32_t step = 0; step < limit && !halted; ++step) {
      uint8_t ins = mem[pc], op = ins >> 4, a = (ins >> 2) & 3, b = ins & 3;
      auto jump = [&] { target = address(); };
      switch (op) {
      case 0:
        r[a] = r[b];
        break;
      case 1: {
        uint16_t v = uint16_t(r[a]) + r[b];
        r[a] = uint8_t(v);
        carry = v > 255;
        break;
      }
      case 2: {
        uint8_t x = r[a];
        r[a] = uint8_t(x - r[b]);
        carry = x < r[b];
        break;
      }
      case 3:
        r[a] &= r[b];
        carry = false;
        break;
      case 4:
        r[a] |= r[b];
        carry = false;
        break;
      case 5:
        r[a] ^= r[b];
        carry = false;
        break;
      case 6:
        r[a] = uint8_t(s8(r[a]) >> r[b]);
        carry = false;
        break;
      case 7:
        if (a == 0) {
          ++r[b];
          carry = r[b] == 0;
        } else if (a == 1) {
          --r[b];
          carry = r[b] == 255;
        } else if (a == 2) {
          uint16_t v = uint16_t(r[b]) + carry;
          r[b] = uint8_t(v);
          carry = v > 255;
        } else {
          uint8_t x = r[b];
          r[b] = uint8_t(x - carry);
          carry = x < carry;
        }
        break;
      case 8:
        if (a == 0) {
          jump();
        } else if (a == 1 && r[b] == 0) {
          jump();
        } else if (a == 2 && r[b] != 0) {
          jump();
        } else if (a == 3 && carry) {
          jump();
        }
        break;
      case 9:
        if (r[a] == r[b])
          jump();
        break;
      case 10:
        if (s8(r[a]) > s8(r[b]))
          jump();
        break;
      case 11:
        if (s8(r[a]) < s8(r[b]))
          jump();
        break;
      case 12:
        if (a == 0)
          r[b] = mem[address()];
        else if (a == 1)
          mem[address()] = r[b];
        else if (a == 2)
          r[b] = stack[--sp];
        else
          stack[sp++] = r[b];
        break;
      case 13: {
        uint8_t channel = mem[++pc];
        (void)channel;
        if (a == 1)
          output.push_back(char(r[b]));
        break;
      }
      case 14:
        if (a == 0)
          r[b] = 1;
        else if (a == 2)
          halted = true;
        break;
      case 15:
        if (a == 0)
          r[b] = mem[++pc];
        else if (a == 1)
          r[b] = uint8_t(r[b] + mem[++pc]);
        break;
      }
      if (halted)
        break;
      if ((op == 8 && (a == 0 || (a == 1 && r[b] == 0) ||
                       (a == 2 && r[b] != 0) || (a == 3 && carry))) ||
          (op == 9 && r[a] == r[b]) || (op == 10 && s8(r[a]) > s8(r[b])) ||
          (op == 11 && s8(r[a]) < s8(r[b])))
        pc = target;
      else
        ++pc;
    }
    check(halted, "program exceeded instruction budget or did not shut down");
  }
  uint8_t global(const Image &im, const std::string &n) const {
    auto it = im.symbols.find("__global_" + n);
    check(it != im.symbols.end(), "missing global symbol " + n);
    return mem.at(uint16_t(it->second));
  }
};

Image build(const std::string &src) {
  static unsigned n = 0;
  auto p = temporaryRoot() / ("mupc-test-" + std::to_string(++n) + ".c");
  {
    std::ofstream f(p);
    f << src;
  }
  try {
    auto tokens = preprocess(p);
    auto unit = parse(tokens);
    auto result = compile(unit);
    std::filesystem::remove(p);
    return result.image;
  } catch (...) {
    std::filesystem::remove(p);
    throw;
  }
}
void preprocessingTest() {
  auto dir = temporaryRoot() / "include";
  std::filesystem::create_directories(dir);
  auto h = dir / "values.h", p = dir / "include.c";
  auto a = dir / "a.h", b = dir / "b.h";
  {
    std::ofstream f(h);
    f << "#ifndef VALUES_H\n#define VALUES_H\n#ifndef OMIT_VALUE\n#define "
         "INCLUDED_VALUE 37\n#else\n#define INCLUDED_VALUE 9\n#endif\n#if "
         "INCLUDED_VALUE * 2 == 74\n#define EXPRESSION_OK 1\n#endif\n#endif\n";
  }
  {
    std::ofstream f(a);
    f << "#ifndef A_H\n#define A_H\n#include \"b.h\"\nint from_a;\n#endif\n";
  }
  {
    std::ofstream f(b);
    f << "#ifndef B_H\n#define B_H\n#include \"a.h\"\nint from_b;\n#endif\n";
  }
  {
    std::ofstream f(p);
    f << "#include \"values.h\"\n#include \"a.h\"\nint included = "
         "INCLUDED_VALUE + EXPRESSION_OK;\n";
  }
  try {
    auto t = preprocess(p);
    auto u = parse(t);
    bool saw37 = false, saw1 = false;
    for (const auto &token : t) {
      saw37 |= token.text == "37";
      saw1 |= token.text == "1";
    }
    check(u.globals.size() == 3 && u.globals[0].name == "from_b" &&
              u.globals[1].name == "from_a" &&
              u.globals[2].name == "included" && u.globals[2].init && saw37 &&
              saw1,
          "quoted include, guarded cycle, nested conditionals and #if "
          "expression");
    auto c1 = dir / "cycle1.c", c2 = dir / "cycle2.h";
    {
      std::ofstream f(c1);
      f << "#include \"cycle2.h\"\n";
    }
    {
      std::ofstream f(c2);
      f << "#include \"cycle1.c\"\n";
    }
    bool rejected = false;
    try {
      (void)preprocess(c1);
    } catch (const std::exception &) {
      rejected = true;
    }
    check(rejected, "unguarded include cycle rejection");
  } catch (...) {
    std::filesystem::remove_all(dir);
    throw;
  }
  std::filesystem::remove_all(dir);
  std::cout << "[ok] nested #ifndef/#else, #if expression, and guarded include "
               "cycle\n";
}
void runCase(const std::string &name, const std::string &src,
             const std::vector<std::pair<std::string, uint8_t>> &checks) {
  Image im = build(src);
  Machine m;
  m.run(im);
  for (const auto &c : checks)
    check(m.global(im, c.first) == c.second, name + ": " + c.first);
  std::string slug = name;
  for (char &c : slug)
    if (!std::isalnum((unsigned char)c))
      c = '_';
  emitImage(slug, im, checks);
  std::cout << "[ok] " << name << "\n";
}
void focusedTests() {
  {
    Image im = build("int main(){ _putchar('O'); _putchar('K'); _halt(); }");
    Machine m;
    m.run(im);
    check(m.output == "OK", "_putchar output stream");
    emitImage("output_capture", im, {}, "OK");
    std::cout << "[ok] output capture\n";
  }
  runCase("signed division and remainder", R"(
int quotient, remainder, result;
int main(){ int n=-17; quotient=n/5; remainder=n%5; result=quotient*10+remainder; _halt(); }
)",
          {{"quotient", 253}, {"remainder", 254}, {"result", 224}});
  runCase("signed and unsigned shifts", R"(
int signed_result; uint unsigned_result;
int main(){ int s=-8; uint u=128; signed_result=s>>1; unsigned_result=u>>1; _halt(); }
)",
          {{"signed_result", 252}, {"unsigned_result", 64}});
  runCase("comparisons and 16-bit pointers", R"(
int flags, signed_equalities, unsigned_flags; int data[260];
int main(){ int a=-2,b=3; int *p=&data[0], *q=&data[256];
 flags=(a<b)+2*(a<=b)+4*(b>a)+8*(b>=a)+16*(p!=q)+32*(p==&data[0])+
       64*(q>p)+128*(p<q); signed_equalities=(a!=b)+2*(a==a);
 uint u=250,v=2; unsigned_flags=(u>v)+2*(u>=v)+4*(v<u)+8*(v<=u)+16*(u!=v)+32*(u==250); _halt(); }
)",
          {{"flags", 255}, {"signed_equalities", 3}, {"unsigned_flags", 63}});
  runCase("short-circuit side effects", R"(
int calls, result;
int bump(){ calls++; return 1; }
int main(){ if(0 && bump()) result=99; if(1 || bump()) result+=1;
 if(1 && bump()) result+=2; if(0 || bump()) result+=4; _halt(); }
)",
          {{"calls", 2}, {"result", 7}});
  runCase("nested do-while break and continue", R"(
int result;
int main(){ int i=0,j=0; do { i++; if(i==2) continue; j=0;
 do { j++; if(j==2) break; result++; } while(j<4); if(i==4) break;
 result+=10; } while(i<9); _halt(); }
)",
          {{"result", 23}});
  runCase("pointer and struct strides with postincrement", R"(
struct Item { uint a; int b; };
int result;
int main(){ struct Item items[3]={{1,4},{2,5},{3,6}}; struct Item *p=items;
 result=p++->a; result+=p->b; result+=(p+1)->a; result+=items[2].b;
 p+=1; p--; result+=p->a; int nums[3]={7,8,9}; int *n=nums;
 result+=*n++; n+=1; result+=*n; _halt(); }
)",
          {{"result", 33}});
  runCase("nested recursive calls preserve expression temporaries", R"(
int result;
int fib(int n){ if(n<2)return n; return fib(n-1)+fib(n-2); }
int mix(int a,int b){return a*3+b;}
int main(){ result=mix(fib(6),mix(2,fib(4))); _halt(); }
)",
          {{"result", 33}});
  runCase("pointer returns, null compare, sizeof does not evaluate", R"(
int result, calls, data[2];
int bump(){calls++;return 7;}
int *identity(int *p){return p;}
int main(){ int *p=identity(&data[1]); result=(p!=0)+2*(p==&data[1]);
 result+=sizeof(bump())+sizeof(data); _halt(); }
)",
          {{"result", 6}, {"calls", 0}});
  runCase("nested partial multidimensional array initialization", R"(
int result;
int main(){ int a[2][2]={{1},{2}};
 result=a[0][0]+2*a[0][1]+4*a[1][0]+8*a[1][1]+sizeof(int[2+3]); _halt(); }
)",
          {{"result", 14}});
  runCase("high byte character and adjacent strings", R"(
char joined[]="A" "B"; int result, signed_char;
int main(){ signed_char=('\x80'<0); result=joined[0]+joined[1]+joined[2]; _halt(); }
)",
          {{"result", 131}, {"signed_char", 1}});
  runCase("array of pointers", R"(
int result;
int main(){ int x=5,y=7; int *ptrs[2]; ptrs[0]=&x; ptrs[1]=&y;
 result=*ptrs[0]+*ptrs[1]; _halt(); }
)",
          {{"result", 12}});
  runCase("nested partial struct field array initialization", R"(
struct Grid { int cell[2][2]; int tail; }; int result;
int main(){ struct Grid g={{{3},{4}},5};
 result=g.cell[0][0]+2*g.cell[0][1]+4*g.cell[1][0]+8*g.cell[1][1]+g.tail; _halt(); }
)",
          {{"result", 24}});
  // Alternate origin forces a 16-bit start and validates the assembler's
  // startup jump as well as the compiler's high-byte address handling.
  runCase("nonzero origin", R"(
origin 0x1234;
int result;
int main(){ result=73; _halt(); }
)",
          {{"result", 73}});
}
void failureTests() {
  const std::vector<std::string> bad = {
      "int main( { _halt(); }",                   // malformed declaration
      "int main(){ missing=1; _halt(); }",        // unknown identifier
      "int main(){ int x; x=\"bad\"; _halt(); }", // incompatible types
      "struct Node { struct Node next; }; int main(){_halt();}", // by-value self
                                                                // recursion
      "int huge[256][256]; int main(){_halt();}", // exceeds addressable object
                                                 // size
      "int (*ptrs)[2]; int main(){_halt();}",     // unsupported pointer-to-array
                                                 // declarator
      "struct Missing object; int main(){_halt();}",        // incomplete object
      "int f(int x){int x;return 0;} int main(){_halt();}", // parameter
                                                           // redeclaration
  };
  for (size_t i = 0; i < bad.size(); ++i) {
    bool failed = false;
    try {
      (void)build(bad[i]);
    } catch (const std::exception &) {
      failed = true;
    }
    check(failed,
          "invalid input case " + std::to_string(i) + " must be rejected");
  }
  std::cout << "[ok] malformed syntax, unknown name, and incompatible type "
               "diagnostics\n";
}
} // namespace

int main() {
  try {
    if (auto dir = emittedDir(); !dir.empty()) {
      std::filesystem::remove_all(dir);
      std::filesystem::create_directories(dir);
    }
    preprocessingTest();
    focusedTests();
    failureTests();
    // Exercises preprocessing, declarations, 8-bit signed/unsigned arithmetic,
    // call frames, recursion, control flow, arrays, structs, pointers and goto.
    // Intermediate arithmetic must survive calls because only r0/r1 are free.
    const char *program = R"(
origin 0;
#define TWICE(x) ((x) * 2)
#define ENABLED 1
#ifdef ENABLED
#define BASE 0x10
#else
#define BASE 0
#endif
struct Pair { int left; uint right; };
int result; int recursion_result; int array_result; int pointer_result;
int arithmetic_result; int flow_result; int literal_result; int struct_result; int wide_result;
int wide[260];
int fact(int n) { if (n <= 1) return 1; return n * fact(n - 1); }
int sum(int a, int b) { return a + b; }
int main() {
  asm("mov a, a");
  int x = 0b00000111; uint u = 0xff; int neg = -3;
  int values[4] = {1,2,3,4}; struct Pair p = {5,6};
  int *q = &values[0];
  wide[255] = 41; wide[256] = 42; wide_result = wide[255] + wide[256];
  result = sum(TWICE(BASE), 2);
  recursion_result = fact(5);
  array_result = values[0] + values[1] + values[2] + values[3];
  pointer_result = *(q + 2);
  arithmetic_result = ((u * 3) / 5) % 7 + (neg < 0);
  struct_result = p.left + p.right;
  flow_result = 0;
  for (int i=0; i<5; i++) { if (i == 3) continue; flow_result += i; }
  if (flow_result == 7) flow_result += 1; else flow_result = 99;
  literal_result = 'A' + ("B"[0] - 'A');
  goto done; flow_result = 99; done:
  _halt();
}
)";
    Image im = build(program);
    Machine m;
    m.run(im);
    check(m.global(im, "result") == 34, "macro/hex/binary/call result");
    check(m.global(im, "recursion_result") == 120, "recursive call result");
    check(m.global(im, "array_result") == 10, "array initializer/index result");
    check(m.global(im, "pointer_result") == 3,
          "pointer arithmetic/dereference result");
    check(m.global(im, "arithmetic_result") == 2,
          "unsigned multiply/divide/modulo and signed comparison result");
    check(m.global(im, "struct_result") == 11, "struct fields result");
    check(m.global(im, "wide_result") == 83,
          "array accesses across a 256-byte address page");
    check(m.global(im, "flow_result") == 8, "for/continue/if/goto result");
    check(m.global(im, "literal_result") == 66,
          "character and string literal result");
    emitImage("integrated_fixture", im,
              {{"result", 34},
               {"recursion_result", 120},
               {"array_result", 10},
               {"pointer_result", 3},
               {"arithmetic_result", 2},
               {"struct_result", 11},
               {"wide_result", 83},
               {"flow_result", 8},
               {"literal_result", 66}});
    std::cout << "[ok] integrated language fixture\n";
    std::cout << "mupc compiler tests passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "test failure: " << e.what() << "\n";
    return 1;
  }
}
