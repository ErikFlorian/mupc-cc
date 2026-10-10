#include "mupc/compiler.hpp"
#include "mupc/frontend.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr const char *version = "mupc-cc 0.1.0";
struct Options {
  bool assembleOnly = false, showHelp = false, showVersion = false;
  fs::path input, output, assembly, map;
  bool haveOutput = false;
  bool haveOrigin = false;
  int origin = 0;
  mupc::PreprocessorOptions pp;
};
void usage(std::ostream &out) {
  out << "Usage:\n"
      << "  mupc-cc [options] input.mc\n"
      << "  mupc-cc --assemble [options] input.asm\n\n"
      << "Options:\n"
      << "  -o PATH             Output binary path (default: input stem + "
         ".bin)\n"
      << "  -S PATH             Also write generated assembly (compile mode)\n"
      << "  --map PATH          Write an alphabetical symbol map\n"
      << "  -I PATH             Add an include search directory\n"
      << "  -DNAME[=VALUE]      Define a preprocessor symbol (default value: "
         "1)\n"
      << "  --origin ADDRESS    Override the source origin (0..65535)\n"
      << "  --assemble          Assemble input MuPC assembly directly\n"
      << "  --help              Show this help\n"
      << "  --version           Show version\n";
}
int parseAddress(const std::string &s) {
  if (s.empty() || s[0] == '-')
    throw std::runtime_error("invalid address: " + s);
  int base = 10;
  std::string digits = s;
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    base = 16;
    digits = s.substr(2);
  } else if (s.size() > 2 && s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
    base = 2;
    digits = s.substr(2);
  }
  if (digits.empty())
    throw std::runtime_error("invalid address: " + s);
  size_t used = 0;
  unsigned long n = std::stoul(digits, &used, base);
  if (used != digits.size() || n > 65535)
    throw std::runtime_error("address must be in the range 0..65535: " + s);
  return static_cast<int>(n);
}
Options arguments(int argc, char **argv) {
  Options o;
  auto needValue = [&](int &i, const std::string &opt) -> std::string {
    if (i + 1 >= argc)
      throw std::runtime_error("missing value after " + opt);
    return argv[++i];
  };
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h")
      o.showHelp = true;
    else if (a == "--version")
      o.showVersion = true;
    else if (a == "--assemble")
      o.assembleOnly = true;
    else if (a == "-o") {
      o.output = needValue(i, a);
      o.haveOutput = true;
    } else if (a == "-S")
      o.assembly = needValue(i, a);
    else if (a == "--map")
      o.map = needValue(i, a);
    else if (a == "-I")
      o.pp.includes.emplace_back(needValue(i, a));
    else if (a.rfind("-I", 0) == 0 && a.size() > 2)
      o.pp.includes.emplace_back(a.substr(2));
    else if (a == "-D") {
      std::string d = needValue(i, a);
      size_t eq = d.find('=');
      std::string name = d.substr(0, eq),
                  val = eq == std::string::npos ? "1" : d.substr(eq + 1);
      if (name.empty() ||
          !(std::isalpha((unsigned char)name[0]) || name[0] == '_'))
        throw std::runtime_error("invalid define: " + d);
      for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_'))
          throw std::runtime_error("invalid define: " + d);
      o.pp.defines[name] = val;
    } else if (a.rfind("-D", 0) == 0 && a.size() > 2) {
      std::string d = a.substr(2);
      size_t eq = d.find('=');
      std::string name = d.substr(0, eq),
                  val = eq == std::string::npos ? "1" : d.substr(eq + 1);
      if (name.empty() ||
          !(std::isalpha((unsigned char)name[0]) || name[0] == '_'))
        throw std::runtime_error("invalid define: " + d);
      for (char c : name)
        if (!(std::isalnum((unsigned char)c) || c == '_'))
          throw std::runtime_error("invalid define: " + d);
      o.pp.defines[name] = val;
    } else if (a == "--origin") {
      o.origin = parseAddress(needValue(i, a));
      o.haveOrigin = true;
    } else if (!a.empty() && a[0] == '-')
      throw std::runtime_error("unknown option: " + a);
    else if (o.input.empty())
      o.input = a;
    else
      throw std::runtime_error("unexpected extra input: " + a);
  }
  if (!o.showHelp && !o.showVersion && o.input.empty())
    throw std::runtime_error("no input file specified");
  if (!o.haveOutput && !o.input.empty()) {
    o.output = o.input;
    o.output.replace_extension(".bin");
  }
  if (!o.assembly.empty() && o.assembleOnly)
    throw std::runtime_error("-S is only available when compiling source");
  return o;
}
std::string readText(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    throw std::runtime_error("cannot open input file: " + p.string());
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad())
    throw std::runtime_error("failed reading input file: " + p.string());
  return ss.str();
}
fs::path tempPath(const fs::path &target) {
  static unsigned seq = 0;
  return target.parent_path() /
         (target.filename().string() + ".tmp." +
          std::to_string(
              std::chrono::steady_clock::now().time_since_epoch().count()) +
          "." + std::to_string(std::random_device{}()) + "." +
          std::to_string(++seq));
}
template <class Writer> void atomicWrite(const fs::path &path, Writer writer) {
  if (path.empty())
    throw std::runtime_error("empty output path");
  if (!path.parent_path().empty())
    fs::create_directories(path.parent_path());
  fs::path tmp = tempPath(path);
  try {
    writer(tmp);
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec)
      throw std::runtime_error("cannot replace output '" + path.string() +
                               "': " + ec.message());
  } catch (...) {
    std::error_code ignored;
    fs::remove(tmp, ignored);
    throw;
  }
}
void writeImage(const fs::path &path, const mupc::Image &image) {
  atomicWrite(path, [&](const fs::path &tmp) {
    std::ofstream out(tmp, std::ios::binary);
    if (!out)
      throw std::runtime_error("cannot write output: " + tmp.string());
    if (!image.bytes.empty())
      out.write(reinterpret_cast<const char *>(image.bytes.data()),
                static_cast<std::streamsize>(image.bytes.size()));
    out.close();
    if (!out)
      throw std::runtime_error("failed writing output: " + path.string());
  });
}
void writeText(const fs::path &path, const std::string &text) {
  atomicWrite(path, [&](const fs::path &tmp) {
    std::ofstream out(tmp, std::ios::binary);
    if (!out)
      throw std::runtime_error("cannot write output: " + tmp.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    if (!out)
      throw std::runtime_error("failed writing output: " + path.string());
  });
}
void writeMap(const fs::path &path, const mupc::Image &image) {
  std::ostringstream s;
  for (const auto &kv : image.symbols)
    s << "0x" << std::uppercase << std::hex << std::setw(4) << std::setfill('0')
      << kv.second << " " << kv.first << "\n";
  writeText(path, s.str());
}
} // namespace
int main(int argc, char **argv) {
  try {
    Options o = arguments(argc, argv);
    if (o.showHelp) {
      usage(std::cout);
      return 0;
    }
    if (o.showVersion) {
      std::cout << version << "\n";
      return 0;
    }
    mupc::Image image;
    if (o.assembleOnly)
      image = mupc::assemble(readText(o.input));
    else {
      auto tokens = mupc::preprocess(o.input, o.pp);
      auto unit = mupc::parse(tokens);
      if (o.haveOrigin)
        unit.origin = o.origin;
      auto result = mupc::compile(unit);
      image = std::move(result.image);
      if (!o.assembly.empty())
        writeText(o.assembly, result.assembly);
    }
    writeImage(o.output, image);
    if (!o.map.empty())
      writeMap(o.map, image);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "mupc-cc: " << e.what() << "\n";
    return 1;
  }
}
