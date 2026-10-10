#include "mupc/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace mupc {
namespace {
struct Macro {
  bool function = false;
  std::vector<std::string> params;
  std::vector<Token> body;
};
struct PP {
  PreprocessorOptions opt;
  std::map<std::string, Macro> macros;
  std::vector<std::filesystem::path> stack;
  bool blockComment = false;
  static std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == s.npos)
      return {};
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
  }
  static bool ident0(char c) {
    return std::isalpha((unsigned char)c) || c == '_';
  }
  static bool ident(char c) {
    return ident0(c) || std::isdigit((unsigned char)c);
  }
  std::vector<Token> lex(const std::string &s, const std::string &file,
                         int line) {
    std::vector<Token> v;
    size_t i = 0;
    int ln = line, col = 1;
    auto advance = [&](char c) {
      if (c == '\n') {
        ++ln;
        col = 1;
      } else
        ++col;
    };
    while (i < s.size()) {
      char c = s[i];
      if (blockComment) {
        if (c == '*' && i + 1 < s.size() && s[i + 1] == '/') {
          advance(s[i++]);
          advance(s[i++]);
          blockComment = false;
        } else {
          advance(c);
          ++i;
        }
        continue;
      }
      if (std::isspace((unsigned char)c)) {
        advance(c);
        ++i;
        continue;
      }
      Location l{file, ln, col};
      size_t b = i;
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
        while (i < s.size() && s[i] != '\n') {
          advance(s[i]);
          ++i;
        }
        continue;
      }
      if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') {
        advance(s[i++]);
        advance(s[i++]);
        blockComment = true;
        continue;
      }
      Token::Kind k = Token::Punct;
      if (ident0(c)) {
        k = Token::Identifier;
        while (i < s.size() && ident(s[i])) {
          advance(s[i]);
          ++i;
        }
      } else if (std::isdigit((unsigned char)c)) {
        k = Token::Number;
        while (i < s.size() &&
               (std::isalnum((unsigned char)s[i]) || s[i] == '_')) {
          advance(s[i]);
          ++i;
        }
        if (i < s.size() && s[i] == '.')
          throw Error(l, "floating point literals are not supported");
        std::string num = s.substr(b, i - b);
        size_t start = 0;
        int base = 10;
        if (num.size() > 2 && num[0] == '0' &&
            (num[1] == 'x' || num[1] == 'X')) {
          base = 16;
          start = 2;
        } else if (num.size() > 2 && num[0] == '0' &&
                   (num[1] == 'b' || num[1] == 'B')) {
          base = 2;
          start = 2;
        }
        if (start == num.size())
          throw Error(l, "malformed integer literal");
        for (size_t q = start; q < num.size(); ++q) {
          char z = num[q];
          bool ok = base == 16  ? std::isxdigit((unsigned char)z)
                    : base == 2 ? (z == '0' || z == '1')
                                : std::isdigit((unsigned char)z);
          if (!ok)
            throw Error(l, "malformed integer literal '" + num + "'");
        }
      } else if (c == '"' || c == '\'') {
        char q = c;
        k = q == '"' ? Token::String : Token::Character;
        advance(s[i++]);
        bool closed = false;
        while (i < s.size()) {
          char x = s[i];
          if (x == '\n')
            throw Error(l, "newline in literal");
          if (x == '\\') {
            advance(s[i++]);
            if (i >= s.size())
              break;
            advance(s[i++]);
            continue;
          }
          advance(s[i++]);
          if (x == q) {
            closed = true;
            break;
          }
        }
        if (!closed)
          throw Error(l, "unterminated literal");
      } else {
        static const char *ops[] = {
            "<<=", ">>=", "...", "==", "!=", "<=", ">=", "&&",
            "||",  "++",  "--",  "->", "<<", ">>", "+=", "-=",
            "*=",  "/=",  "%=",  "&=", "|=", "^="};
        bool found = false;
        for (auto op : ops) {
          size_t n = std::char_traits<char>::length(op);
          if (s.compare(i, n, op) == 0) {
            for (size_t z = 0; z < n; z++)
              advance(s[i++]);
            found = true;
            break;
          }
        }
        if (!found) {
          if (std::string("{}[]();,:.?~!+-*/%<>&^|=#").find(c) ==
              std::string::npos)
            throw Error(l, std::string("invalid character '") + c + "'");
          advance(s[i++]);
        }
      }
      v.push_back({s.substr(b, i - b), l, k});
    }
    return v;
  }
  static int number(std::string s) {
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
      return (int)std::strtol(s.c_str() + 2, nullptr, 16);
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B'))
      return (int)std::strtol(s.c_str() + 2, nullptr, 2);
    return (int)std::strtol(s.c_str(), nullptr, 10);
  }
  std::vector<Token> expand(const std::vector<Token> &in, int depth = 0) {
    if (depth > 64)
      throw Error(in.empty() ? Location{} : in[0].loc,
                  "macro expansion depth exceeded");
    std::vector<Token> out;
    for (size_t i = 0; i < in.size(); ++i) {
      auto it = in[i].kind == Token::Identifier ? macros.find(in[i].text)
                                                : macros.end();
      if (it == macros.end()) {
        out.push_back(in[i]);
        continue;
      }
      Macro m = it->second;
      Location callloc = in[i].loc;
      std::vector<std::vector<Token>> args;
      if (m.function) {
        if (i + 1 >= in.size() || in[i + 1].text != "(") {
          out.push_back(in[i]);
          continue;
        }
        i += 2;
        args.emplace_back();
        int paren = 0, bracket = 0, brace = 0;
        bool done = false;
        for (; i < in.size(); ++i) {
          if (in[i].text == "(") {
            ++paren;
            args.back().push_back(in[i]);
          } else if (in[i].text == ")") {
            if (!paren && !bracket && !brace) {
              done = true;
              break;
            }
            if (paren)
              --paren;
            args.back().push_back(in[i]);
          } else if (in[i].text == "[") {
            ++bracket;
            args.back().push_back(in[i]);
          } else if (in[i].text == "]") {
            if (bracket)
              --bracket;
            args.back().push_back(in[i]);
          } else if (in[i].text == "{") {
            ++brace;
            args.back().push_back(in[i]);
          } else if (in[i].text == "}") {
            if (brace)
              --brace;
            args.back().push_back(in[i]);
          } else if (in[i].text == "," && !paren && !bracket && !brace)
            args.emplace_back();
          else
            args.back().push_back(in[i]);
        }
        if (!done)
          throw Error(in[i - 1].loc, "unterminated macro invocation");
        if (args.size() == 1 && args[0].empty() && m.params.empty())
          args.clear();
        if (args.size() != m.params.size())
          throw Error(in[i].loc, "wrong number of macro arguments");
      }
      std::vector<Token> repl;
      for (size_t j = 0; j < m.body.size(); ++j) {
        auto t = m.body[j];
        bool substituted = false;
        if (t.kind == Token::Identifier && m.function) {
          for (size_t p = 0; p < m.params.size(); p++)
            if (t.text == m.params[p]) {
              auto av = expand(args[p], depth + 1);
              for (auto x : av) {
                x.loc = callloc;
                repl.push_back(x);
              }
              substituted = true;
              break;
            }
        }
        if (!substituted) {
          t.loc = callloc;
          repl.push_back(t);
        }
      }
      auto ex = expand(repl, depth + 1);
      out.insert(out.end(), ex.begin(), ex.end());
    }
    return out;
  }
  bool cond(const std::vector<Token>
                &t) { // deliberately compact integer/defined evaluator
    size_t p = 0;
    std::function<long long(int)> expr;
    auto precedence = [](const std::string &o) {
      if (o == "||")
        return 1;
      if (o == "&&")
        return 2;
      if (o == "|")
        return 3;
      if (o == "^")
        return 4;
      if (o == "&")
        return 5;
      if (o == "==" || o == "!=")
        return 6;
      if (o == "<" || o == ">" || o == "<=" || o == ">=")
        return 7;
      if (o == "<<" || o == ">>")
        return 8;
      if (o == "+" || o == "-")
        return 9;
      if (o == "*" || o == "/" || o == "%")
        return 10;
      return 0;
    };
    std::function<long long()> atom = [&]() -> long long {
      if (p >= t.size())
        return 0;
      auto x = t[p++].text;
      if (x == "!")
        return !atom();
      if (x == "~")
        return ~atom();
      if (x == "+")
        return atom();
      if (x == "-")
        return -atom();
      if (x == "(") {
        auto z = expr(1);
        if (p < t.size() && t[p].text == ")")
          ++p;
        return z;
      }
      if (x == "defined") {
        bool par = p < t.size() && t[p].text == "(";
        if (par)
          ++p;
        std::string n = p < t.size() ? t[p++].text : "";
        if (par && p < t.size() && t[p].text == ")")
          ++p;
        return macros.count(n) != 0;
      }
      if (p == 0 && x == "0")
        return 0;
      if (x.size() && std::isdigit((unsigned char)x[0]))
        return number(x);
      auto it = macros.find(x);
      if (it != macros.end() && !it->second.function &&
          !it->second.body.empty() && it->second.body[0].kind == Token::Number)
        return number(it->second.body[0].text);
      return 0;
    };
    expr = [&](int minp) -> long long {
      long long a = atom();
      while (p < t.size()) {
        std::string o = t[p].text;
        int pr = precedence(o);
        if (pr < minp)
          break;
        ++p;
        long long b = expr(pr + 1);
        if (o == "||")
          a = a || b;
        else if (o == "&&")
          a = a && b;
        else if (o == "|")
          a |= b;
        else if (o == "^")
          a ^= b;
        else if (o == "&")
          a &= b;
        else if (o == "==")
          a = a == b;
        else if (o == "!=")
          a = a != b;
        else if (o == "<")
          a = a < b;
        else if (o == ">")
          a = a > b;
        else if (o == "<=")
          a = a <= b;
        else if (o == ">=")
          a = a >= b;
        else if (o == "<<")
          a <<= b & 31;
        else if (o == ">>")
          a >>= b & 31;
        else if (o == "+")
          a += b;
        else if (o == "-")
          a -= b;
        else if (o == "*")
          a *= b;
        else if (o == "/")
          a = b ? a / b : 0;
        else if (o == "%")
          a = b ? a % b : 0;
      }
      return a;
    };
    std::vector<Token> protectedTokens;
    for (size_t i = 0; i < t.size(); ++i) {
      if (t[i].text == "defined") {
        Location l = t[i].loc;
        bool par = i + 1 < t.size() && t[i + 1].text == "(";
        if (par)
          ++i;
        std::string name = i + 1 < t.size() ? t[++i].text : "";
        if (par && i + 1 < t.size() && t[i + 1].text == ")")
          ++i;
        protectedTokens.push_back(
            {macros.count(name) ? "1" : "0", l, Token::Number});
      } else
        protectedTokens.push_back(t[i]);
    }
    auto e = expand(protectedTokens);
    return expr(1) != 0;
  }
  std::vector<Token> file(const std::filesystem::path &input, int depth = 0) {
    if (depth > 32)
      throw Error({input.string(), 1, 1}, "include nesting exceeds 32 files");
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(input, ec);
    if (ec)
      path = std::filesystem::absolute(input);
    if (std::find(stack.begin(), stack.end(), path) != stack.end()) {
      std::ifstream probe(path);
      std::string a, b;
      bool first = false;
      while (std::getline(probe, a)) {
        a = trim(a);
        if (a.empty() || a.rfind("//", 0) == 0 || a.rfind("/*", 0) == 0)
          continue;
        if (!first) {
          first = true;
          if (a.rfind("#ifndef", 0) != 0)
            break;
          std::string guard = trim(a.substr(7));
          while (std::getline(probe, b)) {
            b = trim(b);
            if (b.empty())
              continue;
            if (b == "#define " + guard && macros.count(guard))
              return {};
            break;
          }
          break;
        }
      }
      throw Error({path.string(), 1, 1},
                  "include cycle detected (no active include guard)");
    }
    std::ifstream f(path);
    if (!f)
      throw Error({path.string(), 1, 1}, "cannot open source file");
    stack.push_back(path);
    std::vector<Token> out;
    struct C {
      bool parent, active, taken, els;
    };
    std::vector<C> cs;
    bool active = true;
    std::string raw, line;
    int lineno = 0;
    while (std::getline(f, line)) {
      ++lineno;
      while (!line.empty() && line.back() == '\\') {
        line.pop_back();
        std::string more;
        if (!std::getline(f, more))
          break;
        ++lineno;
        line += more;
      }
      std::string t = trim(line);
      if (!blockComment && !t.empty() && t[0] == '#') {
        std::string d = trim(t.substr(1));
        size_t n = 0;
        while (n < d.size() && ident(d[n]))
          ++n;
        std::string cmd = d.substr(0, n), arg = trim(d.substr(n));
        Location loc{path.string(), lineno, 1};
        if (cmd == "ifdef" || cmd == "ifndef" || cmd == "if") {
          bool yes = false;
          if (cmd == "ifdef" || cmd == "ifndef") {
            auto z = lex(arg, path.string(), lineno);
            std::string name = z.empty() ? "" : z[0].text;
            yes = macros.count(name);
            if (cmd == "ifndef")
              yes = !yes;
          } else
            yes = cond(lex(arg, path.string(), lineno));
          bool par = active;
          cs.push_back({par, par && yes, par && yes, false});
          active = cs.back().active;
          continue;
        }
        if (cmd == "else" || cmd == "elif") {
          if (cs.empty() || cs.back().els)
            throw Error(loc, "unmatched or duplicate #else/#elif");
          auto &c = cs.back();
          c.els = cmd == "else";
          bool yes =
              cmd == "else" ? true : cond(lex(arg, path.string(), lineno));
          c.active = c.parent && !c.taken && yes;
          c.taken = c.taken || yes;
          active = c.active;
          continue;
        }
        if (cmd == "endif") {
          if (cs.empty())
            throw Error(loc, "unmatched #endif");
          bool par = cs.back().parent;
          cs.pop_back();
          active = par;
          continue;
        }
        if (!active)
          continue;
        if (cmd == "define") {
          size_t k = 0;
          while (k < arg.size() && ident(arg[k]))
            ++k;
          if (!k)
            throw Error(loc, "expected macro name");
          std::string name = arg.substr(0, k);
          Macro m;
          size_t q = k;
          if (q < arg.size() && arg[q] == '(') {
            m.function = true;
            ++q;
            while (q < arg.size() && arg[q] != ')') {
              while (q < arg.size() &&
                     (std::isspace((unsigned char)arg[q]) || arg[q] == ','))
                ++q;
              if (q < arg.size() && arg[q] == ')')
                break;
              size_t b = q;
              while (q < arg.size() && ident(arg[q]))
                ++q;
              if (b == q)
                throw Error(loc, "invalid macro parameter list");
              m.params.push_back(arg.substr(b, q - b));
            }
            if (q >= arg.size() || arg[q] != ')')
              throw Error(loc, "unterminated macro parameter list");
            ++q;
          }
          m.body = lex(arg.substr(q), path.string(), lineno);
          macros[name] = std::move(m);
          continue;
        }
        if (cmd == "undef") {
          auto z = lex(arg, path.string(), lineno);
          if (!z.empty())
            macros.erase(z[0].text);
          continue;
        }
        if (cmd == "include") {
          std::string h = trim(arg);
          if (h.size() < 2 || (h[0] != '"' && h[0] != '<'))
            throw Error(loc, "expected quoted or angle include path");
          char end = h[0] == '"' ? '"' : '>';
          size_t e = h.find(end, 1);
          if (e == h.npos)
            throw Error(loc, "unterminated include path");
          std::filesystem::path name = h.substr(1, e - 1);
          std::vector<std::filesystem::path> tries;
          if (h[0] == '"')
            tries.push_back(path.parent_path() / name);
          for (auto &d : opt.includes)
            tries.push_back(d / name);
          tries.push_back(path.parent_path() / name);
          bool found = false;
          for (auto &x : tries)
            if (std::filesystem::exists(x)) {
              auto more = file(x, depth + 1);
              out.insert(out.end(), more.begin(), more.end());
              found = true;
              break;
            }
          if (!found)
            throw Error(loc, "include file not found: " + name.string());
          continue;
        }
        if (cmd.empty())
          continue;
        throw Error(loc, "unknown preprocessor directive #" + cmd);
      }
      if (active) {
        auto z = expand(lex(line, path.string(), lineno));
        out.insert(out.end(), z.begin(), z.end());
      }
    }
    if (blockComment)
      throw Error({path.string(), lineno ? lineno : 1, 1},
                  "unterminated comment");
    if (!cs.empty())
      throw Error({path.string(), lineno ? lineno : 1, 1},
                  "unterminated conditional directive");
    stack.pop_back();
    return out;
  }
};
} // namespace
std::vector<Token> preprocess(const std::filesystem::path &p,
                              const PreprocessorOptions &o) {
  PP pp;
  pp.opt = o;
  for (auto &d : o.defines) {
    Macro m;
    m.body = pp.lex(d.second, "<command-line>", 1);
    pp.macros[d.first] = std::move(m);
  }
  auto t = pp.file(p);
  t.push_back({"", {p.string(), 1, 1}, Token::End});
  return t;
}
} // namespace mupc
