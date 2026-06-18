#include "agent/presets.hpp"

#include "agent/builtins.hpp"

#include <iterator>

namespace agent {
namespace {

std::vector<ToolDefinition> build_preset_tools(std::vector<std::string> bundles,
                                               std::vector<ToolDefinition> extra_tools) {
  auto tools = create_builtin_tools(std::move(bundles));
  tools.insert(tools.end(),
               std::make_move_iterator(extra_tools.begin()),
               std::make_move_iterator(extra_tools.end()));
  return tools;
}

AgentRuntimeBuilder build_runtime_from_preset(StandardRuntimePresetOptions options) {
  auto model = std::move(options.model);
  if (!model) {
    model = std::make_shared<EchoChatModelAdapter>();
  }

  auto builder = AgentRuntimeBuilder()
                     .model(std::move(model), std::move(options.model_settings))
                     .tools(build_preset_tools(std::move(options.tool_bundles),
                                               std::move(options.tools)))
                     .max_iterations(options.max_iterations)
                     .enable_planning(options.enable_planning)
                     .tool_services(std::move(options.tool_services))
                     .hooks(std::move(options.hooks))
                     .react(std::move(options.react));

  if (!options.context_sources.empty()) {
    builder.context_sources(std::move(options.context_sources));
  }
  if (!options.system_prompt.empty()) {
    builder.system_prompt(std::move(options.system_prompt));
  }
  if (options.session_store) {
    builder.session_store(std::move(options.session_store));
  }
  if (options.scratch_store) {
    builder.scratch_store(std::move(options.scratch_store));
  }
  if (options.long_term_memory) {
    builder.long_term_memory(std::move(options.long_term_memory));
  }
  if (options.knowledge) {
    builder.knowledge(options.knowledge, std::move(options.knowledge_retrieval));
  }
  if (options.permission_policy) {
    builder.permission_policy(std::move(options.permission_policy));
  }
  if (options.approval_handler) {
    builder.approval_handler(std::move(options.approval_handler));
  }
  if (options.event_bus) {
    builder.event_bus(options.event_bus);
  }
  if (options.planner) {
    builder.planner(std::move(options.planner));
  }

  return builder;
}

}  // namespace

AgentRuntimeBuilder create_standard_runtime(StandardRuntimePresetOptions options) {
  return build_runtime_from_preset(std::move(options));
}

AgentRuntimeBuilder create_local_model_runtime(LocalModelRuntimePresetOptions options) {
  if (!options.runtime.model) {
    if (options.llama_cpp.model_path.empty()) {
      throw ConfigurationError(
          "create_local_model_runtime requires runtime.model or llama_cpp.model_path.");
    }
    options.runtime.model =
        std::make_shared<LlamaCppNativeChatModelAdapter>(std::move(options.llama_cpp));
  }

  return build_runtime_from_preset(std::move(options.runtime));
}

}  // namespace agent
