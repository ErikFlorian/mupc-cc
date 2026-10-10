#include "mupc/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <utility>

namespace mupc {
std::string Location::str() const {
  return file + ":" + std::to_string(line) + ":" + std::to_string(column);
}
int Type::size() const {
  std::vector<const Type *> active;
  auto sizeOf = [&](auto &&self, const Type *type) -> long long {
    if (!type)
      throw std::runtime_error("type has no size");
    switch (type->kind) {
    case Int:
    case UInt:
      return 1;
    case Pointer:
      return 2;
    case Void:
      return 0;
    case Array: {
      if (type->count < 0)
        throw std::runtime_error("array has a negative element count");
      const long long elementSize = self(self, type->base.get());
      const long long total = static_cast<long long>(type->count) * elementSize;
      if (total > 65535)
        throw std::runtime_error("array size exceeds the 16-bit address space");
      return total;
    }
    case Struct: {
      if (!type->complete)
        throw std::runtime_error("size requested for incomplete struct '" +
                                 type->name + "'");
      if (std::find(active.begin(), active.end(), type) != active.end())
        throw std::runtime_error("recursive by-value struct '" + type->name +
                                 "'");
      active.push_back(type);
      long long total = 0;
      for (const auto &field : type->fields) {
        const long long fieldSize = self(self, field.type.get());
        total =
            std::max(total, static_cast<long long>(field.offset) + fieldSize);
        if (total > 65535)
          throw std::runtime_error(
              "struct size exceeds the 16-bit address space");
      }
      active.pop_back();
      return total;
    }
    }
    throw std::runtime_error("unknown type size");
  };
  return static_cast<int>(sizeOf(sizeOf, this));
}
Ty scalar(Type::Kind k) {
  auto t = std::make_shared<Type>();
  t->kind = k;
  return t;
}
Ty pointer(Ty b) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Pointer;
  t->base = std::move(b);
  return t;
}
Ty array(Ty b, int n) {
  auto t = std::make_shared<Type>();
  t->kind = Type::Array;
  t->base = std::move(b);
  t->count = n;
  return t;
}
bool sameType(Ty a, Ty b) {
  if (a == b)
    return true;
  if (!a || !b || a->kind != b->kind)
    return false;
  switch (a->kind) {
  case Type::Int:
  case Type::UInt:
  case Type::Void:
    return true;
  case Type::Pointer:
    return sameType(a->base, b->base);
  case Type::Array:
    return a->count == b->count && sameType(a->base, b->base);
  case Type::Struct:
    return a->name == b->name;
  }
  return false;
}

namespace {
using E = std::shared_ptr<Expr>;
using S = std::shared_ptr<Stmt>;
struct Parser {
  const std::vector<Token> &ts;
  size_t p = 0;
  Unit unit;
  std::map<std::string, Ty> tags;
  explicit Parser(const std::vector<Token> &t) : ts(t) {}
  const Token &tok(size_t n = 0) const {
    size_t i = std::min(p + n, ts.size() - 1);
    return ts[i];
  }
  bool is(const std::string &s) const { return tok().text == s; }
  bool eat(const std::string &s) {
    if (is(s)) {
      ++p;
      return true;
    }
    return false;
  }
  [[noreturn]] void fail(const std::string &m) const {
    throw Error(tok().loc, m);
  }
  Token take() {
    Token t = tok();
    if (t.kind != Token::End)
      ++p;
    return t;
  }
  Token need(const std::string &s) {
    if (!eat(s))
      fail("expected '" + s + "', found '" + tok().text + "'");
    return ts[p - 1];
  }
  std::string ident() {
    if (tok().kind != Token::Identifier)
      fail("expected identifier");
    return take().text;
  }
  static bool typeWord(const Token &t) {
    return t.text == "int" || t.text == "uint" || t.text == "char" ||
           t.text == "void" || t.text == "struct";
  }
  bool typeStart() const { return typeWord(tok()); }
  Ty spec() {
    if (eat("int") || eat("char"))
      return scalar(Type::Int);
    if (eat("uint"))
      return scalar(Type::UInt);
    if (eat("void"))
      return scalar(Type::Void);
    if (eat("struct")) {
      std::string name = ident();
      Ty t;
      auto it = tags.find(name);
      if (it != tags.end())
        t = it->second;
      else {
        t = std::make_shared<Type>();
        t->kind = Type::Struct;
        t->name = name;
        tags[name] = t;
      }
      if (eat("{")) {
        if (t->complete)
          fail("redefinition of struct '" + name + "'");
        std::vector<Field> fs;
        int offset = 0;
        while (!eat("}")) {
          if (tok().kind == Token::End)
            fail("unterminated struct definition");
          Ty b = spec();
          auto ds = declarators(b, false);
          need(";");
          for (auto &d : ds) {
            if (d.name.empty())
              fail("struct fields must be named");
            for (auto &f : fs)
              if (f.name == d.name)
                throw Error(d.loc, "duplicate struct field '" + d.name + "'");
            validateObjectType(d.type, d.loc, "field");
            fs.push_back({d.name, d.type, offset});
            try {
              offset += d.type->size();
            } catch (const std::exception &e) {
              throw Error(d.loc, e.what());
            }
            if (offset > 65535)
              throw Error(d.loc,
                          "struct size exceeds the 16-bit address space");
          }
        }
        if (fs.empty())
          fail("struct '" + name + "' must contain at least one field");
        t->fields = std::move(fs);
        t->complete = true;
      }
      return t;
    }
    fail("expected type specifier");
  }
  bool constantValue(const E &e, long long &out) const {
    if (!e)
      return false;
    if (e->kind == Expr::Literal) {
      out = e->value;
      return true;
    }
    if (e->kind == Expr::Unary && e->args.size() == 1) {
      long long a;
      if (!constantValue(e->args[0], a))
        return false;
      if (e->text == "+")
        out = a;
      else if (e->text == "-")
        out = -a;
      else if (e->text == "~")
        out = ~a;
      else if (e->text == "!")
        out = !a;
      else
        return false;
      return true;
    }
    if (e->kind == Expr::Cast && e->args.size() == 1)
      return constantValue(e->args[0], out);
    if (e->kind == Expr::Conditional && e->args.size() == 3) {
      long long condition;
      if (!constantValue(e->args[0], condition))
        return false;
      return constantValue(e->args[condition ? 1 : 2], out);
    }
    if (e->kind != Expr::Binary || e->args.size() != 2)
      return false;
    long long a, b;
    if (!constantValue(e->args[0], a) || !constantValue(e->args[1], b))
      return false;
    if (e->text == "+") {
      if (__builtin_add_overflow(a, b, &out))
        return false;
    } else if (e->text == "-") {
      if (__builtin_sub_overflow(a, b, &out))
        return false;
    } else if (e->text == "*") {
      if (__builtin_mul_overflow(a, b, &out))
        return false;
    } else if (e->text == "/") {
      if (!b)
        return false;
      out = a / b;
    } else if (e->text == "%") {
      if (!b)
        return false;
      out = a % b;
    } else if (e->text == "<<") {
      if (b < 0 || b > 15)
        return false;
      if (__builtin_mul_overflow(a, 1LL << b, &out))
        return false;
    } else if (e->text == ">>") {
      if (b < 0 || b > 15)
        return false;
      out = a >> b;
    } else if (e->text == "&")
      out = a & b;
    else if (e->text == "|")
      out = a | b;
    else if (e->text == "^")
      out = a ^ b;
    else if (e->text == "==")
      out = a == b;
    else if (e->text == "!=")
      out = a != b;
    else if (e->text == "<")
      out = a < b;
    else if (e->text == ">")
      out = a > b;
    else if (e->text == "<=")
      out = a <= b;
    else if (e->text == ">=")
      out = a >= b;
    else if (e->text == "&&")
      out = a && b;
    else if (e->text == "||")
      out = a || b;
    else
      return false;
    return true;
  }
  struct D {
    Location loc;
    std::string name;
    Ty type;
    bool parenthesized = false;
  };
  void validateObjectType(const Ty &type, const Location &loc,
                          const std::string &what) {
    if (!type)
      throw Error(loc, what + " has no type");
    switch (type->kind) {
    case Type::Void:
      throw Error(loc, what + " cannot have void type");
    case Type::Pointer:
    case Type::Int:
    case Type::UInt:
      return;
    case Type::Array:
      if (type->count <= 0)
        throw Error(loc, what + " has an incomplete or empty array type");
      validateObjectType(type->base, loc, what);
      try {
        (void)type->size();
      } catch (const std::exception &e) {
        throw Error(loc, e.what());
      }
      return;
    case Type::Struct:
      if (!type->complete)
        throw Error(loc, what + " uses incomplete struct '" + type->name +
                             "' by value");
      try {
        (void)type->size();
      } catch (const std::exception &e) {
        throw Error(loc, e.what());
      }
      return;
    }
  }
  D declarator(Ty base, bool abstract = false) {
    int stars = 0;
    while (eat("*"))
      ++stars;
    D d;
    d.loc = tok().loc;
    const bool grouped = eat("(");
    if (grouped) {
      d = declarator(base, abstract);
      need(")");
      d.parenthesized = true;
      if (stars != 0)
        fail("pointer declarators around parentheses are not supported");
    } else if (tok().kind == Token::Identifier) {
      d.name = take().text;
      d.type = base;
      for (int i = 0; i < stars; ++i)
        d.type = pointer(d.type);
    } else if (!abstract) {
      fail("expected declarator name");
    } else {
      d.type = base;
      for (int i = 0; i < stars; ++i)
        d.type = pointer(d.type);
    }
    std::vector<int> dims;
    while (eat("[")) {
      int n = 0;
      if (!is("]")) {
        E x = expr(1);
        long long value = 0;
        if (!constantValue(x, value))
          throw Error(x->loc,
                      "array length must be an integer constant expression");
        if (value < 0 || value > 65535)
          throw Error(x->loc, "array length must be in the range 0..65535");
        n = static_cast<int>(value);
      }
      need("]");
      dims.push_back(n);
    }
    if (grouped && !dims.empty() &&
        (d.type->kind == Type::Pointer || d.type->kind == Type::Array))
      throw Error(d.loc, "pointer-to-array declarators are not supported");
    for (auto it = dims.rbegin(); it != dims.rend(); ++it)
      d.type = array(d.type, *it);
    return d;
  }
  std::vector<D> declarators(Ty b, bool abstract = false) {
    std::vector<D> out;
    out.push_back(declarator(b, abstract));
    while (eat(","))
      out.push_back(declarator(b, abstract));
    return out;
  }
  E node(Expr::Kind k, const Location &l, std::string text = "") {
    auto e = std::make_shared<Expr>();
    e->kind = k;
    e->loc = l;
    e->text = std::move(text);
    return e;
  }
  S stmt(Stmt::Kind k, const Location &l) {
    auto s = std::make_shared<Stmt>();
    s->kind = k;
    s->loc = l;
    return s;
  }
  static int digitValue(char c) {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  }
  int numberRaw(const Token &t) {
    const std::string &x = t.text;
    int base = 10;
    size_t i = 0;
    if (x.size() > 2 && x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) {
      base = 16;
      i = 2;
    } else if (x.size() > 2 && x[0] == '0' && (x[1] == 'b' || x[1] == 'B')) {
      base = 2;
      i = 2;
    }
    unsigned value = 0;
    bool any = false;
    for (; i < x.size(); ++i) {
      const int digit = digitValue(x[i]);
      if (digit < 0 || digit >= base)
        break;
      any = true;
      if (value > (65535u - static_cast<unsigned>(digit)) /
                      static_cast<unsigned>(base)) {
        throw Error(t.loc,
                    "integer literal is outside the 16-bit range: '" + x + "'");
      }
      value =
          value * static_cast<unsigned>(base) + static_cast<unsigned>(digit);
    }
    if (!any)
      throw Error(t.loc, "invalid integer literal '" + x + "'");
    if (i != x.size())
      throw Error(t.loc, "invalid integer literal suffix: '" + x + "'");
    return static_cast<int>(value);
  }
  int number(const Token &t) { return numberRaw(t); }
  std::string decode(const Token &t) {
    const std::string &x = t.text;
    if (x.size() < 2)
      return {};
    const size_t end = x.size() - 1;
    std::string out;
    for (size_t i = 1; i < end; ++i) {
      unsigned value = static_cast<unsigned char>(x[i]);
      if (x[i] == '\\') {
        if (++i >= end)
          throw Error(t.loc, "incomplete escape sequence");
        const char escaped = x[i];
        switch (escaped) {
        case 'a':
          value = '\a';
          break;
        case 'b':
          value = '\b';
          break;
        case 'f':
          value = '\f';
          break;
        case 'n':
          value = '\n';
          break;
        case 'r':
          value = '\r';
          break;
        case 't':
          value = '\t';
          break;
        case 'v':
          value = '\v';
          break;
        case '\\':
          value = '\\';
          break;
        case '\'':
          value = '\'';
          break;
        case '"':
          value = '"';
          break;
        case '?':
          value = '?';
          break;
        case 'x': {
          value = 0;
          size_t digits = 0;
          while (i + 1 < end && digits < 2) {
            const int d = digitValue(x[i + 1]);
            if (d < 0 || d >= 16)
              break;
            value = value * 16 + static_cast<unsigned>(d);
            ++i;
            ++digits;
          }
          if (digits == 0)
            throw Error(t.loc, "hex escape requires at least one digit");
          break;
        }
        default:
          if (escaped < '0' || escaped > '7')
            throw Error(t.loc,
                        std::string("unknown escape sequence \\\\") + escaped);
          value = static_cast<unsigned>(escaped - '0');
          for (int digits = 1;
               digits < 3 && i + 1 < end && x[i + 1] >= '0' && x[i + 1] <= '7';
               ++digits) {
            value = value * 8 + static_cast<unsigned>(x[++i] - '0');
          }
          if (value > 255)
            throw Error(t.loc, "octal escape exceeds one byte");
        }
      }
      out.push_back(static_cast<char>(value));
    }
    return out;
  }
  E aggregateValues() {
    Token open = need("{");
    E list = node(Expr::InitList, open.loc);
    if (!is("}")) {
      do {
        if (is("{"))
          list->args.push_back(aggregateValues());
        else
          list->args.push_back(expr(1));
      } while (eat(",") && !is("}"));
    }
    need("}");
    return list;
  }

  E primary() {
    Token t = tok();
    if (eat("(")) {
      E e = expr(1);
      need(")");
      return e;
    }
    if (t.kind == Token::Number) {
      take();
      E e = node(Expr::Literal, t.loc, t.text);
      e->value = number(t);
      e->type = scalar(e->value > 127 ? Type::UInt : Type::Int);
      return e;
    }
    if (t.kind == Token::String) {
      take();
      std::string bytes = decode(t);
      while (tok().kind == Token::String)
        bytes += decode(take());
      E e = node(Expr::StringLiteral, t.loc, std::move(bytes));
      e->value = int(e->text.size() + 1);
      e->type = array(scalar(Type::Int), e->value);
      return e;
    }
    if (t.kind == Token::Character) {
      take();
      auto v = decode(t);
      if (v.size() != 1)
        throw Error(t.loc, "character literal must decode to exactly one byte");
      E e = node(Expr::Literal, t.loc,
                 std::to_string(static_cast<unsigned char>(v[0])));
      e->value = static_cast<unsigned char>(v[0]);
      e->type = scalar(Type::Int);
      return e;
    }
    if (t.kind == Token::Identifier) {
      take();
      return node(Expr::Name, t.loc, t.text);
    }
    fail("expected expression");
  }
  E postfix() {
    E e = primary();
    for (;;) {
      if (eat("(")) {
        auto c = node(Expr::Call, e->loc);
        c->args.push_back(e);
        if (!is(")")) {
          do {
            c->args.push_back(expr(1));
          } while (eat(","));
        }
        need(")");
        e = c;
      } else if (eat("[")) {
        auto x = node(Expr::Index, e->loc);
        x->args = {e, expr(1)};
        need("]");
        e = x;
      } else if (eat(".") || eat("->")) {
        std::string op = ts[p - 1].text;
        auto x = node(Expr::Member, e->loc, ident());
        if (op == "->") {
          auto u = node(Expr::Unary, e->loc, "*");
          u->args.push_back(e);
          e = u;
        }
        x->args.push_back(e);
        e = x;
      } else if (is("++") || is("--")) {
        Token t = take();
        auto x = node(Expr::Postfix, t.loc, t.text);
        x->args.push_back(e);
        e = x;
      } else
        break;
    }
    return e;
  }
  E unary() {
    Token t = tok();
    if (is("+") || is("-") || is("!") || is("~") || is("*") || is("&") ||
        is("++") || is("--")) {
      take();
      auto e = node(Expr::Unary, t.loc, t.text);
      e->args.push_back(unary());
      return e;
    }
    if (eat("sizeof")) {
      auto e = node(Expr::Sizeof, t.loc);
      if (eat("(")) {
        if (typeStart()) {
          Ty b = spec();
          auto d = declarator(b, true);
          validateObjectType(d.type, t.loc, "sizeof operand");
          e->type = d.type;
          need(")");
        } else {
          e->args.push_back(expr(1));
          need(")");
        }
      } else
        e->args.push_back(unary());
      return e;
    }
    if (eat("(")) {
      if (typeStart()) {
        Ty b = spec();
        D d = declarator(b, true);
        need(")");
        if (d.type->kind == Type::Struct && !d.type->complete)
          throw Error(t.loc, "cannot cast to incomplete struct '" +
                                 d.type->name + "'");
        auto e = node(Expr::Cast, t.loc);
        e->type = d.type;
        e->args.push_back(unary());
        return e;
      }
      --p;
    }
    return postfix();
  }
  int prec(const std::string &o) const {
    if (o == "=" || o == "+=" || o == "-=" || o == "*=" || o == "/=" ||
        o == "%=" || o == "&=" || o == "|=" || o == "^=" || o == "<<=" ||
        o == ">>=")
      return 1;
    if (o == "||")
      return 3;
    if (o == "&&")
      return 4;
    if (o == "|")
      return 5;
    if (o == "^")
      return 6;
    if (o == "&")
      return 7;
    if (o == "==" || o == "!=")
      return 8;
    if (o == "<" || o == ">" || o == "<=" || o == ">=")
      return 9;
    if (o == "<<" || o == ">>")
      return 10;
    if (o == "+" || o == "-")
      return 11;
    if (o == "*" || o == "/" || o == "%")
      return 12;
    return 0;
  }
  E expr(int minp = 1) {
    E lhs = unary();
    for (;;) {
      if (is("?") && minp <= 2) {
        Token t = take();
        auto e = node(Expr::Conditional, t.loc);
        e->args = {lhs, expr(1)};
        need(":");
        e->args.push_back(expr(2));
        lhs = e;
        continue;
      }
      std::string op = tok().text;
      int pr = prec(op);
      if (pr < minp || pr == 0)
        break;
      Token t = take();
      bool right = pr == 1;
      E rhs = expr(pr + (right ? 0 : 1));
      auto e = node(pr == 1 ? Expr::Assign : Expr::Binary, t.loc, op);
      e->args = {lhs, rhs};
      lhs = e;
    }
    return lhs;
  }
  bool assignStart() { return typeStart(); }
  Decl initializedDecl(D d) {
    Decl x;
    x.loc = d.loc;
    x.name = std::move(d.name);
    x.type = std::move(d.type);
    if (eat("=")) {
      if (is("{")) {
        x.hasInitList = true;
        x.initList = aggregateValues()->args;
      } else {
        x.init = expr(1);
      }
    }
    if (x.type->kind == Type::Array && x.type->count == 0) {
      if (x.hasInitList)
        x.type = array(x.type->base, static_cast<int>(x.initList.size()));
      else if (x.init && x.init->kind == Expr::StringLiteral)
        x.type = array(x.type->base, x.init->value);
    }
    validateObjectType(x.type, x.loc, "variable");
    return x;
  }
  std::vector<Decl> localDecl(bool semi = true) {
    Ty b = spec();
    std::vector<Decl> out;
    do {
      out.push_back(initializedDecl(declarator(b)));
    } while (eat(","));
    if (semi)
      need(";");
    return out;
  }
  S compound() {
    Token t = need("{");
    S b = stmt(Stmt::Block, t.loc);
    while (!eat("}")) {
      if (tok().kind == Token::End)
        fail("unterminated block");
      if (typeStart()) {
        auto s = stmt(Stmt::Declaration, tok().loc);
        s->decls = localDecl();
        b->children.push_back(s);
      } else
        b->children.push_back(statement());
    }
    return b;
  }
  S statement() {
    Token t = tok();
    if (is("{"))
      return compound();
    if (eat(";"))
      return stmt(Stmt::Empty, t.loc);
    if (eat("if")) {
      S s = stmt(Stmt::If, t.loc);
      need("(");
      s->exprs.push_back(expr());
      need(")");
      s->children.push_back(statement());
      if (eat("else"))
        s->children.push_back(statement());
      return s;
    }
    if (eat("while")) {
      S s = stmt(Stmt::While, t.loc);
      need("(");
      s->exprs.push_back(expr());
      need(")");
      s->children.push_back(statement());
      return s;
    }
    if (eat("do")) {
      S s = stmt(Stmt::DoWhile, t.loc);
      s->children.push_back(statement());
      need("while");
      need("(");
      s->exprs.push_back(expr());
      need(")");
      need(";");
      return s;
    }
    if (eat("for")) {
      S s = stmt(Stmt::For, t.loc);
      need("(");
      if (typeStart()) {
        auto ds = localDecl(false);
        s->decls = std::move(ds);
        need(";");
        s->exprs.push_back(nullptr);
      } else {
        if (!is(";"))
          s->exprs.push_back(expr());
        else
          s->exprs.push_back(nullptr);
        need(";");
      }
      if (!is(";"))
        s->exprs.push_back(expr());
      else
        s->exprs.push_back(nullptr);
      need(";");
      if (!is(")"))
        s->exprs.push_back(expr());
      else
        s->exprs.push_back(nullptr);
      need(")");
      s->children.push_back(statement());
      return s;
    }
    if (eat("return")) {
      S s = stmt(Stmt::Return, t.loc);
      if (!is(";"))
        s->exprs.push_back(expr());
      need(";");
      return s;
    }
    if (eat("break")) {
      need(";");
      return stmt(Stmt::Break, t.loc);
    }
    if (eat("continue")) {
      need(";");
      return stmt(Stmt::Continue, t.loc);
    }
    if (eat("goto")) {
      S s = stmt(Stmt::Goto, t.loc);
      s->text = ident();
      need(";");
      return s;
    }
    if (eat("asm")) {
      S s = stmt(Stmt::Asm, t.loc);
      if (eat("(")) {
        if (tok().kind != Token::String)
          fail("asm(...) expects a string literal");
        s->text = decode(take());
        need(")");
      } else if (eat("{")) {
        int depth = 1;
        int previousLine = -1;
        std::string previousFile;
        std::string previousToken;
        bool haveToken = false;
        while (depth) {
          if (tok().kind == Token::End)
            fail("unterminated asm block");
          Token a = take();
          if (a.text == "{") {
            ++depth;
            continue;
          }
          if (a.text == "}") {
            if (--depth == 0)
              break;
            continue;
          }
          if (a.loc.file != previousFile || a.loc.line != previousLine) {
            if (!s->text.empty())
              s->text += '\n';
          } else if (haveToken && previousToken != "." &&
                     previousToken != ":") {
            s->text += ' ';
          }
          s->text += a.text;
          previousLine = a.loc.line;
          previousFile = a.loc.file;
          previousToken = a.text;
          haveToken = true;
        }
      } else
        fail("expected '(' or '{' after asm");
      eat(";");
      return s;
    }
    if (tok().kind == Token::Identifier && tok(1).text == ":") {
      S s = stmt(Stmt::Label, t.loc);
      s->text = take().text;
      take();
      s->children.push_back(statement());
      return s;
    }
    S s = stmt(Stmt::Expression, t.loc);
    s->exprs.push_back(expr());
    need(";");
    return s;
  }
  void top() {
    if (eat("origin")) {
      Token t = take();
      if (t.kind != Token::Number)
        throw Error(t.loc, "origin expects an integer address");
      unit.origin = numberRaw(t);
      need(";");
      return;
    }
    Ty b = spec();
    if (eat(";"))
      return;
    D d = declarator(b);
    if (is("(")) {
      if (d.parenthesized && d.type->kind == Type::Pointer)
        throw Error(d.loc, "function pointer declarators are not supported");
      take();
      std::vector<Decl> params;
      if (!is(")")) {
        if (is("void") && tok(1).text == ")")
          take();
        else
          do {
            Ty pt = spec();
            D pd = declarator(pt, true);
            if (pd.name.empty())
              pd.name = "_arg" + std::to_string(params.size());
            validateObjectType(pd.type, pd.loc, "parameter");
            params.push_back({pd.loc, pd.name, pd.type, {}, {}, false});
          } while (eat(","));
      }
      need(")");
      if (eat(";"))
        return;
      for (auto &f : unit.functions)
        if (f.name == d.name)
          throw Error(d.loc, "duplicate function definition '" + d.name + "'");
      Function f;
      f.loc = d.loc;
      f.name = d.name;
      f.result = d.type;
      f.params = std::move(params);
      f.body = compound();
      unit.functions.push_back(std::move(f));
      return;
    }
    for (;;) {
      D x = std::move(d);
      for (auto &g : unit.globals)
        if (g.name == x.name)
          throw Error(x.loc, "duplicate global definition '" + x.name + "'");
      Decl g;
      g.loc = x.loc;
      g.name = x.name;
      g.type = x.type;
      if (eat("=")) {
        if (is("{")) {
          g.hasInitList = true;
          g.initList = aggregateValues()->args;
        } else
          g.init = expr(1);
      }
      if (g.type->kind == Type::Array && g.type->count == 0) {
        if (g.hasInitList)
          g.type = array(g.type->base, (int)g.initList.size());
        else if (g.init && g.init->kind == Expr::StringLiteral)
          g.type = array(g.type->base, g.init->value);
      }
      validateObjectType(g.type, g.loc, "variable");
      unit.globals.push_back(std::move(g));
      if (!eat(","))
        break;
      d = declarator(b);
    }
    need(";");
  }
  Unit run() {
    while (tok().kind != Token::End)
      top();
    unit.structs = tags;
    return std::move(unit);
  }
};
} // namespace
Unit parse(const std::vector<Token> &tokens) {
  Parser p(tokens);
  return p.run();
}
} // namespace mupc
