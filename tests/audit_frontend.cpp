// Standalone frontend audit. Reuse the test harness without its existing main.
#define main mupc_existing_tests_main
#include "test.cpp"
#undef main

namespace {
void expectFrontendGap(const std::string &name, const std::string &source) {
  bool rejected = false;
  try {
    (void)build(source);
  } catch (const std::exception &) {
    rejected = true;
  }
  check(rejected, name + " unexpectedly started failing to reproduce");
  std::cout << "[gap confirmed] " << name << '\n';
}

void auditMacroExpansionAndExpressionGrammar() {
  runCase("nested function macros and conditional compilation", R"(
#define ADD(x,y) ((x)+(y))
#define ID(x) x
#define TWICE(x) ADD(ID(x),ID(x))
#define ENABLED 1
#if defined(ENABLED) && ENABLED
#define VALUE TWICE(0x10)
#else
#define VALUE 0
#endif
int result;
int main(){ result=VALUE; halt(); }
)", {{"result", 32}});

  expectFrontendGap("comma operator in expressions", R"(
int main(){ int x; x=(1,2); halt(); return x; }
)");
  expectFrontendGap("self-referential macro suppression", R"(
#define SELF SELF
int main(){ int x=SELF; halt(); return x; }
)");
}

void auditPrototypeConsistencyAndScopes() {
  // Prototypes currently disappear during parsing. This source therefore
  // compiles despite the declaration and definition disagreeing on signedness.
  auto im = build(R"(
int f(uint x);
int f(int x){ return x; }
int main(){ halt(); return f(1); }
)");
  check(!im.bytes.empty(), "mismatched prototype probe did not compile");
  std::cout << "[gap confirmed] mismatched function prototype is ignored\n";

  runCase("nested block shadowing and precedence", R"(
int result;
int main(){ int x=3; { int x=9; result=x; } result += x + 2*3 << 1; halt(); }
)", {{"result", 27}});
}
} // namespace

int main() {
  try {
    auditMacroExpansionAndExpressionGrammar();
    auditPrototypeConsistencyAndScopes();
    std::cout << "frontend audit probes passed\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "frontend audit failed: " << e.what() << '\n';
    return 1;
  }
}
