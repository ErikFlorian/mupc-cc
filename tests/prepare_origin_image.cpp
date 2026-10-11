#include <cstdint>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 4) {
    std::cerr << "usage: prepare-origin-image INPUT OUTPUT ADDRESS\n";
    return 2;
  }
  try {
    const unsigned long start = std::stoul(argv[3], nullptr, 0);
    if (start > 65535 || (start != 0 && start < 5))
      throw std::runtime_error("start address must be zero or at least five");
    std::ifstream in(argv[1], std::ios::binary);
    if (!in)
      throw std::runtime_error("cannot open input image");
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(in)), {});
    if (image.size() + start > 65536)
      throw std::runtime_error("image exceeds the 16-bit address space");
    std::vector<uint8_t> memoryImage(start + image.size(), 0);
    if (start != 0) {
      memoryImage[0] = 0xf2; // ldi c
      memoryImage[1] = static_cast<uint8_t>((start >> 8) & 0xff);
      memoryImage[2] = 0xf3; // ldi d
      memoryImage[3] = static_cast<uint8_t>(start & 0xff);
      memoryImage[4] = 0x80; // jmp
    }
    std::copy(image.begin(), image.end(), memoryImage.begin() + start);
    std::ofstream out(argv[2], std::ios::binary);
    if (!out)
      throw std::runtime_error("cannot open output image");
    out.write(reinterpret_cast<const char *>(memoryImage.data()),
              static_cast<std::streamsize>(memoryImage.size()));
    if (!out)
      throw std::runtime_error("failed writing output image");
  } catch (const std::exception &error) {
    std::cerr << "prepare-origin-image: " << error.what() << "\n";
    return 1;
  }
  return 0;
}
