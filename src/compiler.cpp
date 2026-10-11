#include "mupc/compiler.hpp"
#include <algorithm>
#include <cctype>
#include <functional>
#include <limits>
#include <set>
#include <sstream>
namespace mupc {
namespace {
bool numeric(Ty t) { return t->kind == Type::Int || t->kind == Type::UInt; }
Ty decay(Ty t) { return t->kind == Type::Array ? pointer(t->base) : t; }
struct Value {
  Ty type;
  std::vector<std::string> cells;
};
struct Place {
  Ty type;
  Value address;
};
struct Frame {
  const Function *fn = nullptr;
  std::vector<Value> params;
  std::vector<std::string> cells;
};
struct CallSite {
  std::string fn, label;
  std::vector<Value> args;
  Value result;
};
class Generator {
  const Unit &unit;
  std::vector<std::string> code;
  std::vector<std::pair<std::string, int>> data;
  std::map<std::string, Frame> frames;
  std::map<std::string, Value> globals;
  std::vector<std::map<std::string, Value>> scopes;
  std::vector<std::pair<std::string, std::string>> loops;
  std::vector<CallSite> calls;
  Frame *current = nullptr;
  int seq = 0;
  std::string exitLabel;
  std::set<std::string> definedLabels, usedLabels;
  std::vector<std::string> returns;
  Location here;
  [[noreturn]] void fail(const std::string &s) { throw Error(here, s); }
  std::string fresh(const std::string &prefix = "L") {
    return "__" + prefix + "_" + std::to_string(seq++);
  }
  void emit(const std::string &s) { code.push_back(s); }
  void label(const std::string &s) { emit("." + s); }
  void immediate(int reg, int n) {
    emit(std::string("ldi ") + "abcd"[reg]);
    emit("db " + std::to_string(n & 255));
  }
  void addr(const std::string &s) { emit(":" + s); }
  void load(const std::string &s, int r = 0) {
    addr(s);
    emit(std::string("rdm ") + "abcd"[r]);
  }
  void store(const std::string &s, int r = 0) {
    addr(s);
    emit(std::string("wrm ") + "abcd"[r]);
  }
  void jump(const std::string &s, const std::string &op = "jmp") {
    addr(s);
    emit(op);
  }
  Value allocate(Ty t, const std::string &name = "", bool frame = true) {
    int size = t->size();
    if (size < 0 || size > 65535)
      fail("object size exceeds target memory");
    if (size == 0 && t->kind != Type::Void)
      fail("object requires a complete, nonempty type");
    Value v{t, {}};
    std::string base = name.empty() ? fresh("tmp") : name;
    for (int i = 0; i < size; ++i) {
      std::string s = i == 0 ? base : fresh("byte");
      v.cells.push_back(s);
      data.push_back({s, 0});
      if (frame && current)
        current->cells.push_back(s);
    }
    return v;
  }
  Value constant(int n, Ty t = scalar(Type::UInt)) {
    auto v = allocate(t);
    for (size_t i = 0; i < v.cells.size(); ++i) {
      immediate(0, n >> (8 * i));
      store(v.cells[i]);
    }
    return v;
  }
  Value copy(Value v) {
    auto r = allocate(v.type);
    for (size_t i = 0; i < v.cells.size(); ++i) {
      load(v.cells[i]);
      store(r.cells[i]);
    }
    return r;
  }
  Value view(Value v, int i, Ty t = scalar(Type::UInt)) {
    return {t, {v.cells.at(i)}};
  }
  void write(Value dst, Value src) {
    if (dst.cells.size() != src.cells.size())
      fail("incompatible object size");
    for (size_t i = 0; i < dst.cells.size(); ++i) {
      load(src.cells[i]);
      store(dst.cells[i]);
    }
  }
  Value addressOf(Value v) {
    if (v.cells.empty())
      fail("cannot take address of an empty object");
    auto a = allocate(pointer(v.type));
    addr(v.cells[0]);
    emit("mov a, c");
    emit("mov b, d");
    store(a.cells[0], 1);
    store(a.cells[1], 0);
    return a;
  }
  void dynamicAddress(Value a) {
    load(a.cells[0], 1);
    load(a.cells[1], 2);
    emit("mov d, b");
  }
  Value native(const std::string &op, Value a, Value b, Ty t = nullptr) {
    auto r = allocate(t ? t : a.type);
    load(a.cells[0]);
    load(b.cells[0], 1);
    emit(op + " a, b");
    store(r.cells[0]);
    return r;
  }
  Value negate(Value a) {
    if (a.cells.size() == 2)
      return wide("-", constant(0, a.type), a);
    return native("sub", constant(0, a.type), a, a.type);
  }
  Value compare8(const std::string &op, Value a, Value b, bool unsig) {
    if (unsig && op != "==" && op != "!=") {
      a = native("xor", a, constant(128));
      b = native("xor", b, constant(128));
    }
    auto r = allocate(scalar(Type::Int));
    auto yes = fresh(), end = fresh();
    load(a.cells[0]);
    load(b.cells[0], 1);
    addr(yes);
    if (op == "==" || op == "!=")
      emit("je a, b");
    else if (op == "<" || op == ">=")
      emit("jl a, b");
    else
      emit("jg a, b");
    bool invert = op == "!=" || op == ">=" || op == "<=";
    immediate(0, invert ? 1 : 0);
    store(r.cells[0]);
    jump(end);
    label(yes);
    immediate(0, invert ? 0 : 1);
    store(r.cells[0]);
    label(end);
    return r;
  }
  Value compare(const std::string &op, Value a, Value b) {
    if (a.cells.size() == 1 && b.cells.size() == 1)
      return compare8(op, a, b,
                      a.type->kind == Type::UInt || b.type->kind == Type::UInt);
    a = convert(a, pointer(scalar(Type::Void)));
    b = convert(b, a.type);
    auto ah = view(a, 1), bh = view(b, 1), al = view(a, 0), bl = view(b, 0);
    if (op == "==" || op == "!=") {
      auto hi = compare8("==", ah, bh, true), lo = compare8("==", al, bl, true);
      auto r = native("and", hi, lo, scalar(Type::Int));
      if (op == "!=")
        r = native("xor", r, constant(1), scalar(Type::Int));
      return r;
    }
    std::string strict = (op == "<" || op == "<=") ? "<" : ">";
    auto h = compare8(strict, ah, bh, true), eq = compare8("==", ah, bh, true),
         l = compare8(op, al, bl, true);
    return native("or", h, native("and", eq, l), scalar(Type::Int));
  }
  Value truth(Value v) {
    if (v.type->kind == Type::Void)
      fail("void expression used as a value");
    if (v.cells.size() == 2)
      v = native("or", view(v, 0), view(v, 1));
    if (v.cells.size() != 1)
      fail("aggregate cannot be used as a condition");
    return compare8("!=", v, constant(0), false);
  }
  void branchZero(Value v, const std::string &target) {
    v = truth(v);
    load(v.cells[0]);
    jump(target, "jz a");
  }
  Value convert(Value v, Ty t, bool explicitCast = false) {
    if (sameType(v.type, t))
      return v;
    if (t->kind == Type::Void)
      return {t, {}};
    if (numeric(t) &&
        (numeric(v.type) || (explicitCast && v.type->kind == Type::Pointer))) {
      auto r = allocate(t);
      load(v.cells[0]);
      store(r.cells[0]);
      return r;
    }
    if (t->kind == Type::Pointer &&
        (numeric(v.type) || v.type->kind == Type::Pointer)) {
      if (v.cells.size() == 2) {
        v.type = t;
        return v;
      }
      auto r = allocate(t);
      load(v.cells[0]);
      store(r.cells[0]);
      immediate(0, 0);
      store(r.cells[1]);
      if (v.type->kind == Type::Int) {
        auto done = fresh();
        load(v.cells[0]);
        immediate(1, 0);
        addr(done);
        emit("jg a, b");
        addr(done);
        emit("je a, b");
        immediate(0, 255);
        store(r.cells[1]);
        label(done);
      }
      return r;
    }
    fail("incompatible conversion");
  }
  Value wide(const std::string &op, Value a, Value b) {
    a = convert(a, pointer(scalar(Type::Void)));
    b = convert(b, a.type);
    auto r = allocate(a.type);
    auto low = native(op == "+" ? "add" : "sub", view(a, 0), view(b, 0));
    auto carry = compare8("<", op == "+" ? low : view(a, 0),
                          op == "+" ? view(a, 0) : view(b, 0), true);
    auto high = native(op == "+" ? "add" : "sub", view(a, 1), view(b, 1));
    high = native(op == "+" ? "add" : "sub", high, carry);
    write(view(r, 0), low);
    write(view(r, 1), high);
    return r;
  }
  Value shiftOne(Value a, bool left, bool logical = false) {
    if (a.cells.size() == 1) {
      auto r = native(left ? "add" : "shr", a, left ? a : constant(1));
      if (!left && logical)
        r = native("and", r, constant(127));
      r.type = a.type;
      return r;
    }
    auto r = allocate(a.type);
    if (left) {
      auto lo = native("add", view(a, 0), view(a, 0));
      auto bit = compare8("<", view(a, 0), constant(128), true);
      bit = native("xor", bit, constant(1));
      auto hi = native("add", view(a, 1), view(a, 1));
      hi = native("add", hi, bit);
      write(view(r, 0), lo);
      write(view(r, 1), hi);
    } else {
      auto bit = native("and", view(a, 1), constant(1));
      auto top = copy(bit);
      for (int i = 0; i < 7; ++i)
        top = shiftOne(top, true);
      auto lo = native("or", shiftOne(view(a, 0), false, true), top);
      write(view(r, 0), lo);
      write(view(r, 1), shiftOne(view(a, 1), false, true));
    }
    return r;
  }
  Value multiply(Value a, Value b) {
    Ty t = a.cells.size() == 2
               ? a.type
               : (a.type->kind == Type::UInt || b.type->kind == Type::UInt
                      ? scalar(Type::UInt)
                      : scalar(Type::Int));
    a = convert(a, t);
    b = convert(b, t);
    auto lhs = copy(a), rhs = copy(b), result = constant(0, t),
         count = constant(a.cells.size() == 2 ? 16 : 8);
    auto loop = fresh(), skip = fresh(), done = fresh();
    label(loop);
    load(count.cells[0]);
    jump(done, "jz a");
    auto bit = native("and", view(rhs, 0), constant(1));
    load(bit.cells[0]);
    jump(skip, "jz a");
    write(result, result.cells.size() == 2 ? wide("+", result, lhs)
                                           : native("add", result, lhs, t));
    label(skip);
    write(lhs, shiftOne(lhs, true));
    write(rhs, shiftOne(rhs, false, true));
    load(count.cells[0]);
    emit("dec a");
    store(count.cells[0]);
    jump(loop);
    label(done);
    return result;
  }
  Value divide(Value a, Value b, bool remainder) {
    bool sign = a.type->kind == Type::Int && b.type->kind == Type::Int;
    auto lhs = copy(a), rhs = copy(b), zero = constant(0), q = constant(0),
         minusA = constant(0), minusB = constant(0);
    if (sign) {
      write(minusA, compare8("<", lhs, zero, false));
      write(minusB, compare8("<", rhs, zero, false));
      auto sa = fresh(), sb = fresh();
      load(minusA.cells[0]);
      jump(sa, "jz a");
      write(lhs, negate(lhs));
      label(sa);
      load(minusB.cells[0]);
      jump(sb, "jz a");
      write(rhs, negate(rhs));
      label(sb);
    }
    auto nonzero = fresh();
    load(rhs.cells[0]);
    jump(nonzero, "jnz a");
    emit("sdw");
    label(nonzero);
    auto loop = fresh(), done = fresh();
    label(loop);
    auto less = compare8("<", lhs, rhs, true);
    load(less.cells[0]);
    jump(done, "jnz a");
    write(lhs, native("sub", lhs, rhs));
    load(q.cells[0]);
    emit("inc a");
    store(q.cells[0]);
    jump(loop);
    label(done);
    Value r = remainder ? lhs : q;
    if (sign) {
      auto minus = remainder ? minusA : native("xor", minusA, minusB);
      auto end = fresh();
      load(minus.cells[0]);
      jump(end, "jz a");
      write(r, negate(r));
      label(end);
      r.type = scalar(Type::Int);
    } else
      r.type = scalar(Type::UInt);
    return r;
  }
  Value scaled(Value offset, int stride) {
    Value result;
    bool haveResult = false;
    while (stride) {
      if (stride & 1) {
        result = haveResult ? wide("+", result, offset) : copy(offset);
        haveResult = true;
      }
      stride >>= 1;
      if (stride)
        offset = shiftOne(offset, true);
    }
    return result;
  }
  Value pointerDifference(Value a, Value b, int stride) {
    auto delta = wide("-", a, b);
    if (stride == 1)
      return convert(delta, scalar(Type::Int), true);
    auto negative = compare("<", a, b);
    auto magnitude = copy(delta);
    auto positive = fresh();
    load(negative.cells[0]);
    jump(positive, "jz a");
    write(magnitude, negate(magnitude));
    label(positive);
    auto divisor = constant(stride, delta.type),
         quotient = constant(0, delta.type);
    auto loop = fresh(), done = fresh();
    label(loop);
    auto less = compare("<", magnitude, divisor);
    load(less.cells[0]);
    jump(done, "jnz a");
    write(magnitude, wide("-", magnitude, divisor));
    write(quotient, wide("+", quotient, constant(1, quotient.type)));
    jump(loop);
    label(done);
    auto end = fresh();
    load(negative.cells[0]);
    jump(end, "jz a");
    write(quotient, negate(quotient));
    label(end);
    return convert(quotient, scalar(Type::Int), true);
  }
  Value binary(const std::string &op, Value a, Value b) {
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
        op == ">=")
      return compare(op, a, b);
    if ((op == "+" || op == "-") &&
        (a.type->kind == Type::Pointer || b.type->kind == Type::Pointer)) {
      if (a.type->kind != Type::Pointer) {
        if (op == "-")
          fail("cannot subtract a pointer from an integer");
        std::swap(a, b);
      }
      Ty pt = a.type;
      int stride = pt->base->size();
      if (stride == 0)
        fail("arithmetic on a void pointer");
      if (b.type->kind == Type::Pointer) {
        if (op != "-" || !sameType(pt, b.type))
          fail("invalid pointer arithmetic");
        return pointerDifference(a, b, stride);
      }
      if (!numeric(b.type))
        fail("pointer offset must be an integer");
      b = convert(b, pointer(scalar(Type::Void)));
      if (stride != 1)
        b = scaled(b, stride);
      auto r = wide(op, a, b);
      r.type = pt;
      return r;
    }
    if (!numeric(a.type) || !numeric(b.type))
      fail("operator '" + op + "' needs integer operands");
    Ty t = a.type->kind == Type::UInt || b.type->kind == Type::UInt
               ? scalar(Type::UInt)
               : scalar(Type::Int);
    if (op == "+")
      return native("add", a, b, t);
    if (op == "-")
      return native("sub", a, b, t);
    if (op == "&")
      return native("and", a, b, t);
    if (op == "|")
      return native("or", a, b, t);
    if (op == "^")
      return native("xor", a, b, t);
    if (op == "*")
      return multiply(a, b);
    if (op == "/" || op == "%")
      return divide(a, b, op == "%");
    if (op == "<<" || op == ">>") {
      auto r = copy(a), count = native("and", b, constant(7));
      auto loop = fresh(), done = fresh();
      label(loop);
      load(count.cells[0]);
      jump(done, "jz a");
      write(r, shiftOne(r, op == "<<", a.type->kind == Type::UInt));
      load(count.cells[0]);
      emit("dec a");
      store(count.cells[0]);
      jump(loop);
      label(done);
      return r;
    }
    fail("unsupported binary operator '" + op + "'");
  }
  Value lookup(const std::string &name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
      auto v = it->find(name);
      if (v != it->end())
        return v->second;
    }
    auto it = globals.find(name);
    if (it == globals.end())
      fail("unknown variable '" + name + "'");
    return it->second;
  }
  const Field &field(Ty t, const std::string &n) {
    if (t->kind != Type::Struct)
      fail("member access requires a struct");
    for (const auto &f : t->fields)
      if (f.name == n)
        return f;
    fail("unknown field '" + n + "'");
  }
  Ty typeOf(Ex e) {
    here = e->loc;
    switch (e->kind) {
    case Expr::InitList:
      fail("initializer list is not an expression");
    case Expr::Literal:
      if (e->type)
        return e->type;
      return scalar(e->value > 127 || e->text.find('u') != std::string::npos ||
                            e->text.find('U') != std::string::npos
                        ? Type::UInt
                        : Type::Int);
    case Expr::StringLiteral:
      return array(scalar(Type::Int), int(e->text.size() + 1));
    case Expr::Name:
      return lookup(e->text).type;
    case Expr::Cast:
      return e->type;
    case Expr::Sizeof:
      return scalar(Type::UInt);
    case Expr::Index: {
      auto t = decay(typeOf(e->args[0]));
      if (t->kind != Type::Pointer)
        fail("indexing requires an array or pointer");
      return t->base;
    }
    case Expr::Member:
      return field(typeOf(e->args[0]), e->text).type;
    case Expr::Unary:
      if (e->text == "&")
        return pointer(typeOf(e->args[0]));
      if (e->text == "*") {
        auto t = decay(typeOf(e->args[0]));
        if (t->kind != Type::Pointer)
          fail("dereference requires a pointer");
        return t->base;
      }
      if (e->text == "-" && e->args[0]->kind == Expr::Literal &&
          e->args[0]->value <= 128 &&
          e->args[0]->text.find_first_of("uU") == std::string::npos)
        return scalar(Type::Int);
      if (e->text == "!")
        return scalar(Type::Int);
      return decay(typeOf(e->args[0]));
    case Expr::Postfix:
      return decay(typeOf(e->args[0]));
    case Expr::Assign:
      return typeOf(e->args[0]);
    case Expr::Conditional: {
      auto a = decay(typeOf(e->args[1])), b = decay(typeOf(e->args[2]));
      if (a->kind == Type::Pointer)
        return a;
      if (b->kind == Type::Pointer)
        return b;
      return numeric(a) && numeric(b)
                 ? scalar(a->kind == Type::UInt || b->kind == Type::UInt
                              ? Type::UInt
                              : Type::Int)
                 : a;
    }
    case Expr::Call: {
      if (e->args[0]->kind != Expr::Name)
        fail("function pointers are not supported");
      auto n = e->args[0]->text;
      if (n == "_putchar")
        return scalar(Type::Int);
      if (n == "_halt")
        return scalar(Type::Void);
      auto f = frames.find(n);
      if (f == frames.end())
        fail("unknown function '" + n + "'");
      return f->second.fn->result;
    }
    case Expr::Binary: {
      auto a = decay(typeOf(e->args[0])), b = decay(typeOf(e->args[1]));
      auto op = e->text;
      if (op == ",")
        return b;
      if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" ||
          op == ">=" || op == "&&" || op == "||")
        return scalar(Type::Int);
      if (a->kind == Type::Pointer && b->kind == Type::Pointer && op == "-")
        return scalar(Type::Int);
      if (a->kind == Type::Pointer)
        return a;
      if (b->kind == Type::Pointer)
        return b;
      return scalar(a->kind == Type::UInt || b->kind == Type::UInt ? Type::UInt
                                                                   : Type::Int);
    }
    }
    fail("cannot determine expression type");
  }
  Place place(Ex e) {
    here = e->loc;
    if (e->kind == Expr::Name) {
      auto v = lookup(e->text);
      return {v.type, addressOf(v)};
    }
    if (e->kind == Expr::Unary && e->text == "*") {
      auto a = eval(e->args[0]);
      if (a.type->kind != Type::Pointer)
        fail("dereference requires a pointer");
      if (a.type->base->size() == 0)
        fail("cannot dereference a void or incomplete pointer");
      return {a.type->base, a};
    }
    if (e->kind == Expr::Index) {
      auto a = eval(e->args[0]);
      if (a.type->kind != Type::Pointer)
        fail("indexing requires an array or pointer");
      Value address;
      if (e->args[1]->kind == Expr::Literal && e->args[1]->value > 255) {
        address = wide(
            "+", a, constant(e->args[1]->value * a.type->base->size(), a.type));
        address.type = a.type;
      } else
        address = binary("+", a, eval(e->args[1]));
      return {a.type->base, address};
    }
    if (e->kind == Expr::Member) {
      auto p = place(e->args[0]);
      const auto &f = field(p.type, e->text);
      auto a = wide("+", p.address, constant(f.offset, p.address.type));
      a.type = pointer(f.type);
      return {f.type, a};
    }
    fail("expression is not an assignable lvalue");
  }
  Value read(Place p) {
    if (p.type->kind == Type::Array) {
      p.address.type = pointer(p.type->base);
      return p.address;
    }
    auto v = allocate(p.type);
    for (size_t i = 0; i < v.cells.size(); ++i) {
      auto a = i ? wide("+", p.address, constant(int(i), p.address.type))
                 : p.address;
      dynamicAddress(a);
      emit("rdm a");
      store(v.cells[i]);
    }
    return v;
  }
  void assign(Place p, Value value) {
    if (p.type->kind == Type::Array)
      fail("an array cannot be assigned");
    value = convert(value, p.type);
    for (size_t i = 0; i < value.cells.size(); ++i) {
      auto a = i ? wide("+", p.address, constant(int(i), p.address.type))
                 : p.address;
      load(value.cells[i]);
      dynamicAddress(a);
      emit("wrm a");
    }
  }
  Value eval(Ex e) {
    here = e->loc;
    switch (e->kind) {
    case Expr::InitList:
      fail("initializer list is not an expression");
    case Expr::Literal:
      if (e->value > 255)
        fail("integer literal exceeds 8 bits; use a pointer cast for an "
             "address");
      return constant(e->value, typeOf(e));
    case Expr::StringLiteral: {
      auto v = allocate(array(scalar(Type::Int), int(e->text.size() + 1)),
                        fresh("string"), false);
      for (size_t i = 0; i < e->text.size(); ++i) {
        auto it = std::find_if(data.begin(), data.end(),
                               [&](auto &d) { return d.first == v.cells[i]; });
        it->second = static_cast<unsigned char>(e->text[i]);
      }
      auto a = addressOf(v);
      a.type = pointer(v.type->base);
      return a;
    }
    case Expr::Name: {
      auto v = lookup(e->text);
      if (v.type->kind == Type::Array) {
        auto a = addressOf(v);
        a.type = pointer(v.type->base);
        return a;
      }
      return copy(v);
    }
    case Expr::Sizeof: {
      auto t = e->type ? e->type : typeOf(e->args[0]);
      if (t->size() == 0)
        fail("sizeof requires a complete object type");
      return constant(t->size());
    }
    case Expr::Cast:
      if (e->type->kind == Type::Pointer && e->args[0]->kind == Expr::Literal)
        return constant(e->args[0]->value, e->type);
      return convert(eval(e->args[0]), e->type, true);
    case Expr::Index:
      return read(place(e));
    case Expr::Member:
      if (e->args[0]->kind == Expr::Call ||
          e->args[0]->kind == Expr::Conditional) {
        auto base = eval(e->args[0]);
        const auto &f = field(base.type, e->text);
        Value v{f.type, {}};
        for (int i = 0; i < f.type->size(); ++i)
          v.cells.push_back(base.cells.at(f.offset + i));
        return copy(v);
      }
      return read(place(e));
    case Expr::Unary: {
      auto op = e->text;
      if (op == "&")
        return place(e->args[0]).address;
      if (op == "*")
        return read(place(e));
      if (op == "++" || op == "--") {
        auto p = place(e->args[0]);
        auto v = binary(op == "++" ? "+" : "-", read(p),
                        constant(1, scalar(Type::Int)));
        assign(p, v);
        return v;
      }
      auto v = eval(e->args[0]);
      if (op == "!")
        return compare8("==", truth(v), constant(0), false);
      if (!numeric(v.type))
        fail("unary arithmetic requires an integer");
      if (op == "+")
        return v;
      if (op == "-") {
        auto r = negate(v);
        r.type = typeOf(e);
        return r;
      }
      if (op == "~")
        return native("xor", v, constant(255), v.type);
      fail("unknown unary operator");
    }
    case Expr::Postfix: {
      auto p = place(e->args[0]);
      auto old = read(p);
      assign(p, binary(e->text == "++" ? "+" : "-", old,
                       constant(1, scalar(Type::Int))));
      return old;
    }
    case Expr::Assign: {
      auto p = place(e->args[0]);
      Value v;
      if (e->text == "=")
        v = eval(e->args[1]);
      else {
        auto lhs = read(p);
        auto rhs = eval(e->args[1]);
        v = binary(e->text.substr(0, e->text.size() - 1), lhs, rhs);
      }
      v = convert(v, p.type);
      assign(p, v);
      return v;
    }
    case Expr::Binary: {
      auto op = e->text;
      if (op == ",") {
        eval(e->args[0]);
        return eval(e->args[1]);
      }
      if (op == "&&" || op == "||") {
        auto result = constant(op == "||" ? 1 : 0, scalar(Type::Int));
        auto done = fresh();
        auto left = truth(eval(e->args[0]));
        load(left.cells[0]);
        jump(done, op == "&&" ? "jz a" : "jnz a");
        write(result, truth(eval(e->args[1])));
        label(done);
        return result;
      }
      if ((op == "+" || op == "-") &&
          typeOf(e->args[0])->kind == Type::Pointer &&
          e->args[1]->kind == Expr::Literal && e->args[1]->value > 255) {
        auto a = eval(e->args[0]);
        auto t = a.type;
        auto r = wide(op, a, constant(e->args[1]->value * t->base->size(), t));
        r.type = t;
        return r;
      }
      auto a = eval(e->args[0]);
      auto b = eval(e->args[1]);
      return binary(op, a, b);
    }
    case Expr::Conditional: {
      auto result = allocate(typeOf(e));
      auto no = fresh(), done = fresh();
      branchZero(eval(e->args[0]), no);
      write(result, convert(eval(e->args[1]), result.type));
      jump(done);
      label(no);
      write(result, convert(eval(e->args[2]), result.type));
      label(done);
      return result;
    }
    case Expr::Call: {
      auto t = typeOf(e);
      auto n = e->args[0]->text;
      if (n == "_halt") {
        if (e->args.size() != 1)
          fail("_halt expects no arguments");
        emit("sdw");
        return {t, {}};
      }
      if (n == "_putchar") {
        if (e->args.size() != 2)
          fail("_putchar expects one argument");
        auto v = convert(eval(e->args[1]), scalar(Type::Int));
        load(v.cells[0]);
        emit("out a");
        emit("db 0");
        return v;
      }
      auto &f = frames.at(n);
      if (e->args.size() - 1 != f.params.size())
        fail("incorrect number of arguments to '" + n + "'");
      CallSite site;
      site.fn = n;
      site.label = fresh("return");
      for (size_t i = 1; i < e->args.size(); ++i)
        site.args.push_back(convert(eval(e->args[i]), f.params[i - 1].type));
      site.result = allocate(t);
      calls.push_back(site);
      emit("@" + std::to_string(calls.size() - 1));
      return site.result;
    }
    }
    fail("unsupported expression");
  }
  void initializer(Value v, const Decl &d) {
    if (d.hasInitList) {
      for (const auto &cell : v.cells) {
        immediate(0, 0);
        store(cell);
      }
      std::function<void(Ty, int, const std::vector<Ex> &, size_t &, bool)>
          fill;
      fill = [&](Ty t, int offset, const std::vector<Ex> &items, size_t &cursor,
                 bool allowBraces) {
        if (cursor == items.size())
          return;
        if ((allowBraces ||
             (t->kind != Type::Array && t->kind != Type::Struct)) &&
            items[cursor]->kind == Expr::InitList) {
          auto nested = items[cursor++];
          size_t childCursor = 0;
          fill(t, offset, nested->args, childCursor, false);
          if (childCursor != nested->args.size())
            fail("too many nested initializer elements");
          return;
        }
        if (t->kind == Type::Array && t->base->size() == 1 &&
            items[cursor]->kind == Expr::StringLiteral) {
          auto e = items[cursor++];
          if (e->text.size() + 1 > static_cast<size_t>(t->count))
            fail("string initializer does not fit byte array");
          for (size_t i = 0; i < e->text.size(); ++i) {
            immediate(0, static_cast<unsigned char>(e->text[i]));
            store(v.cells.at(offset + int(i)));
          }
          return;
        }
        if (t->kind == Type::Array) {
          for (int i = 0; i < t->count; ++i)
            fill(t->base, offset + i * t->base->size(), items, cursor, true);
        } else if (t->kind == Type::Struct) {
          for (const auto &f : t->fields)
            fill(f.type, offset + f.offset, items, cursor, true);
        } else {
          Value dest{t, {}};
          for (int i = 0; i < t->size(); ++i)
            dest.cells.push_back(v.cells.at(offset + i));
          write(dest, convert(eval(items[cursor++]), t));
        }
      };
      // The declaration's braces delimit the whole object. Inner braces are
      // consumed at the corresponding child object, preserving zero padding.
      size_t cursor = 0;
      fill(v.type, 0, d.initList, cursor, false);
      if (cursor != d.initList.size())
        fail("too many initializer elements");
      return;
    }
    if (d.init && v.type->kind == Type::Array &&
        d.init->kind == Expr::StringLiteral) {
      if (v.type->base->size() != 1 || d.init->text.size() + 1 > v.cells.size())
        fail("string initializer does not fit byte array");
      for (size_t i = 0; i < v.cells.size(); ++i) {
        immediate(0, i < d.init->text.size()
                         ? static_cast<unsigned char>(d.init->text[i])
                         : 0);
        store(v.cells[i]);
      }
      return;
    }
    if (d.init)
      write(v, convert(eval(d.init), v.type));
  }
  void declare(const Decl &d) {
    here = d.loc;
    if (scopes.back().count(d.name))
      fail("duplicate local '" + d.name + "'");
    auto v = allocate(d.type);
    scopes.back()[d.name] = v;
    initializer(v, d);
  }
  void inlineAssembly(const St &s) {
    struct Operand {
      int reg = -1;
      bool readWrite = false;
      bool immediate = false;
      int immediateValue = 0;
      Value value;
    };
    const auto regName = [](int reg) { return std::string(1, "abcd"[reg]); };
    std::vector<Operand> outputs, inputs;
    bool usedRegs[2] = {false, false};
    auto bind = [&](const AsmOperand &operand, bool output) {
      std::string constraint = operand.constraint;
      bool readWrite = false;
      if (output) {
        if (constraint.empty() ||
            (constraint[0] != '=' && constraint[0] != '+'))
          fail("inline assembly output constraint must start with '=' or '+'");
        readWrite = constraint[0] == '+';
        constraint.erase(constraint.begin());
      }
      Operand bound;
      bound.readWrite = readWrite;
      if (!output && constraint == "i") {
        if (!operand.expression ||
            operand.expression->kind != Expr::Literal ||
            operand.expression->value < 0 || operand.expression->value > 255)
          fail("inline assembly 'i' constraint requires a byte literal");
        bound.immediate = true;
        bound.immediateValue = operand.expression->value;
        return bound;
      }
      int requested = -1;
      if (constraint == "a")
        requested = 0;
      else if (constraint == "b")
        requested = 1;
      else if (constraint != "r")
        fail("unsupported inline assembly constraint '" + operand.constraint +
             "'");
      if (requested < 0) {
        for (int candidate = 0; candidate < 2; ++candidate)
          if (!usedRegs[candidate]) {
            requested = candidate;
            break;
          }
        if (requested < 0)
          fail("inline assembly needs more than two register operands");
      }
      if (usedRegs[requested])
        fail("inline assembly operands cannot share register '" +
             regName(requested) + "'");
      usedRegs[requested] = true;
      bound.reg = requested;
      return bound;
    };
    for (const auto &operand : s->asmOutputs)
      outputs.push_back(bind(operand, true));
    for (const auto &operand : s->asmInputs)
      inputs.push_back(bind(operand, false));

    std::vector<Place> places;
    for (size_t i = 0; i < s->asmOutputs.size(); ++i) {
      Place p = place(s->asmOutputs[i].expression);
      if (p.type->size() != 1 || !numeric(p.type))
        fail("inline assembly outputs must be one-byte integer lvalues");
      places.push_back(std::move(p));
      if (outputs[i].readWrite)
        outputs[i].value = read(places.back());
    }
    for (size_t i = 0; i < s->asmInputs.size(); ++i) {
      if (inputs[i].immediate)
        continue;
      Value value = eval(s->asmInputs[i].expression);
      if (value.cells.size() != 1 || !numeric(value.type))
        fail("inline assembly register inputs must be one-byte integers");
      inputs[i].value = std::move(value);
    }

    std::vector<std::string> replacements;
    replacements.reserve(outputs.size() + inputs.size());
    for (size_t i = 0; i < outputs.size(); ++i) {
      auto &operand = outputs[i];
      if (operand.readWrite)
        load(operand.value.cells[0], operand.reg);
      replacements.push_back(regName(operand.reg));
    }
    for (const auto &operand : inputs) {
      if (operand.immediate)
        replacements.push_back(std::to_string(operand.immediateValue));
      else {
        load(operand.value.cells[0], operand.reg);
        replacements.push_back(regName(operand.reg));
      }
    }
    for (const auto &clobber : s->asmClobbers)
      if (clobber != "a" && clobber != "b" && clobber != "c" &&
          clobber != "d" && clobber != "cc" && clobber != "memory")
        fail("unsupported inline assembly clobber '" + clobber + "'");

    std::string expanded;
    const std::string serial = std::to_string(seq++);
    for (size_t i = 0; i < s->text.size();) {
      if (s->text[i] != '%' || i + 1 == s->text.size()) {
        expanded.push_back(s->text[i++]);
        continue;
      }
      if (s->text[i + 1] == '%') {
        expanded.push_back('%');
        i += 2;
      } else if (s->text[i + 1] == '=') {
        expanded += serial;
        i += 2;
      } else if (std::isdigit(static_cast<unsigned char>(s->text[i + 1]))) {
        size_t end = i + 1;
        size_t index = 0;
        while (end < s->text.size() &&
               std::isdigit(static_cast<unsigned char>(s->text[end]))) {
          const size_t digit = static_cast<unsigned>(s->text[end] - '0');
          if (index > (std::numeric_limits<size_t>::max() - digit) / 10)
            fail("inline assembly placeholder is out of range");
          index = index * 10 + digit;
          ++end;
        }
        if (index >= replacements.size())
          fail("inline assembly placeholder is out of range");
        expanded += replacements[index];
        i = end;
      } else {
        expanded.push_back(s->text[i++]);
      }
    }
    emit(expanded);

    std::vector<Value> outputValues;
    outputValues.reserve(outputs.size());
    for (const auto &operand : outputs) {
      Value saved = allocate(scalar(Type::UInt));
      store(saved.cells[0], operand.reg);
      outputValues.push_back(std::move(saved));
    }
    for (size_t i = 0; i < places.size(); ++i)
      assign(std::move(places[i]), outputValues[i]);
  }
  std::string userLabel(const std::string &n) {
    return "__user_" + current->fn->name + "_" + n;
  }
  void statement(St s) {
    if (!s)
      return;
    here = s->loc;
    switch (s->kind) {
    case Stmt::Block:
      scopes.push_back({});
      for (const auto &child : s->children)
        statement(child);
      scopes.pop_back();
      break;
    case Stmt::Declaration:
      for (const auto &d : s->decls)
        declare(d);
      break;
    case Stmt::Expression:
      if (!s->exprs.empty())
        eval(s->exprs[0]);
      break;
    case Stmt::Empty:
      break;
    case Stmt::Asm:
      inlineAssembly(s);
      break;
    case Stmt::If: {
      auto no = fresh(), done = fresh();
      branchZero(eval(s->exprs[0]), no);
      statement(s->children[0]);
      jump(done);
      label(no);
      if (s->children.size() > 1)
        statement(s->children[1]);
      label(done);
      break;
    }
    case Stmt::While:
    case Stmt::DoWhile:
    case Stmt::For: {
      auto start = fresh(), step = fresh(), done = fresh();
      scopes.push_back({});
      if (s->kind == Stmt::For) {
        for (const auto &d : s->decls)
          declare(d);
        if (s->exprs[0])
          eval(s->exprs[0]);
      }
      label(start);
      if (s->kind == Stmt::While)
        branchZero(eval(s->exprs[0]), done);
      if (s->kind == Stmt::For && s->exprs[1])
        branchZero(eval(s->exprs[1]), done);
      loops.push_back({done, step});
      statement(s->children[0]);
      loops.pop_back();
      label(step);
      if (s->kind == Stmt::For && s->exprs[2])
        eval(s->exprs[2]);
      if (s->kind == Stmt::DoWhile)
        branchZero(eval(s->exprs[0]), done);
      jump(start);
      label(done);
      scopes.pop_back();
      break;
    }
    case Stmt::Break:
    case Stmt::Continue:
      if (loops.empty())
        fail("loop control outside a loop");
      jump(s->kind == Stmt::Break ? loops.back().first : loops.back().second);
      break;
    case Stmt::Return: {
      Ty t = current->fn->result;
      if (s->exprs.empty()) {
        if (t->kind != Type::Void)
          fail("non-void function requires a return value");
      } else {
        if (t->kind == Type::Void)
          fail("void function cannot return a value");
        auto v = convert(eval(s->exprs[0]), t);
        for (size_t i = 0; i < v.cells.size(); ++i) {
          load(v.cells[i]);
          store(returns[i]);
        }
      }
      jump(exitLabel);
      break;
    }
    case Stmt::Label:
      if (!definedLabels.insert(s->text).second)
        fail("duplicate label '" + s->text + "'");
      label(userLabel(s->text));
      if (!s->children.empty())
        statement(s->children[0]);
      break;
    case Stmt::Goto:
      usedLabels.insert(s->text);
      jump(userLabel(s->text));
      break;
    }
  }
  std::vector<std::string> expandCall(const CallSite &site) {
    auto old = std::move(code);
    code.clear();
    auto &f = frames.at(site.fn);
    for (const auto &cell : f.cells) {
      load(cell);
      emit("push a");
    }
    for (size_t i = 0; i < site.args.size(); ++i)
      write(f.params[i], site.args[i]);
    addr(site.label);
    emit("mov a, c");
    emit("push a");
    emit("mov a, d");
    emit("push a");
    jump("__fn_" + site.fn);
    label(site.label);
    for (auto it = f.cells.rbegin(); it != f.cells.rend(); ++it) {
      emit("pop a");
      store(*it);
    }
    for (size_t i = 0; i < site.result.cells.size(); ++i) {
      load(returns[i]);
      store(site.result.cells[i]);
    }
    auto result = std::move(code);
    code = std::move(old);
    return result;
  }

public:
  explicit Generator(const Unit &u) : unit(u) {}
  CompileResult run() {
    here = {"<compiler>", 1, 1};
    if (unit.origin < 0 || unit.origin > 65535)
      fail("origin outside 16-bit address space");
    int returnSize = 1;
    for (const auto &f : unit.functions) {
      if (frames.count(f.name) || f.name == "_putchar" || f.name == "_halt")
        fail("duplicate or reserved function '" + f.name + "'");
      frames[f.name].fn = &f;
      returnSize = std::max(returnSize, f.result->size());
    }
    if (!frames.count("main"))
      fail("program requires a main function");
    if (!frames.at("main").fn->params.empty())
      fail("main must take no parameters");
    for (int i = 0; i < returnSize; ++i) {
      auto s = "__return_value_" + std::to_string(i);
      returns.push_back(s);
      data.push_back({s, 0});
    }
    for (const auto &d : unit.globals) {
      here = d.loc;
      if (globals.count(d.name) || frames.count(d.name))
        fail("duplicate global '" + d.name + "'");
      globals[d.name] = allocate(d.type, "__global_" + d.name, false);
    }
    for (auto &[name, frame] : frames) {
      current = &frame;
      std::set<std::string> names;
      for (const auto &p : frame.fn->params) {
        if (!names.insert(p.name).second)
          fail("duplicate parameter");
        Ty t = decay(p.type);
        frame.params.push_back(allocate(t));
      }
    }
    current = nullptr;
    if (unit.origin)
      emit("org " + std::to_string(unit.origin));
    label("__entry");
    for (const auto &d : unit.globals)
      initializer(globals.at(d.name), d);
    CallSite main;
    main.fn = "main";
    main.label = fresh("_halt");
    main.result = {scalar(Type::Void), {}};
    calls.push_back(main);
    emit("@" + std::to_string(calls.size() - 1));
    emit("sdw");
    for (const auto &f : unit.functions) {
      current = &frames.at(f.name);
      here = f.loc;
      definedLabels.clear();
      usedLabels.clear();
      scopes.clear();
      scopes.push_back({});
      for (size_t i = 0; i < f.params.size(); ++i)
        scopes.back()[f.params[i].name] = current->params[i];
      exitLabel = fresh("epilogue");
      label("__fn_" + f.name);
      for (const auto &child : f.body->children)
        statement(child);
      // Deterministic zero return on fallthrough, including C-style main.
      for (int i = 0; i < f.result->size(); ++i) {
        immediate(0, 0);
        store(returns[i]);
      }
      label(exitLabel);
      emit("pop d");
      emit("pop c");
      emit("jmp");
      for (const auto &n : usedLabels)
        if (!definedLabels.count(n))
          fail("undefined label '" + n + "'");
      current = nullptr;
    }
    std::ostringstream out;
    out << "; generated by mupc-cc; c:d is reserved for addresses\n";
    for (const auto &line : code) {
      if (!line.empty() && line[0] == '@') {
        auto n = std::stoul(line.substr(1));
        for (const auto &l : expandCall(calls.at(n)))
          out << l << '\n';
      } else
        out << line << '\n';
    }
    out << "; RAM objects and expression temporaries\n";
    for (const auto &[name, value] : data)
      out << '.' << name << "\ndb " << value << '\n';
    CompileResult result;
    result.assembly = out.str();
    result.image = assemble(result.assembly);
    for (const auto &[name, v] : globals) {
      for (size_t i = 1; i < v.cells.size(); ++i)
        result.image.symbols.emplace("__global_" + name + "_b" +
                                         std::to_string(i),
                                     result.image.symbols.at(v.cells[i]));
      if (v.type->kind == Type::Pointer)
        result.image.symbols.emplace("__global_" + name + "_hi",
                                     result.image.symbols.at(v.cells[1]));
    }
    return result;
  }
};
} // namespace
CompileResult compile(const Unit &u) { return Generator(u).run(); }
} // namespace mupc
