#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv) {
  bool addressesOnly = argc == 4 && std::string(argv[3]) == "--addresses-only";
  if (argc != 3 && !addressesOnly) {
    std::cerr
        << "usage: prepare-upstream INPUT.c OUTPUT.c [--addresses-only]\n";
    return 2;
  }
  std::ifstream in(argv[1]);
  if (!in) {
    std::cerr << "cannot open upstream source: " << argv[1] << "\n";
    return 2;
  }
  std::string s((std::istreambuf_iterator<char>(in)), {});
  if (!addressesOnly) {
    const std::string mov = "void mov_op(";
    const auto p = s.find(mov);
    if (p == std::string::npos) {
      std::cerr << "upstream mov_op anchor not found\n";
      return 2;
    }
    s.insert(p, "bool upstream_halted = false;\n");
    const std::string stop = "exit(EXIT_SUCCESS);";
    const auto q = s.find(stop);
    if (q == std::string::npos) {
      std::cerr << "upstream shutdown anchor not found\n";
      return 2;
    }
    s.replace(q, stop.size(), "upstream_halted = true; return;");
  }
  const std::string signedAddress = "(registers[2] << 8) | registers[3]";
  const std::string unsignedAddress =
      "(((uint16_t)(uint8_t)registers[2]) << 8) | (uint8_t)registers[3]";
  std::size_t n = 0, count = 0;
  while ((n = s.find(signedAddress, n)) != std::string::npos) {
    s.replace(n, signedAddress.size(), unsignedAddress);
    n += unsignedAddress.size();
    ++count;
  }
  if (count < 1) {
    std::cerr << "upstream address anchors not found\n";
    return 2;
  }
  std::ofstream out(argv[2]);
  if (!out) {
    std::cerr << "cannot write prepared upstream source\n";
    return 2;
  }
  out << s;
  return out ? 0 : 2;
}
