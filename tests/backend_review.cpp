#define main mupc_original_tests_main
#include "test.cpp"
#undef main

int main() {
  try {
    runCase("global function initializer", R"(
      uint seed(void) { return 37; }
      uint value = seed();
      int main(void) { _halt(); }
    )",
            {{"value", 37}});
    runCase("struct return and by value", R"(
      struct Pair { uint x; uint y; };
      uint result;
      struct Pair make(uint x, uint y) { struct Pair p={x,y}; return p; }
      uint sum(struct Pair p) { return p.x+p.y; }
      int main(void) { struct Pair p=make(100,155); result=sum(p); _halt(); }
    )",
            {{"result", 255}});
    runCase("large stride pointer difference", R"(
      struct Big { uint bytes[100]; };
      int positive, negative;
      int main(void) {
        struct Big a[3]; struct Big *p=&a[2]; struct Big *q=&a[0];
        positive=p-q; negative=q-p; _halt();
      }
    )",
            {{"positive", 2}, {"negative", 254}});
    {
      Image im = build(R"(
        uint *ptr;
        int main(void) { uint *p=(uint*)0x8100; p-=1; ptr=p; _halt(); }
      )");
      Machine m;
      m.run(im);
      auto lo = im.symbols.at("__global_ptr");
      auto hi = im.symbols.at("__global_ptr_hi");
      check(m.mem.at(lo) == 255 && m.mem.at(hi) == 128,
            "negative pointer offset crosses page 0x8100 to 0x80ff");
      std::cout << "[ok] negative pointer offset across high byte\n";
    }
    runCase("pointer double indirection", R"(
      uint result;
      int main(void) { uint x=19; uint *p=&x; uint **q=&p; **q=73; result=x; _halt(); }
    )",
            {{"result", 73}});
    runCase("pointer difference across signed word boundary", R"(
      struct Big { uint bytes[257]; };
      int positive, negative;
      int main(void) {
        struct Big *p=(struct Big*)0xafae;
        struct Big *q=(struct Big*)0x0100;
        positive=p-q; negative=q-p; _halt();
      }
    )",
            {{"positive", 174}, {"negative", 82}});
    std::cout << "backend review cases passed\n";
  } catch (const std::exception &e) {
    std::cerr << "backend review failure: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
