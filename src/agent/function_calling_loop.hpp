#pragma once

#include "agent/runtime.hpp"

namespace agent::internal {

struct AgentLoopConfig {
  std::shared_ptr<ChatModelAdapter> model;
  ToolRegistry* tool_registry = nullptr;
  ToolExecutor* tool_executor = nullptr;
  EmbeddedContextManager* context_manager = nullptr;
  std::string system_prompt;
  int max_iterations = 8;
  EventBus* event_bus = nullptr;
  ExecutionPolicies execution_policies;
  HookSet hooks;
  TraceContext trace_context;
  std::vector<ContextStatsBucketInput> prompt_stats_buckets;
  std::optional<std::size_t> context_window_tokens;
  std::function<ContextTokenCounter(const SessionMemory&)> context_token_counter_resolver;
  std::function<void(const ContextStatsSnapshot&)> on_context_stats;
};

class AgentLoop {
 public:
  explicit AgentLoop(AgentLoopConfig config);
  AgentLoopRunResult run(SessionMemory& session, const std::string& input,
                         const ModelSettings& model_settings = {},
                         const std::vector<AgentMessage>& preface_messages = {},
                         ToolExecutionContext tool_context = {},
                         Value runtime_context = Value::object({}),
                         AgentLoopDurableOptions durable_options = {},
                         CancellationToken* cancellation = nullptr);
  AgentLoopRunResult run(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                         const ModelSettings& model_settings = {},
                         const std::vector<AgentMessage>& preface_messages = {},
                         ToolExecutionContext tool_context = {},
                         Value runtime_context = Value::object({}),
                         AgentLoopDurableOptions durable_options = {},
                         CancellationToken* cancellation = nullptr);
  AgentLoopRunResult run(SessionMemory& session, AgentMessage input_message,
                         const ModelSettings& model_settings = {},
                         const std::vector<AgentMessage>& preface_messages = {},
                         ToolExecutionContext tool_context = {},
                         Value runtime_context = Value::object({}),
                         AgentLoopDurableOptions durable_options = {},
                         CancellationToken* cancellation = nullptr);
  AgentLoopStreamResult stream(SessionMemory& session, const std::string& input,
                               AgentLoopStreamEventHandler on_event,
                               const ModelSettings& model_settings = {},
                               const std::vector<AgentMessage>& preface_messages = {},
                               ToolExecutionContext tool_context = {},
                               Value runtime_context = Value::object({}),
                               CancellationToken* cancellation = nullptr);
  AgentLoopStreamResult stream(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                               AgentLoopStreamEventHandler on_event,
                               const ModelSettings& model_settings = {},
                               const std::vector<AgentMessage>& preface_messages = {},
                               ToolExecutionContext tool_context = {},
                               Value runtime_context = Value::object({}),
                               CancellationToken* cancellation = nullptr);
  AgentLoopStreamResult stream(SessionMemory& session, AgentMessage input_message,
                               AgentLoopStreamEventHandler on_event,
                               const ModelSettings& model_settings = {},
                               const std::vector<AgentMessage>& preface_messages = {},
                               ToolExecutionContext tool_context = {},
                               Value runtime_context = Value::object({}),
                               CancellationToken* cancellation = nullptr);

 private:
  AgentLoopRunResult run_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                               const ModelSettings& model_settings,
                               const std::vector<AgentMessage>& preface_messages,
                               ToolExecutionContext tool_context,
                               Value runtime_context,
                               AgentLoopDurableOptions durable_options,
                               CancellationToken* cancellation);
  AgentLoopStreamResult stream_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                     const ModelSettings& model_settings,
                                     const std::vector<AgentMessage>& preface_messages,
                                     ToolExecutionContext tool_context,
                                     Value runtime_context,
                                     CancellationToken* cancellation,
                                     AgentLoopStreamEventHandler on_event);
  std::vector<AgentMessage> build_prompt_messages(SessionMemory& session, const std::string& input,
                                                  const Value& input_value,
                                                  int iteration,
                                                  const std::vector<AgentTraceEntry>& trace,
                                                  const std::vector<AgentMessage>& preface_messages,
                                                  const Value& runtime_context,
                                                  EmbeddedContextAssembly* context_assembly = nullptr) const;
  PromptAssembly build_prompt_assembly(SessionMemory& session, const std::string& input,
                                       const Value& input_value,
                                       int iteration,
                                       const std::vector<AgentTraceEntry>& trace,
                                       const std::vector<AgentMessage>& preface_messages,
                                       const Value& runtime_context,
                                       EmbeddedContextAssembly* context_assembly = nullptr) const;
  ContextStatsSnapshot build_context_stats_snapshot(
      SessionMemory& session,
      const std::vector<AgentMessage>& preface_messages,
      const EmbeddedContextAssembly& context_assembly,
      const std::vector<AgentMessage>& prompt_messages) const;
  AgentOutput call_model(int iteration, const std::vector<AgentMessage>& messages,
                           const ModelSettings& model_settings,
                           CancellationToken* cancellation = nullptr);

  AgentLoopConfig config_;
};

}  // namespace agent::internal
