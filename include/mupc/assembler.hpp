#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace mupc {
struct Image {
  std::vector<uint8_t> bytes;
  std::map<std::string, int> symbols;
  int loadAddress = 0;
};
Image assemble(const std::string &source);
} // namespace mupc
