#define main mupc_original_tests_main
#include "test.cpp"
#undef main

int main() {
  try {
    runCase("audit nested struct field array and pointer alias", R"(
      struct Row { uint values[3]; };
      struct Box { struct Row rows[2]; uint tail; };
      uint result;
      uint bump(uint *p) { *p += 7; return *p; }
      int main(void) {
        struct Box b={{{1,2,3},{4,5,6}},9};
        struct Box *p=&b;
        result=bump(&p->rows[1].values[2])+b.rows[0].values[1]+b.tail;
        halt();
      }
    )", {{"result", 24}});
    runCase("audit call results in compound expressions", R"(
      uint result;
      uint f(uint x) { return x*3+1; }
      uint g(uint x) { return x+5; }
      int main(void) { result=(f(7)+g(9))*2; halt(); }
    )", {{"result", 72}});
    runCase("audit pointer casts and signed extension", R"(
      uint result, eq, cv;
      int main(void) {
        int neg=-1; uint *p=(uint*)neg; uint *q=(uint*)255;
        eq=(p==q); cv=(uint)(int)0x80; result=eq+(cv==128); halt();
      }
    )", {{"result", 1}, {"eq", 0}, {"cv", 128}});
    runCase("audit void and scalar returns", R"(
      uint result;
      void set(uint *p) { *p=11; }
      int value(void) { return -4; }
      int main(void) { uint x=2; set(&x); result=x+(uint)value(); halt(); }
    )", {{"result", 7}});
    runCase("audit recursive pointer mutation and globals", R"(
      uint total;
      uint rec(uint *p, uint n) {
        if(n==0) return *p;
        *p += n; total += 1; return rec(p,n-1)+1;
      }
      int main(void) { uint x=1; total=0; uint z=rec(&x,4); total+=x+z; halt(); }
    )", {{"total", 30}});
    runCase("audit conditional pointer and struct values", R"(
      struct Pair { uint x; uint y; };
      uint result;
      struct Pair make(uint x) { struct Pair p={x,x+1}; return p; }
      int main(void) {
        struct Pair a=make(8), b=make(20);
        uint *p=1 ? &a.y : &b.x;
        struct Pair c=(0 ? a : b);
        *p += c.x;
        result=a.y+c.y;
        halt();
      }
    )", {{"result", 50}});
    runCase("audit loops with pointer postincrement", R"(
      uint result;
      int main(void) {
        uint a[4]={3,5,7,11}; uint *p=a; uint sum=0;
        for(uint i=0;i<4;i++) sum+=*p++;
        result=sum; halt();
      }
    )", {{"result", 26}});
  } catch (const std::exception &e) {
    std::cerr << "backend audit failure: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
