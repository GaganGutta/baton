#include <iostream>
#include <span>
#include <string_view>

#include "common/version.h"

namespace {

constexpr std::string_view kUsage =
    "baton - a durable job and workflow engine\n"
    "\n"
    "Usage: baton [--version] [--help]\n"
    "\n"
    "The server is under construction; see PROGRESS.md.\n";

}  // namespace

int main(int argc, char** argv) {
  const std::span<char*> args(argv, static_cast<size_t>(argc));
  for (const std::string_view arg : args.subspan(1)) {
    if (arg == "--version") {
      std::cout << "baton " << baton::kVersion << '\n';
      return 0;
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << kUsage;
      return 0;
    }
    std::cerr << "baton: unknown argument '" << arg << "' (try --help)\n";
    return 2;
  }
  std::cout << kUsage;
  return 0;
}
