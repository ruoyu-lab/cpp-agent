#pragma once

#include "agent/runtime.hpp"

#include <optional>
#include <string>
#include <vector>

namespace agent {

struct PromptContextBuildOptions {
  std::string system_prompt;
  std::vector<AgentMessage> system_messages;
  Value system_metadata = Value::object({});
  EmbeddedContextManager* context_manager = nullptr;
  SessionMemory* session = nullptr;
  std::string input;
  Value input_value;
  int iteration = 0;
  std::optional<std::size_t> trace_length;
  std::vector<AgentMessage> preface_messages;
  Value runtime_context = Value::object({});
};

struct PromptContextBuildResult {
  PromptAssembly assembly;
  EmbeddedContextAssembly context_assembly;
};

class PromptContextBuilder {
 public:
  PromptContextBuildResult build(PromptContextBuildOptions options) const;
};

}  // namespace agent
