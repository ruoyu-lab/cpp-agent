#include "internal.hpp"
#include "compaction_planner.hpp"
#include "memory_writeback.hpp"
#include "runner_kernel.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace agent {

AgentRunnerStreamResult RunnerStreaming::stream(const std::string& input,
                                            AgentRunnerStreamEventHandler on_event,
                                            const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  return runner_->stream_input(create_message(MessageRole::User, input), Value(input), true, session_id, model_settings,
                      std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                      std::move(context), std::move(knowledge_retrieval_options), cancellation, enable_planning,
                      std::move(on_event));
}

AgentRunnerStreamResult RunnerStreaming::stream(std::vector<MessageContentPart> input_parts,
                                            AgentRunnerStreamEventHandler on_event,
                                            const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  const std::string input_text = extract_text_content(input_parts);
  return runner_->stream_input(user_input_message(std::move(input_parts)), Value(input_text), true, session_id,
                      model_settings, std::move(retrieval_options), std::move(writeback_options),
                      std::move(skill_activations), std::move(context), std::move(knowledge_retrieval_options),
                      cancellation, enable_planning, std::move(on_event));
}

AgentRunnerStreamResult RunnerStreaming::stream(AgentMessage input_message,
                                            AgentRunnerStreamEventHandler on_event,
                                            const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  const Value input_value = agent_message_to_value(input_message);
  return runner_->stream_input(std::move(input_message), input_value, false, session_id, model_settings,
                      std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                      std::move(context), std::move(knowledge_retrieval_options), cancellation, enable_planning,
                      std::move(on_event));
}

AgentRunnerEventStream RunnerStreaming::events(const std::string& input,
                                                  StreamQueueOptions queue_options,
                                                  const std::string& session_id,
                                                  const ModelSettings& model_settings,
                                                  RunnerRetrievalOptions retrieval_options,
                                                  RunnerWritebackOptions writeback_options,
                                                  std::vector<SkillActivation> skill_activations,
                                                  Value context,
                                                  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                                  CancellationToken* cancellation,
                                                  bool enable_planning) {
  BoundedStreamQueue<AgentRunnerStreamEvent> queue(std::move(queue_options));
  auto producer_queue = queue;
  auto owned_cancellation = cancellation ? std::shared_ptr<CancellationToken>{}
                                         : std::make_shared<CancellationToken>();
  CancellationToken* effective_cancellation = cancellation ? cancellation : owned_cancellation.get();
  std::thread producer([owner = runner_,
                        input,
                        session_id,
                        model_settings,
                        retrieval_options = std::move(retrieval_options),
                        writeback_options = std::move(writeback_options),
                        skill_activations = std::move(skill_activations),
                        context = std::move(context),
                        knowledge_retrieval_options = std::move(knowledge_retrieval_options),
                        cancellation = effective_cancellation,
                        enable_planning,
                        producer_queue]() mutable {
    try {
      RunnerStreaming(*owner).stream(input,
             [producer_queue](const AgentRunnerStreamEvent& event) mutable {
               producer_queue.push(event);
             },
             session_id,
             model_settings,
             std::move(retrieval_options),
             std::move(writeback_options),
             std::move(skill_activations),
             std::move(context),
             std::move(knowledge_retrieval_options),
             cancellation,
             enable_planning);
      producer_queue.close();
    } catch (const StreamQueueClosed&) {
      producer_queue.close();
    } catch (...) {
      if (producer_queue.closed()) {
        producer_queue.close();
      } else {
        producer_queue.fail(std::current_exception());
      }
    }
  });
  return AgentRunnerEventStream(std::move(queue), std::move(producer), std::move(owned_cancellation));
}

AgentRunnerEventStream RunnerStreaming::events(std::vector<MessageContentPart> input_parts,
                                                  StreamQueueOptions queue_options,
                                                  const std::string& session_id,
                                                  const ModelSettings& model_settings,
                                                  RunnerRetrievalOptions retrieval_options,
                                                  RunnerWritebackOptions writeback_options,
                                                  std::vector<SkillActivation> skill_activations,
                                                  Value context,
                                                  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                                  CancellationToken* cancellation,
                                                  bool enable_planning) {
  BoundedStreamQueue<AgentRunnerStreamEvent> queue(std::move(queue_options));
  auto producer_queue = queue;
  auto owned_cancellation = cancellation ? std::shared_ptr<CancellationToken>{}
                                         : std::make_shared<CancellationToken>();
  CancellationToken* effective_cancellation = cancellation ? cancellation : owned_cancellation.get();
  std::thread producer([owner = runner_,
                        input_parts = std::move(input_parts),
                        session_id,
                        model_settings,
                        retrieval_options = std::move(retrieval_options),
                        writeback_options = std::move(writeback_options),
                        skill_activations = std::move(skill_activations),
                        context = std::move(context),
                        knowledge_retrieval_options = std::move(knowledge_retrieval_options),
                        cancellation = effective_cancellation,
                        enable_planning,
                        producer_queue]() mutable {
    try {
      RunnerStreaming(*owner).stream(std::move(input_parts),
             [producer_queue](const AgentRunnerStreamEvent& event) mutable {
               producer_queue.push(event);
             },
             session_id,
             model_settings,
             std::move(retrieval_options),
             std::move(writeback_options),
             std::move(skill_activations),
             std::move(context),
             std::move(knowledge_retrieval_options),
             cancellation,
             enable_planning);
      producer_queue.close();
    } catch (const StreamQueueClosed&) {
      producer_queue.close();
    } catch (...) {
      if (producer_queue.closed()) {
        producer_queue.close();
      } else {
        producer_queue.fail(std::current_exception());
      }
    }
  });
  return AgentRunnerEventStream(std::move(queue), std::move(producer), std::move(owned_cancellation));
}

AgentRunnerEventStream RunnerStreaming::events(AgentMessage input_message,
                                                  StreamQueueOptions queue_options,
                                                  const std::string& session_id,
                                                  const ModelSettings& model_settings,
                                                  RunnerRetrievalOptions retrieval_options,
                                                  RunnerWritebackOptions writeback_options,
                                                  std::vector<SkillActivation> skill_activations,
                                                  Value context,
                                                  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                                  CancellationToken* cancellation,
                                                  bool enable_planning) {
  BoundedStreamQueue<AgentRunnerStreamEvent> queue(std::move(queue_options));
  auto producer_queue = queue;
  auto owned_cancellation = cancellation ? std::shared_ptr<CancellationToken>{}
                                         : std::make_shared<CancellationToken>();
  CancellationToken* effective_cancellation = cancellation ? cancellation : owned_cancellation.get();
  std::thread producer([owner = runner_,
                        input_message = std::move(input_message),
                        session_id,
                        model_settings,
                        retrieval_options = std::move(retrieval_options),
                        writeback_options = std::move(writeback_options),
                        skill_activations = std::move(skill_activations),
                        context = std::move(context),
                        knowledge_retrieval_options = std::move(knowledge_retrieval_options),
                        cancellation = effective_cancellation,
                        enable_planning,
                        producer_queue]() mutable {
    try {
      RunnerStreaming(*owner).stream(std::move(input_message),
             [producer_queue](const AgentRunnerStreamEvent& event) mutable {
               producer_queue.push(event);
             },
             session_id,
             model_settings,
             std::move(retrieval_options),
             std::move(writeback_options),
             std::move(skill_activations),
             std::move(context),
             std::move(knowledge_retrieval_options),
             cancellation,
             enable_planning);
      producer_queue.close();
    } catch (const StreamQueueClosed&) {
      producer_queue.close();
    } catch (...) {
      if (producer_queue.closed()) {
        producer_queue.close();
      } else {
        producer_queue.fail(std::current_exception());
      }
    }
  });
  return AgentRunnerEventStream(std::move(queue), std::move(producer), std::move(owned_cancellation));
}

AgentRunnerStreamResult AgentRunner::stream_input(AgentMessage input_message, Value input_value,
                                                  bool allow_skill_input_rewrite,
                                                  const std::string& session_id,
                                                  const ModelSettings& model_settings,
                                                  RunnerRetrievalOptions retrieval_options,
                                                  RunnerWritebackOptions writeback_options,
                                                  std::vector<SkillActivation> skill_activations,
                                                  Value context,
                                                  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                                  CancellationToken* cancellation,
                                                  bool enable_planning,
                                                  AgentRunnerStreamEventHandler on_event) {
  std::uint64_t runner_sequence = 0;
  AgentRunnerStreamEventHandler sequenced_on_event;
  if (on_event) {
    sequenced_on_event = [on_event = std::move(on_event), &runner_sequence](const AgentRunnerStreamEvent& event) {
      auto sequenced = event;
      sequenced.schema_version = kAgentStreamEventSchemaVersion;
      sequenced.sequence = ++runner_sequence;
      on_event(sequenced);
    };
  }
  EventBus* event_bus = kernel_->config->event_bus ? kernel_->config->event_bus : &kernel_->owned_event_bus;
  auto invocation = resolve_runner_invocation(*kernel_->config, std::move(input_message), std::move(input_value),
                                              allow_skill_input_rewrite, session_id, model_settings,
                                              std::move(skill_activations), std::move(context),
                                              event_bus, nullptr, "agent.stream");
  clear_last_context_stats();
  if (kernel_->config->hooks.before_run) {
    RunHookContext hook_context;
    hook_context.target = ExecutionTarget::Run;
    hook_context.trace_id = invocation.run_trace.trace_id;
    hook_context.run_id = invocation.run_trace.run_id;
    hook_context.workflow_run_id = invocation.run_trace.workflow_run_id;
    hook_context.input = invocation.run_input_value;
    hook_context.context = invocation.run_hook_context_value;
    hook_context.metadata = Value::object({{"sessionId", invocation.session_id},
                                          {"runId", invocation.run_id}});
    kernel_->config->hooks.before_run(hook_context);
  }
  if (event_bus) {
    event_bus->publish("run.started", ExecutionTarget::Run,
                       run_started_event_payload(invocation.run_input_value, invocation.session_id,
                                                 invocation.run_hook_context_value, invocation.run_id),
                       invocation.run_trace);
  }
  try {
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    auto session = get_session(invocation.session_id);
    const RunnerRetrievalOptions retrieval =
        merge_runner_retrieval_options(kernel_->config->retrieval_options, retrieval_options);
    const RunnerWritebackOptions writeback =
        MemoryWriteback::merge_options(kernel_->config->writeback_options, writeback_options);
    const RunnerKnowledgeRetrievalOptions knowledge_retrieval =
        knowledge_retrieval_options ? *knowledge_retrieval_options : kernel_->config->knowledge_retrieval_options;
    auto prepared = prepare_runner_state(this, *kernel_->config, kernel_->tool_registry, *session, invocation,
                                         retrieval, knowledge_retrieval, event_bus,
                                         cancellation, enable_planning, nullptr, sequenced_on_event);

    ToolExecutor executor(kernel_->tool_registry, kernel_->config->permission_policy, kernel_->config->approval_handler, event_bus,
                          kernel_->config->execution_policies, kernel_->config->hooks);
    auto loop_setup = build_runner_loop_setup(*kernel_->config, kernel_->scratch_store.get(), *session, invocation,
                                              prepared, cancellation);
    auto on_loop_event = [&](const AgentLoopStreamEvent& event) {
      append_runner_loop_stream_event(sequenced_on_event, event);
    };
    AgentLoopStreamResult loop_stream;
    switch (kernel_->config->tool_calling_strategy) {
      case AgentToolCallingStrategy::TextReAct: {
        auto loop = create_text_react_runner_loop(*kernel_->config, kernel_->tool_registry, executor, kernel_->context_manager, event_bus,
                                                  invocation.run_trace,
                                                  [this](const ContextStatsSnapshot& snapshot) {
                                                    store_last_context_stats(snapshot);
                                                  });
        loop_stream = invocation.run_input_value.is_string()
                          ? loop.stream(*session, invocation.effective_input_message.content, on_loop_event,
                                        invocation.effective_model_settings, prepared.preface_messages,
                                        std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                                        cancellation)
                          : loop.stream(*session, invocation.effective_input_message, on_loop_event,
                                        invocation.effective_model_settings, prepared.preface_messages,
                                        std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                                        cancellation);
        break;
      }
      case AgentToolCallingStrategy::NativeToolCalling: {
        auto loop = create_native_tool_calling_runner_loop(*kernel_->config, kernel_->tool_registry, executor, kernel_->context_manager,
                                                           event_bus, invocation.run_trace,
                                                           [this](const ContextStatsSnapshot& snapshot) {
                                                             store_last_context_stats(snapshot);
                                                           });
        loop_stream = invocation.run_input_value.is_string()
                          ? loop.stream(*session, invocation.effective_input_message.content, on_loop_event,
                                        invocation.effective_model_settings, prepared.preface_messages,
                                        std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                                        cancellation)
                          : loop.stream(*session, invocation.effective_input_message, on_loop_event,
                                        invocation.effective_model_settings, prepared.preface_messages,
                                        std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                                        cancellation);
        break;
      }
    }
    if (kernel_->memory_store) {
      kernel_->memory_store->flush(invocation.session_id);
    }

    MemoryWriteback(kernel_->config->long_term_memory.get(), writeback)
        .apply_conversation_turn(invocation.session_id,
                                 invocation.effective_input,
                                 loop_stream.result.text,
                                 prepared.plan);

    auto result = runner_result_from_loop(std::move(loop_stream.result), prepared.memory_hits,
                                          prepared.knowledge_hits, prepared.knowledge_debug,
                                          prepared.plan);
    if (kernel_->config->hooks.after_run) {
      RunHookContext hook_context;
      hook_context.target = ExecutionTarget::Run;
      hook_context.trace_id = invocation.run_trace.trace_id;
      hook_context.run_id = invocation.run_trace.run_id;
      hook_context.workflow_run_id = invocation.run_trace.workflow_run_id;
      hook_context.input = invocation.run_input_value;
      hook_context.context = invocation.run_hook_context_value;
      hook_context.result = Value::object({
          {"sessionId", result.session_id},
          {"iterationCount", result.iteration_count},
          {"text", result.text},
          {"terminationReason", to_string(result.termination_reason)},
          {"memoryHitCount", result.memory_hits.size()},
          {"knowledgeHitCount", result.knowledge_hits.size()},
      });
      if (result.plan) {
        hook_context.result["plan"] = execution_plan_summary_value(*result.plan);
      }
      hook_context.metadata = Value::object({{"sessionId", invocation.session_id},
                                            {"runId", invocation.run_id}});
      kernel_->config->hooks.after_run(hook_context);
    }
    if (event_bus) {
      event_bus->publish("run.completed", ExecutionTarget::Run,
                         run_completed_event_payload(result, invocation.run_id),
                         invocation.run_trace);
    }
    emit_runner_stream_event(sequenced_on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Done,
        .result = result,
    });
    return AgentRunnerStreamResult{std::move(result)};
  } catch (const CancellationError& error) {
    if (event_bus) {
      event_bus->publish("run.cancelled", ExecutionTarget::Run,
                         run_cancelled_event_payload(invocation.run_input_value, invocation.session_id,
                                                     error, invocation.run_id),
                         invocation.run_trace);
    }
    emit_runner_stream_event(sequenced_on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Cancelled,
        .cancellation = stream_cancellation_payload(error),
    });
    throw;
  } catch (const AgentFrameworkError& error) {
    if (kernel_->config->hooks.on_run_error) {
      RunHookContext hook_context;
      hook_context.target = ExecutionTarget::Run;
      hook_context.trace_id = invocation.run_trace.trace_id;
      hook_context.run_id = invocation.run_trace.run_id;
      hook_context.workflow_run_id = invocation.run_trace.workflow_run_id;
      hook_context.input = invocation.run_input_value;
      hook_context.context = invocation.run_hook_context_value;
      hook_context.error = error.what();
      hook_context.metadata = Value::object({{"sessionId", invocation.session_id},
                                            {"runId", invocation.run_id}});
      kernel_->config->hooks.on_run_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("run.failed", ExecutionTarget::Run,
                         run_failed_event_payload(invocation.run_input_value, invocation.session_id,
                                                  error.what(), invocation.run_id),
                         invocation.run_trace);
    }
    emit_runner_stream_event(sequenced_on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Error,
        .error = stream_error_payload(error),
    });
    throw;
  } catch (const std::exception& error) {
    if (kernel_->config->hooks.on_run_error) {
      RunHookContext hook_context;
      hook_context.target = ExecutionTarget::Run;
      hook_context.trace_id = invocation.run_trace.trace_id;
      hook_context.run_id = invocation.run_trace.run_id;
      hook_context.workflow_run_id = invocation.run_trace.workflow_run_id;
      hook_context.input = invocation.run_input_value;
      hook_context.context = invocation.run_hook_context_value;
      hook_context.error = error.what();
      hook_context.metadata = Value::object({{"sessionId", invocation.session_id},
                                            {"runId", invocation.run_id}});
      kernel_->config->hooks.on_run_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("run.failed", ExecutionTarget::Run,
                         run_failed_event_payload(invocation.run_input_value, invocation.session_id,
                                                  error.what(), invocation.run_id),
                         invocation.run_trace);
    }
    emit_runner_stream_event(sequenced_on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Error,
        .error = stream_error_payload(error),
    });
    throw;
  }
}

}  // namespace agent
