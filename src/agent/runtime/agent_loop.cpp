#include "internal.hpp"
#include "compaction_planner.hpp"
#include "prompt_context_builder.hpp"
#include "run_state_codec.hpp"
#include "stream_event_reducer.hpp"
#include "tool_call_orchestrator.hpp"
#include "usage_aggregator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace agent {

std::string to_string(AgentLoopDurablePhase phase) {
  return RunStateCodec::loop_phase_to_string(phase);
}

Value agent_loop_durable_state_to_value(const AgentLoopDurableState& state) {
  return RunStateCodec::loop_to_value(state);
}

std::string to_string(AgentRunnerDurableStatus status) {
  return RunStateCodec::runner_status_to_string(status);
}

Value agent_runner_durable_state_to_value(const AgentRunnerDurableState& state) {
  return RunStateCodec::runner_to_value(state);
}

PromptAssembly internal::AgentLoop::build_prompt_assembly(SessionMemory& session, const std::string& input,
                                                          const Value& input_value,
                                                          int iteration,
                                                          const std::vector<AgentTraceEntry>& trace,
                                                          const std::vector<AgentMessage>& preface_messages,
                                                          const Value& runtime_context,
                                                          EmbeddedContextAssembly* context_assembly) const {
  auto built = PromptContextBuilder().build(PromptContextBuildOptions{
      .system_prompt = config_.system_prompt,
      .context_manager = config_.context_manager,
      .session = &session,
      .input = input,
      .input_value = input_value,
      .iteration = iteration,
      .trace_length = trace.size(),
      .preface_messages = preface_messages,
      .runtime_context = runtime_context,
  });
  if (context_assembly) {
    *context_assembly = std::move(built.context_assembly);
  }
  return std::move(built.assembly);
}

std::vector<AgentMessage> internal::AgentLoop::build_prompt_messages(SessionMemory& session, const std::string& input,
                                                                     const Value& input_value,
                                                                     int iteration,
                                                                     const std::vector<AgentTraceEntry>& trace,
                                                                     const std::vector<AgentMessage>& preface_messages,
                                                                     const Value& runtime_context,
                                                                     EmbeddedContextAssembly* context_assembly) const {
  return build_prompt_assembly(session, input, input_value, iteration, trace, preface_messages,
                               runtime_context, context_assembly).messages;
}

ContextStatsSnapshot internal::AgentLoop::build_context_stats_snapshot(
    SessionMemory& session,
    const std::vector<AgentMessage>& preface_messages,
    const EmbeddedContextAssembly& context_assembly,
    const std::vector<AgentMessage>& prompt_messages) const {
  ContextStatsAssembly assembly;
  assembly.session_id = session.session_id();
  assembly.context_window_tokens = config_.context_window_tokens;
  if (config_.context_token_counter_resolver) {
    assembly.counter = config_.context_token_counter_resolver(session);
  }

  auto append_message_bucket = [&](std::string id,
                                   std::string label,
                                   ContextStatsBucketKind kind,
                                   AgentMessage message,
                                   Value metadata = Value::object({})) {
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .messages = {std::move(message)},
        .metadata = std::move(metadata),
    });
  };

  auto append_messages_bucket = [&](std::string id,
                                    std::string label,
                                    ContextStatsBucketKind kind,
                                    std::vector<AgentMessage> messages,
                                    Value metadata = Value::object({})) {
    if (messages.empty()) {
      return;
    }
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .messages = std::move(messages),
        .metadata = std::move(metadata),
    });
  };

  auto append_tools_bucket = [&](std::string id,
                                 std::string label,
                                 ContextStatsBucketKind kind,
                                 std::vector<ChatToolDescriptor> tools,
                                 Value metadata = Value::object({})) {
    if (tools.empty()) {
      return;
    }
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .tools = std::move(tools),
        .metadata = std::move(metadata),
    });
  };

  auto append_stats_bucket = [&](ContextStatsBucketInput bucket) {
    if (bucket.text.empty() && bucket.messages.empty() && bucket.tools.empty()) {
      return;
    }
    assembly.buckets.push_back(std::move(bucket));
  };

  if (!config_.prompt_stats_buckets.empty()) {
    for (auto bucket : config_.prompt_stats_buckets) {
      append_stats_bucket(std::move(bucket));
    }
  } else if (!prompt_messages.empty() && prompt_messages.front().role == MessageRole::System) {
    append_message_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                          prompt_messages.front(),
                          Value::object({{"source", prompt_messages.front().metadata.at("source")}}));
  }

  std::vector<ChatToolDescriptor> regular_tools;
  std::vector<ChatToolDescriptor> mcp_tools;
  for (const auto& tool : config_.tool_registry->list()) {
    const bool is_mcp = tool.bundle == "mcp"
                        || std::find(tool.tags.begin(), tool.tags.end(), "mcp") != tool.tags.end();
    if (is_mcp) {
      mcp_tools.push_back(tool.descriptor());
    } else {
      regular_tools.push_back(tool.descriptor());
    }
  }
  append_tools_bucket("tool_definitions", "Tool Definitions", ContextStatsBucketKind::ToolDefinitions,
                      std::move(regular_tools),
                      Value::object({{"source", "native-tool-calling"}}));
  append_tools_bucket("mcp_tools", "MCP Tools", ContextStatsBucketKind::Mcp,
                      std::move(mcp_tools),
                      Value::object({{"source", "native-tool-calling"}}));

  for (std::size_t index = 0; index < preface_messages.size(); ++index) {
    const auto& message = preface_messages[index];
    const auto source = message.metadata.at("source").as_string();
    if (source == "skills-catalog" || source == "skill") {
      append_message_bucket("skills." + std::to_string(index), "Skills", ContextStatsBucketKind::Skills,
                            message, Value::object({{"source", source}}));
    } else if (source == "skill-fork-result") {
      append_message_bucket("subagents." + std::to_string(index), "Subagents",
                            ContextStatsBucketKind::Subagents, message,
                            Value::object({{"source", source}}));
    } else if (source == "mcp-prompt" || source == "mcp-resource") {
      append_message_bucket("mcp." + std::to_string(index), "MCP", ContextStatsBucketKind::Mcp,
                            message, Value::object({{"source", source}}));
    } else if (source == "long-term-memory") {
      append_message_bucket("memory." + std::to_string(index), "Memory", ContextStatsBucketKind::Memory,
                            message, Value::object({{"source", source}}));
    } else if (source == "knowledge-base" || source == "knowledge-base-manager") {
      append_message_bucket("knowledge." + std::to_string(index), "Knowledge",
                            ContextStatsBucketKind::Knowledge, message,
                            Value::object({{"source", source}}));
    } else if (source == "planner") {
      append_message_bucket("planning." + std::to_string(index), "Planning",
                            ContextStatsBucketKind::Planning, message,
                            Value::object({{"source", source}}));
    } else {
      append_message_bucket("other_preface." + std::to_string(index), "Other Preface",
                            ContextStatsBucketKind::Other, message,
                            Value::object({{"source", source}}));
    }
  }

  for (auto bucket : context_assembly.stats_buckets) {
    append_stats_bucket(std::move(bucket));
  }

  append_messages_bucket("conversation", "Conversation", ContextStatsBucketKind::Conversation,
                         session.get_messages(),
                         Value::object({{"source", "session-memory"}}));

  return estimate_context_stats(assembly);
}

AgentOutput internal::AgentLoop::call_model(int iteration, const std::vector<AgentMessage>& messages,
                                    const ModelSettings& model_settings,
                                    CancellationToken* cancellation) {
  const auto model_trace = child_or_root_trace_context(config_.trace_context);
  const Value request = Value::object({
      {"iteration", iteration},
      {"messageCount", messages.size()},
      {"toolCount", config_.tool_registry->descriptors().size()},
      {"model", model_settings.model},
  });
  if (config_.hooks.before_model) {
    ModelHookContext hook_context;
    hook_context.target = ExecutionTarget::Model;
    hook_context.trace_id = model_trace.trace_id;
    hook_context.run_id = model_trace.run_id;
    hook_context.workflow_run_id = model_trace.workflow_run_id;
    hook_context.request = request;
    config_.hooks.before_model(hook_context);
  }
  if (config_.event_bus) {
    config_.event_bus->publish("model.started", ExecutionTarget::Model,
                               Value::object({{"iteration", iteration}, {"messageCount", messages.size()}}),
                               model_trace);
  }
  try {
    AgentOutput response = execute_with_policies(
        ExecutionTarget::Model, config_.execution_policies, Value::object({{"iteration", iteration}}), cancellation,
        [&]() {
          return config_.model->generate(GenerateParams{
              .messages = messages,
              .tools = config_.tool_registry->descriptors(),
              .settings = config_.model->resolve_settings(model_settings),
              .cancellation = cancellation,
          });
        },
        [&](const RetryScheduledContext& retry) {
          if (config_.event_bus) {
            config_.event_bus->publish(
                "retry.scheduled", ExecutionTarget::Model,
                retry_scheduled_event_payload(retry, Value::object({{"iteration", iteration}})),
                model_trace);
          }
        });
    if (config_.event_bus) {
      config_.event_bus->publish("model.completed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"finishReason", response.finish_reason}}),
                                 model_trace);
      UsageAggregator::emit_cache_stats(*config_.event_bus, response, model_settings, model_trace);
    }
    if (config_.hooks.after_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.response = model_response_hook_value(response);
      config_.hooks.after_model(hook_context);
    }
    return response;
  } catch (const std::exception& error) {
    if (config_.hooks.on_model_error) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.error = error.what();
      config_.hooks.on_model_error(hook_context);
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.failed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"error", error.what()}}),
                                 model_trace);
    }
    throw;
  }
}

AgentLoopRunResult internal::AgentLoop::run(SessionMemory& session, const std::string& input,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  return run_input(session, create_message(MessageRole::User, input), Value(input), model_settings,
                   preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation);
}

AgentLoopRunResult internal::AgentLoop::run(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_parts);
  return run_input(session, user_input_message(std::move(input_parts)), Value(input_text), model_settings,
                   preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation);
}

AgentLoopRunResult internal::AgentLoop::run(SessionMemory& session, AgentMessage input_message,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  const Value input_value = agent_message_to_value(input_message);
  return run_input(session, std::move(input_message), input_value, model_settings, preface_messages,
                   std::move(tool_context), std::move(runtime_context), std::move(durable_options),
                   cancellation);
}

AgentLoopRunResult internal::AgentLoop::run_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        AgentLoopDurableOptions durable_options,
                                        CancellationToken* cancellation) {
  if (durable_options.resume_state) {
    input_message = input_message_from_durable_state(*durable_options.resume_state);
    input_value = durable_options.resume_state->input_value.is_null()
                      ? agent_message_to_value(input_message)
                      : durable_options.resume_state->input_value;
  }
  const std::string input_text = extract_text_content(input_message.content);
  const auto input_parts = input_message.content;
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  std::vector<AgentTraceEntry> trace = durable_options.resume_state ? durable_options.resume_state->trace
                                                                    : std::vector<AgentTraceEntry>{};
  AgentOutput last_response;
  bool has_last_response = false;
  std::string last_assistant_text = durable_options.resume_state
                                        ? durable_options.resume_state->last_assistant_text
                                        : std::string{};
  int start_iteration = 0;
  std::optional<AgentOutput> resume_pending_model_response;
  if (durable_options.resume_state) {
    const auto& resume = *durable_options.resume_state;
    session.restore(resume.session);
    start_iteration = std::max(0, resume.next_iteration);
    if (resume.last_response) {
      last_response = *resume.last_response;
      has_last_response = true;
      if (resume.phase == AgentLoopDurablePhase::ModelCompleted) {
        resume_pending_model_response = *resume.last_response;
      }
    }
  } else {
    session.add(input_message);
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
  }

  auto checkpoint = [&](AgentLoopDurablePhase phase,
                        int next_iteration,
                        std::optional<AgentLoopTerminationReason> termination_reason = std::nullopt) {
    if (!durable_options.on_checkpoint) {
      return;
    }
    const AgentOutput* checkpoint_response = has_last_response ? &last_response : nullptr;
    durable_options.on_checkpoint(RunStateCodec::build_loop_checkpoint(
        phase, session, next_iteration, input_text, input_parts, input_message, input_value,
        trace, checkpoint_response, last_assistant_text, termination_reason));
  };

  if (!durable_options.resume_state) {
    checkpoint(AgentLoopDurablePhase::BeforeModel, 0);
  }

  for (int iteration = start_iteration; iteration < config_.max_iterations; ++iteration) {
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    CompactionPlanner::maybe_auto_compact(session, config_.event_bus, config_.trace_context);
    EmbeddedContextAssembly context_assembly;
    const auto prompt_messages = build_prompt_messages(session, input_text, input_value, iteration, trace,
                                                       preface_messages, runtime_context,
                                                       &context_assembly);
    const bool resumed_model_response = resume_pending_model_response.has_value();
    if (!resumed_model_response) {
      const auto context_stats = build_context_stats_snapshot(session, preface_messages,
                                                              context_assembly,
                                                              prompt_messages);
      if (config_.on_context_stats) {
        config_.on_context_stats(context_stats);
      }
      if (config_.event_bus) {
        Value payload = context_stats_snapshot_to_value(context_stats);
        payload["iteration"] = iteration;
        config_.event_bus->publish("context.stats", ExecutionTarget::Model, std::move(payload),
                                   child_or_root_trace_context(config_.trace_context, "native.context"));
      }
    }
    checkpoint(AgentLoopDurablePhase::BeforeModel, iteration);
    AgentOutput response = resumed_model_response ? *resume_pending_model_response
                                                    : call_model(iteration, prompt_messages, model_settings,
                                                                 cancellation);
    resume_pending_model_response.reset();
    last_response = response;
    has_last_response = true;
    if (!response.text.empty()) {
      last_assistant_text = response.text;
    }
    if (!resumed_model_response) {
      session.add(assistant_message_from_output(response));
      trace.push_back(AgentTraceEntry{.type = "model", .iteration = iteration, .response = response});
    }
    checkpoint(AgentLoopDurablePhase::ModelCompleted, iteration);

    if (response.tool_calls.empty()) {
      const auto termination = is_incomplete_finish_reason(response.finish_reason)
                                   ? AgentLoopTerminationReason::IncompleteResponse
                                   : AgentLoopTerminationReason::Completed;
      AgentLoopRunResult result{session.session_id(), iteration + 1, response.text, response, trace, {},
                                session.get_messages(), termination, {}, {}};
      result.usage = UsageAggregator::from_trace(result.trace);
      checkpoint(AgentLoopDurablePhase::Completed, iteration + 1, termination);
      return result;
    }

    auto tool_orchestration = ToolCallOrchestrator(*config_.tool_executor)
                                  .run_tool_calls(response.tool_calls, session, trace,
                                                  std::move(tool_context), iteration, cancellation);
    tool_context = std::move(tool_orchestration.tool_context);
    checkpoint(AgentLoopDurablePhase::ToolBatchCompleted, iteration + 1);
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
  }

  if (!has_last_response) {
    throw AgentFrameworkError("Agent loop reached the max iteration limit ("
                              + std::to_string(config_.max_iterations)
                              + ") without producing a model response.");
  }
  AgentLoopRunResult result{session.session_id(),
                            config_.max_iterations,
                            last_assistant_text,
                            last_response,
                            trace,
                            {},
                            session.get_messages(),
                            AgentLoopTerminationReason::MaxIterations, {}, {}};
  result.usage = UsageAggregator::from_trace(result.trace);
  checkpoint(AgentLoopDurablePhase::Completed, config_.max_iterations, AgentLoopTerminationReason::MaxIterations);
  return result;
}

AgentLoopStreamResult internal::AgentLoop::stream(SessionMemory& session, const std::string& input,
                                        AgentLoopStreamEventHandler on_event,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  return stream_input(session, create_message(MessageRole::User, input), Value(input), model_settings,
                      preface_messages, std::move(tool_context), std::move(runtime_context), cancellation,
                      std::move(on_event));
}

AgentLoopStreamResult internal::AgentLoop::stream(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                                        AgentLoopStreamEventHandler on_event,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_parts);
  return stream_input(session, user_input_message(std::move(input_parts)), Value(input_text), model_settings,
                      preface_messages, std::move(tool_context), std::move(runtime_context), cancellation,
                      std::move(on_event));
}

AgentLoopStreamResult internal::AgentLoop::stream(SessionMemory& session, AgentMessage input_message,
                                        AgentLoopStreamEventHandler on_event,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  const Value input_value = agent_message_to_value(input_message);
  return stream_input(session, std::move(input_message), input_value, model_settings, preface_messages,
                      std::move(tool_context), std::move(runtime_context), cancellation,
                      std::move(on_event));
}

AgentLoopStreamResult internal::AgentLoop::stream_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                              const ModelSettings& model_settings,
                                              const std::vector<AgentMessage>& preface_messages,
                                              ToolExecutionContext tool_context,
                                              Value runtime_context,
                                              CancellationToken* cancellation,
                                              AgentLoopStreamEventHandler on_event) {
  const std::string input_text = extract_text_content(input_message.content);
  std::vector<AgentTraceEntry> trace;
  std::uint64_t loop_sequence = 0;
  AgentLoopStreamEventHandler sequenced_on_event;
  if (on_event) {
    sequenced_on_event = [on_event = std::move(on_event), &loop_sequence](const AgentLoopStreamEvent& event) {
      auto sequenced = event;
      sequenced.schema_version = kAgentStreamEventSchemaVersion;
      sequenced.sequence = ++loop_sequence;
      on_event(sequenced);
    };
  }
  auto emit_stream_event = [&](AgentLoopStreamEvent event) {
    if (sequenced_on_event) {
      sequenced_on_event(event);
    }
  };
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  session.add(std::move(input_message));
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  AgentOutput last_response;
  std::string last_assistant_text;

  for (int iteration = 0; iteration < config_.max_iterations; ++iteration) {
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    const auto model_trace = child_or_root_trace_context(config_.trace_context);
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::IterationStart,
        .iteration = iteration,
    });
    CompactionPlanner::maybe_auto_compact(session, config_.event_bus, config_.trace_context);
    EmbeddedContextAssembly context_assembly;
    const auto prompt_messages = build_prompt_messages(session, input_text, input_value, iteration, trace,
                                                       preface_messages, runtime_context,
                                                       &context_assembly);
    const auto context_stats = build_context_stats_snapshot(session, preface_messages,
                                                            context_assembly,
                                                            prompt_messages);
    if (config_.on_context_stats) {
      config_.on_context_stats(context_stats);
    }
    if (config_.event_bus) {
      Value payload = context_stats_snapshot_to_value(context_stats);
      payload["iteration"] = iteration;
      config_.event_bus->publish("context.stats", ExecutionTarget::Model, std::move(payload),
                                 child_or_root_trace_context(config_.trace_context, "native.context"));
    }
    const Value model_request = Value::object({
        {"iteration", iteration},
        {"messageCount", prompt_messages.size()},
        {"toolCount", config_.tool_registry->descriptors().size()},
        {"model", model_settings.model},
        {"stream", true},
    });
    if (config_.hooks.before_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = model_request;
      config_.hooks.before_model(hook_context);
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.started", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"messageCount", prompt_messages.size()}}),
                                 model_trace);
    }

    ModelStreamReductionState stream_state;
    bool model_event_emitted = false;
    StreamEventReducer stream_reducer(config_.event_bus);

    auto stream_policies = config_.execution_policies;
    auto retry_it = stream_policies.retry.find(ExecutionTarget::Model);
    if (retry_it != stream_policies.retry.end()) {
      const auto retry_on = retry_it->second.retry_on;
      retry_it->second.retry_on = [retry_on, &model_event_emitted](const RetryContext& retry) {
        if (model_event_emitted) {
          return false;
        }
        return retry_on ? retry_on(retry) : false;
      };
    }

    try {
      (void)execute_with_policies(
          ExecutionTarget::Model, stream_policies, Value::object({{"iteration", iteration}}), cancellation,
          [&]() {
            std::vector<ModelStreamEvent> collected;
            config_.model->stream(GenerateParams{
                .messages = prompt_messages,
                .tools = config_.tool_registry->descriptors(),
                .settings = config_.model->resolve_settings(model_settings),
                .cancellation = cancellation,
            },
            [&](const ModelStreamEvent& event) {
              collected.push_back(event);
              model_event_emitted = true;
              stream_reducer.reduce_model_event(event, iteration, model_trace,
                                                emit_stream_event, stream_state);
            });
            return collected;
          },
          [&](const RetryScheduledContext& retry) {
            if (config_.event_bus) {
              config_.event_bus->publish(
                  "retry.scheduled", ExecutionTarget::Model,
                  retry_scheduled_event_payload(retry, Value::object({{"iteration", iteration},
                                                                      {"stream", true}})),
                  model_trace);
            }
          });
    } catch (const std::exception& error) {
      if (config_.hooks.on_model_error) {
        ModelHookContext hook_context;
        hook_context.target = ExecutionTarget::Model;
        hook_context.trace_id = model_trace.trace_id;
        hook_context.run_id = model_trace.run_id;
        hook_context.workflow_run_id = model_trace.workflow_run_id;
        hook_context.request = model_request;
        hook_context.error = error.what();
        config_.hooks.on_model_error(hook_context);
      }
      if (config_.event_bus) {
        config_.event_bus->publish("model.failed", ExecutionTarget::Model,
                                   Value::object({{"iteration", iteration}, {"error", error.what()}}),
                                   model_trace);
      }
      throw;
    }
    if (!stream_state.saw_response) {
      throw AdapterError("Model stream completed without a final response.");
    }
    AgentOutput response = stream_state.response;
    if (config_.event_bus) {
      config_.event_bus->publish("model.completed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"finishReason", response.finish_reason}}),
                                 model_trace);
      UsageAggregator::emit_cache_stats(*config_.event_bus, response, model_settings, model_trace);
    }
    if (config_.hooks.after_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = model_request;
      hook_context.response = model_response_hook_value(response);
      config_.hooks.after_model(hook_context);
    }

    last_response = response;
    if (!response.text.empty()) {
      last_assistant_text = response.text;
    }
    session.add(assistant_message_from_output(response));
    trace.push_back(AgentTraceEntry{.type = "model", .iteration = iteration, .response = response});

    if (response.tool_calls.empty()) {
      const auto termination = is_incomplete_finish_reason(response.finish_reason)
                                   ? AgentLoopTerminationReason::IncompleteResponse
                                   : AgentLoopTerminationReason::Completed;
      AgentLoopRunResult result{session.session_id(), iteration + 1, response.text, response, trace, {},
                                session.get_messages(), termination, {}, {}};
      result.usage = UsageAggregator::from_trace(result.trace);
      if (!response.text.empty()) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::UserVisibleDelta,
            .iteration = iteration,
            .provider = response.provider,
            .model = response.model,
            .delta = response.text,
            .text = response.text,
        });
      }
      emit_stream_event(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::Done,
          .iteration = iteration,
          .result = result,
      });
      return AgentLoopStreamResult{std::move(result)};
    }

    auto tool_orchestration = ToolCallOrchestrator(*config_.tool_executor)
                                  .stream_tool_calls(response.tool_calls, session, trace,
                                                     std::move(tool_context), iteration,
                                                     emit_stream_event, cancellation);
    tool_context = std::move(tool_orchestration.tool_context);
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
  }

  AgentLoopRunResult result{session.session_id(),
                            config_.max_iterations,
                            last_assistant_text,
                            last_response,
                            trace,
                            {},
                            session.get_messages(),
                            AgentLoopTerminationReason::MaxIterations, {}, {}};
  result.usage = UsageAggregator::from_trace(result.trace);
  emit_stream_event(AgentLoopStreamEvent{
      .type = AgentLoopStreamEventType::Done,
      .iteration = config_.max_iterations,
      .result = result,
  });
  return AgentLoopStreamResult{std::move(result)};
}


}  // namespace agent
