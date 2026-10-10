#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
namespace mupc {
struct Location {
  std::string file;
  int line = 1, column = 1;
  std::string str() const;
};
struct Error : std::runtime_error {
  explicit Error(const Location &l, const std::string &s)
      : std::runtime_error(l.str() + ": " + s) {}
};
struct Token {
  std::string text;
  Location loc;
  enum Kind { Identifier, Number, String, Character, Punct, End } kind = Punct;
};
struct PreprocessorOptions {
  std::vector<std::filesystem::path> includes;
  std::map<std::string, std::string> defines;
};
std::vector<Token> preprocess(const std::filesystem::path &,
                              const PreprocessorOptions & = {});
struct Type;
using Ty = std::shared_ptr<Type>;
struct Field {
  std::string name;
  Ty type;
  int offset = 0;
};
struct Type {
  enum Kind { Int, UInt, Void, Pointer, Array, Struct } kind = Int;
  Ty base;
  int count = 0;
  std::string name;
  std::vector<Field> fields;
  bool complete = false;
  int size() const;
};
Ty scalar(Type::Kind);
Ty pointer(Ty);
Ty array(Ty, int);
bool sameType(Ty, Ty);
struct Expr {
  enum Kind {
    Literal,
    StringLiteral,
    Name,
    Unary,
    Binary,
    Assign,
    Call,
    Index,
    Member,
    Cast,
    Sizeof,
    Conditional,
    Postfix,
    InitList
  } kind = Literal;
  Location loc;
  std::string text;
  int value = 0;
  Ty type;
  std::vector<std::shared_ptr<Expr>> args;
};
using Ex = std::shared_ptr<Expr>;
struct Decl {
  Location loc;
  std::string name;
  Ty type;
  Ex init;
  std::vector<Ex> initList;
  bool hasInitList = false;
};
struct Stmt {
  enum Kind {
    Block,
    Declaration,
    Expression,
    If,
    While,
    DoWhile,
    For,
    Return,
    Break,
    Continue,
    Label,
    Goto,
    Asm,
    Empty
  } kind = Empty;
  Location loc;
  std::string text;
  std::vector<std::shared_ptr<Stmt>> children;
  std::vector<Ex> exprs;
  std::vector<Decl> decls;
};
using St = std::shared_ptr<Stmt>;
struct Function {
  Location loc;
  std::string name;
  Ty result;
  std::vector<Decl> params;
  St body;
};
struct Unit {
  std::vector<Decl> globals;
  std::vector<Function> functions;
  int origin = 0;
  std::map<std::string, Ty> structs;
};
Unit parse(const std::vector<Token> &);
} // namespace mupc
