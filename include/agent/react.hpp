#pragma once

#include "agent/runtime.hpp"

namespace agent {

class ReActParser {
 public:
  virtual ~ReActParser() = default;
  virtual ReActParserResult parse(const std::string& text,
                                  const ToolRegistry& tools,
                                  int iteration) const = 0;
};

class DefaultReActParser : public ReActParser {
 public:
  explicit DefaultReActParser(ReActParserOptions options = {});
  ReActParserResult parse(const std::string& text,
                          const ToolRegistry& tools,
                          int iteration) const override;

 private:
  ReActParserOptions options_;
};

struct ReActPromptBuilderInput {
  std::string system_prompt;
  std::vector<ChatToolDescriptor> tools;
};

std::string render_managed_react_rules_text();
std::string render_managed_react_tool_definitions_text(const std::vector<ChatToolDescriptor>& tools);
std::string render_react_visible_message(const std::string& text);

class ReActPromptBuilder {
 public:
  virtual ~ReActPromptBuilder() = default;
  virtual AgentMessage build_system_message(const ReActPromptBuilderInput& input) const = 0;
};

class DefaultReActPromptBuilder : public ReActPromptBuilder {
 public:
  AgentMessage build_system_message(const ReActPromptBuilderInput& input) const override;
};

class ReActObservationRenderer {
 public:
  virtual ~ReActObservationRenderer() = default;
  virtual AgentMessage render_tool_results(const std::vector<ToolExecutionResult>& results,
                                           int iteration) const = 0;
  virtual AgentMessage render_parse_error(const std::string& error,
                                          int iteration) const = 0;
  virtual AgentMessage render_final_rejection(const std::string& reason,
                                              int iteration) const = 0;
  virtual AgentMessage render_reasoning_protocol_leak(int iteration) const = 0;
};

class DefaultReActObservationRenderer : public ReActObservationRenderer {
 public:
  AgentMessage render_tool_results(const std::vector<ToolExecutionResult>& results,
                                   int iteration) const override;
  AgentMessage render_parse_error(const std::string& error,
                                  int iteration) const override;
  AgentMessage render_final_rejection(const std::string& reason,
                                      int iteration) const override;
  AgentMessage render_reasoning_protocol_leak(int iteration) const override;
};

struct FinalAnswerValidationInput {
  std::string user_request;
  Value input = Value();
  std::string final_answer;
  std::vector<ReActTraceEntry> react_trace;
  std::vector<ToolExecutionResult> completed_tool_results;
  std::vector<ToolDefinition> tools;
  int iteration = 0;
  Value runtime_context = Value::object({});
};

struct FinalAnswerValidationResult {
  bool accepted = true;
  std::string reason;
  Value metadata = Value::object({});
};

class FinalAnswerValidator {
 public:
  virtual ~FinalAnswerValidator() = default;
  virtual FinalAnswerValidationResult validate(const FinalAnswerValidationInput& input) const = 0;
};

class DefaultFinalAnswerValidator : public FinalAnswerValidator {
 public:
  FinalAnswerValidationResult validate(const FinalAnswerValidationInput& input) const override;
};

struct ReActLoopConfig {
  std::shared_ptr<ChatModelAdapter> model;
  ToolRegistry* tool_registry = nullptr;
  ToolExecutor* tool_executor = nullptr;
  EmbeddedContextManager* context_manager = nullptr;
  std::string system_prompt;
  int max_iterations = 8;
  int max_parse_errors = 2;
  EventBus* event_bus = nullptr;
  ExecutionPolicies execution_policies;
  HookSet hooks;
  TraceContext trace_context;
  std::shared_ptr<ReActParser> parser;
  ReActParserOptions parser_options;
  std::shared_ptr<ReActPromptBuilder> prompt_builder;
  std::shared_ptr<ReActObservationRenderer> observation_renderer;
  std::shared_ptr<FinalAnswerValidator> final_answer_validator;
  ReActPromptMode prompt_mode = ReActPromptMode::Managed;
  std::vector<ContextStatsBucketInput> prompt_stats_buckets;
  bool persist_visible_messages = false;
  std::optional<std::size_t> context_window_tokens;
  std::function<ContextTokenCounter(const SessionMemory&)> context_token_counter_resolver;
  std::function<void(const ContextStatsSnapshot&)> on_context_stats;
};

class ReActLoop {
 public:
  explicit ReActLoop(ReActLoopConfig config);

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
                               CancellationToken* cancellation,
                               AgentLoopStreamEventHandler on_event);
  std::vector<AgentMessage> build_prompt_messages(SessionMemory& session,
                                                  const std::string& input,
      const Value& input_value,
      int iteration,
      const std::vector<AgentMessage>& preface_messages,
      const Value& runtime_context,
      EmbeddedContextAssembly* context_assembly = nullptr) const;
  PromptAssembly build_prompt_assembly(SessionMemory& session,
                                       const std::string& input,
                                       const Value& input_value,
                                       int iteration,
                                       const std::vector<AgentMessage>& preface_messages,
                                       const Value& runtime_context,
                                       EmbeddedContextAssembly* context_assembly = nullptr) const;
  AgentOutput call_model(int iteration,
                           const std::vector<AgentMessage>& messages,
                           const ModelSettings& model_settings,
                           CancellationToken* cancellation = nullptr);
  AgentOutput call_model_stream(int iteration,
                                  const std::vector<AgentMessage>& messages,
                                  const ModelSettings& model_settings,
                                  CancellationToken* cancellation,
                                  AgentLoopStreamEventHandler on_event,
                                  const std::string& reasoning_id,
                                  std::string& turn_reasoning);
  ContextStatsSnapshot build_context_stats_snapshot(
      SessionMemory& session,
      const std::string& input,
      const Value& input_value,
      int iteration,
      const std::vector<AgentMessage>& preface_messages,
      const Value& runtime_context,
      const EmbeddedContextAssembly& context_assembly,
      const std::vector<AgentMessage>& prompt_messages) const;

  ReActLoopConfig config_;
};

}  // namespace agent
