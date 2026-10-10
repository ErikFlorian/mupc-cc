#pragma once
#include "assembler.hpp"
#include "frontend.hpp"
namespace mupc {
struct CompileResult {
  std::string assembly;
  Image image;
};
CompileResult compile(const Unit &);
} // namespace mupc
