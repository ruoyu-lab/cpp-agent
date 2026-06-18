#pragma once

#include "agent/hooks.hpp"

namespace agent {

// Out-of-process hook adapter. Spawns an executable, pipes the hook payload
// (as JSON) to stdin, and reads stdout. Exit code 0 -> allow; exit code 2 ->
// block with stderr as the reason; other non-zero codes -> allow + warn.
struct ProcessHookConfig {
  std::filesystem::path executable;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::chrono::milliseconds timeout{5000};
};

enum class ProcessHookDecision {
  Allow,
  Block,
  Warn,
};

struct ProcessHookResult {
  ProcessHookDecision decision = ProcessHookDecision::Allow;
  int exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;
};

// Runs a hook process synchronously given an arbitrary JSON-serialized payload.
// Returns Allow/Block/Warn. Unix-only (uses fork/exec/pipe).
ProcessHookResult run_process_hook(const ProcessHookConfig& config, const Value& payload);

// Convenience: returns a `before_tool` ToolHookContext callback that runs the
// process hook. If the hook blocks, throws ConfigurationError with the stderr
// as the reason so the executor surfaces it as a tool failure.
std::function<void(const ToolHookContext&)> make_process_tool_hook(ProcessHookConfig config);

}  // namespace agent
