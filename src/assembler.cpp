#include "mupc/assembler.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace mupc {
namespace {

struct InstDef {
  unsigned opcode;
  bool sub;
  unsigned subop;
};

const std::map<std::string, InstDef> &instructions() {
  static const std::map<std::string, InstDef> table{
      {"mov", {0x0, false, 0}}, {"add", {0x1, false, 0}},
      {"sub", {0x2, false, 0}}, {"and", {0x3, false, 0}},
      {"or", {0x4, false, 0}},  {"xor", {0x5, false, 0}},
      {"shr", {0x6, false, 0}}, {"idc", {0x7, false, 0}},
      {"sjm", {0x8, false, 0}}, {"je", {0x9, false, 0}},
      {"jg", {0xa, false, 0}},  {"jl", {0xb, false, 0}},
      {"mst", {0xc, false, 0}}, {"io", {0xd, false, 0}},
      {"msc", {0xe, false, 0}}, {"imm", {0xf, false, 0}},
      {"inc", {0x7, true, 0}},  {"dec", {0x7, true, 1}},
      {"adc", {0x7, true, 2}},  {"sbc", {0x7, true, 3}},
      {"jmp", {0x8, true, 0}},  {"jz", {0x8, true, 1}},
      {"jnz", {0x8, true, 2}},  {"jc", {0x8, true, 3}},
      {"rdm", {0xc, true, 0}},  {"wrm", {0xc, true, 1}},
      {"pop", {0xc, true, 2}},  {"push", {0xc, true, 3}},
      {"in", {0xd, true, 0}},   {"out", {0xd, true, 1}},
      {"ldo", {0xe, true, 0}},  {"rnd", {0xe, true, 1}},
      {"sdw", {0xe, true, 2}},  {"rbt", {0xe, true, 3}},
      {"ldi", {0xf, true, 0}},  {"adi", {0xf, true, 1}},
  };
  return table;
}

struct Token {
  enum Kind { Word, Number, String, Character, Label, Reference, Comma } kind;
  std::string text;
};

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool ident_start(char c) {
  const auto u = static_cast<unsigned char>(c);
  return std::isalpha(u) || c == '_';
}
bool ident_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return std::isalnum(u) || c == '_';
}

std::vector<Token> lex(const std::string &source) {
  std::vector<Token> out;
  for (std::size_t i = 0; i < source.size();) {
    const char c = source[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == ';') {
      while (i < source.size() && source[i] != '\n')
        ++i;
      continue;
    }
    if (c == ',') {
      out.push_back({Token::Comma, ","});
      ++i;
      continue;
    }
    if (c == '.' || c == ':') {
      const auto kind = c == '.' ? Token::Label : Token::Reference;
      ++i;
      const auto start = i;
      if (i == source.size() || !ident_start(source[i]))
        throw std::runtime_error("expected label name after '.' or ':'");
      while (i < source.size() && ident_char(source[i]))
        ++i;
      out.push_back({kind, source.substr(start, i - start)});
      continue;
    }
    if (c == '"' || c == '\'') {
      const char quote = c;
      ++i;
      std::string value;
      bool closed = false;
      while (i < source.size()) {
        char ch = source[i++];
        if (ch == quote) {
          closed = true;
          break;
        }
        if (ch == '\\') {
          if (i == source.size())
            throw std::runtime_error("unterminated escape sequence");
          ch = source[i++];
          switch (ch) {
          case 'n':
            value.push_back('\n');
            break;
          case 't':
            value.push_back('\t');
            break;
          case 'r':
            value.push_back('\r');
            break;
          case 'b':
            value.push_back('\b');
            break;
          case '0':
            value.push_back('\0');
            break;
          case '\\':
            value.push_back('\\');
            break;
          case '\'':
            value.push_back('\'');
            break;
          case '"':
            value.push_back('"');
            break;
          default:
            throw std::runtime_error("unsupported assembly escape sequence");
          }
        } else {
          value.push_back(ch);
        }
      }
      if (!closed)
        throw std::runtime_error("unterminated string or character literal");
      if (quote == '\'') {
        if (value.size() != 1)
          throw std::runtime_error("character literal must contain one byte");
        out.push_back({Token::Character, value});
      } else {
        out.push_back({Token::String, value});
      }
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(c))) {
      const auto start = i++;
      if (c == '0' && i < source.size() &&
          (source[i] == 'x' || source[i] == 'X' || source[i] == 'b' ||
           source[i] == 'B')) {
        ++i;
        while (i < source.size() &&
               std::isxdigit(static_cast<unsigned char>(source[i])))
          ++i;
      } else {
        while (i < source.size() &&
               std::isdigit(static_cast<unsigned char>(source[i])))
          ++i;
      }
      out.push_back({Token::Number, source.substr(start, i - start)});
      continue;
    }
    if (ident_start(c)) {
      const auto start = i++;
      while (i < source.size() && ident_char(source[i]))
        ++i;
      out.push_back({Token::Word, source.substr(start, i - start)});
      continue;
    }
    throw std::runtime_error(std::string("unexpected assembly character '") +
                             c + "'");
  }
  return out;
}

long long parse_number(const std::string &text) {
  try {
    std::size_t consumed = 0;
    int base = 10;
    std::string digits = text;
    if (digits.size() > 2 && digits[0] == '0' &&
        (digits[1] == 'x' || digits[1] == 'X')) {
      base = 16;
      digits.erase(0, 2);
    } else if (digits.size() > 2 && digits[0] == '0' &&
               (digits[1] == 'b' || digits[1] == 'B')) {
      base = 2;
      digits.erase(0, 2);
    }
    const auto value = std::stoll(digits, &consumed, base);
    if (consumed != digits.size())
      throw std::runtime_error("invalid numeric literal: " + text);
    return value;
  } catch (const std::exception &) {
    throw std::runtime_error("invalid numeric literal: " + text);
  }
}

struct Node {
  enum Kind { Instruction, Byte, Word, Label, Reference, Origin } kind;
  std::string name;
  std::vector<std::uint8_t> data;
  unsigned opcode = 0;
  bool sub = false;
  unsigned subop = 0;
  unsigned r1 = 0;
  unsigned r2 = 0;
  unsigned size = 0;
  int origin = 0;
};

unsigned reg_index(const Token &token) {
  if (token.kind != Token::Word)
    throw std::runtime_error("expected register a, b, c, or d");
  const auto reg = lower(token.text);
  if (reg == "a")
    return 0;
  if (reg == "b")
    return 1;
  if (reg == "c")
    return 2;
  if (reg == "d")
    return 3;
  throw std::runtime_error("unknown register: " + token.text);
}

std::vector<std::uint8_t> data_token(const Token &token) {
  if (token.kind == Token::String || token.kind == Token::Character) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(token.text.size());
    for (unsigned char c : token.text)
      bytes.push_back(c);
    return bytes;
  }
  if (token.kind == Token::Number) {
    const auto value = parse_number(token.text);
    if (value < 0 || value > 255)
      throw std::runtime_error("db value must be in the range 0..255");
    return {static_cast<std::uint8_t>(value)};
  }
  throw std::runtime_error("expected number, string, or character after db");
}

} // namespace

Image assemble(const std::string &source) {
  const auto tokens = lex(source);
  std::vector<Node> nodes;
  std::size_t i = 0;
  auto need = [&](Token::Kind kind,
                  const std::string &message) -> const Token & {
    if (i >= tokens.size() || tokens[i].kind != kind)
      throw std::runtime_error(message);
    return tokens[i++];
  };
  while (i < tokens.size()) {
    const Token token = tokens[i++];
    if (token.kind == Token::Label) {
      Node n{};
      n.kind = Node::Label;
      n.name = token.text;
      nodes.push_back(std::move(n));
    } else if (token.kind == Token::Reference) {
      Node n{};
      n.kind = Node::Reference;
      n.name = token.text;
      n.size = 4;
      nodes.push_back(std::move(n));
    } else if (token.kind == Token::Word) {
      const auto word = lower(token.text);
      if (word == "org") {
        const auto value = parse_number(
            need(Token::Number, "expected address after org").text);
        if (value < 0 || value > 65535)
          throw std::runtime_error("org address must be in the range 0..65535");
        Node n{};
        n.kind = Node::Origin;
        n.origin = static_cast<int>(value);
        nodes.push_back(std::move(n));
      } else if (word == "db") {
        Node n{};
        n.kind = Node::Byte;
        n.data = data_token(tokens.at(i++));
        n.size = static_cast<unsigned>(n.data.size());
        nodes.push_back(std::move(n));
      } else if (word == "dw") {
        const auto value =
            parse_number(need(Token::Number, "expected number after dw").text);
        if (value < 0 || value > 65535)
          throw std::runtime_error("dw value must be in the range 0..65535");
        Node n{};
        n.kind = Node::Word;
        n.data = {static_cast<std::uint8_t>((value >> 8) & 0xff),
                  static_cast<std::uint8_t>(value & 0xff)};
        n.size = 2;
        nodes.push_back(std::move(n));
      } else {
        const auto found = instructions().find(word);
        if (found == instructions().end())
          throw std::runtime_error("unknown assembly instruction/directive: " +
                                   token.text);
        Node n{};
        n.kind = Node::Instruction;
        n.opcode = found->second.opcode;
        n.sub = found->second.sub;
        n.subop = found->second.subop;
        n.size = 1;
        if (i < tokens.size() && tokens[i].kind == Token::Word &&
            (lower(tokens[i].text) == "a" || lower(tokens[i].text) == "b" ||
             lower(tokens[i].text) == "c" || lower(tokens[i].text) == "d")) {
          n.r1 = reg_index(tokens[i++]);
          if (i < tokens.size() && tokens[i].kind == Token::Comma) {
            ++i;
            n.r2 = reg_index(
                need(Token::Word, "expected second register after comma"));
          }
        } else if (i < tokens.size() && tokens[i].kind == Token::Comma) {
          throw std::runtime_error("unexpected comma after instruction");
        }
        nodes.push_back(std::move(n));
      }
    } else if (token.kind == Token::Comma) {
      throw std::runtime_error("unexpected comma");
    } else {
      throw std::runtime_error("unexpected data token; use db or dw");
    }
  }

  Image image;
  bool hasOrigin = false;
  for (const auto &node : nodes)
    if (node.kind == Node::Origin) {
      if (hasOrigin)
        throw std::runtime_error("org may only appear once");
      hasOrigin = true;
      image.loadAddress = node.origin;
    }

  std::size_t cursor = 0;
  std::size_t byteCount = 0;
  for (const auto &node : nodes) {
    switch (node.kind) {
    case Node::Label:
      if (static_cast<std::size_t>(image.loadAddress) + cursor > 65535)
        throw std::runtime_error(
            "label address is outside the 16-bit address space: " + node.name);
      if (!image.symbols
               .emplace(node.name,
                        static_cast<int>(image.loadAddress + cursor))
               .second)
        throw std::runtime_error("duplicate label: " + node.name);
      break;
    case Node::Origin:
      break;
    default:
      if (node.size > 65536 ||
          static_cast<std::size_t>(image.loadAddress) + cursor + node.size >
              65536 ||
          byteCount + node.size > 65536)
        throw std::runtime_error("assembly exceeds 65536-byte address space");
      cursor += node.size;
      byteCount += node.size;
      break;
    }
  }
  image.bytes.reserve(byteCount);
  for (const auto &node : nodes) {
    if (node.kind == Node::Label)
      continue;
    if (node.kind == Node::Origin)
      continue;
    if (node.kind == Node::Instruction) {
      unsigned a = node.sub ? node.subop : node.r1;
      unsigned b = node.sub ? node.r1 : node.r2;
      image.bytes.push_back(
          static_cast<std::uint8_t>((node.opcode << 4) | (a << 2) | b));
    } else if (node.kind == Node::Reference) {
      const auto found = image.symbols.find(node.name);
      if (found == image.symbols.end())
        throw std::runtime_error("undefined label: " + node.name);
      if (found->second < 0 || found->second > 65535)
        throw std::runtime_error(
            "label address is outside the address space: " + node.name);
      const auto address = static_cast<unsigned>(found->second);
      image.bytes.push_back(0xf2);
      image.bytes.push_back(static_cast<std::uint8_t>((address >> 8) & 0xff));
      image.bytes.push_back(0xf3);
      image.bytes.push_back(static_cast<std::uint8_t>(address & 0xff));
    } else {
      image.bytes.insert(image.bytes.end(), node.data.begin(), node.data.end());
    }
  }
  return image;
}

} // namespace mupc
