#pragma once

#include "agent/config.hpp"
#include "agent/evals.hpp"
#include "agent/replay.hpp"

#include <iosfwd>

namespace agent {

struct NativeCliResult {
  int exit_code = 0;
};

NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::istream& stdin_stream,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream);
NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::istream& stdin_stream,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream,
                                     NativeConfigModuleLoader config_module_loader);
NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream);
NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream,
                                     NativeConfigModuleLoader config_module_loader);

}  // namespace agent
