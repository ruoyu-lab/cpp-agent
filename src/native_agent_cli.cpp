#include "agent/app_api.hpp"

#include <iostream>

int main(int argc, char** argv) {
  std::vector<std::string> args;
  for (int index = 1; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  const auto result = agent::run_native_agent_cli(args, std::cout, std::cerr);
  return result.exit_code;
}
