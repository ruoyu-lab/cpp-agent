#include "internal.hpp"
#include "compaction_planner.hpp"
#include "memory_writeback.hpp"
#include "runner_kernel.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace agent {

AgentRunnerRunResult RunnerExecution::run(const std::string& input, const std::string& session_id,
                                      const ModelSettings& model_settings,
                                      RunnerRetrievalOptions retrieval_options,
                                      RunnerWritebackOptions writeback_options,
                                      std::vector<SkillActivation> skill_activations,
                                      Value context,
                                      std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                      AgentRunnerDurableOptions durable_options,
                                      CancellationToken* cancellation,
                                      bool enable_planning) {
  return runner_->run_input(create_message(MessageRole::User, input), Value(input), true, session_id, model_settings,
                   std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                   std::move(context), std::move(knowledge_retrieval_options), std::move(durable_options),
                   cancellation, enable_planning);
}

AgentRunnerRunResult RunnerExecution::run(std::vector<MessageContentPart> input_parts, const std::string& session_id,
                                      const ModelSettings& model_settings,
                                      RunnerRetrievalOptions retrieval_options,
                                      RunnerWritebackOptions writeback_options,
                                      std::vector<SkillActivation> skill_activations,
                                      Value context,
                                      std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                      AgentRunnerDurableOptions durable_options,
                                      CancellationToken* cancellation,
                                      bool enable_planning) {
  const std::string input_text = extract_text_content(input_parts);
  return runner_->run_input(user_input_message(std::move(input_parts)), Value(input_text), true, session_id,
                   model_settings, std::move(retrieval_options), std::move(writeback_options),
                   std::move(skill_activations), std::move(context), std::move(knowledge_retrieval_options),
                   std::move(durable_options), cancellation, enable_planning);
}

AgentRunnerRunResult RunnerExecution::run(AgentMessage input_message, const std::string& session_id,
                                      const ModelSettings& model_settings,
                                      RunnerRetrievalOptions retrieval_options,
                                      RunnerWritebackOptions writeback_options,
                                      std::vector<SkillActivation> skill_activations,
                                      Value context,
                                      std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                      AgentRunnerDurableOptions durable_options,
                                      CancellationToken* cancellation,
                                      bool enable_planning) {
  const Value input_value = agent_message_to_value(input_message);
  return runner_->run_input(std::move(input_message), input_value, false, session_id, model_settings,
                   std::move(retrieval_options), std::move(writeback_options), std::move(skill_activations),
                   std::move(context), std::move(knowledge_retrieval_options), std::move(durable_options),
                   cancellation, enable_planning);
}

AgentRunnerRunResult AgentRunner::run_input(AgentMessage input_message, Value input_value,
                                            bool allow_skill_input_rewrite,
                                            const std::string& session_id,
                                            const ModelSettings& model_settings,
                                            RunnerRetrievalOptions retrieval_options,
                                            RunnerWritebackOptions writeback_options,
                                            std::vector<SkillActivation> skill_activations,
                                            Value context,
                                            std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options,
                                            AgentRunnerDurableOptions durable_options,
                                            CancellationToken* cancellation,
                                            bool enable_planning) {
  clear_last_context_stats();
  const auto* resume_state = durable_options.resume_state ? &*durable_options.resume_state : nullptr;
  EventBus* event_bus = kernel_->config->event_bus ? kernel_->config->event_bus : &kernel_->owned_event_bus;
  auto invocation = resolve_runner_invocation(*kernel_->config, std::move(input_message), std::move(input_value),
                                              allow_skill_input_rewrite, session_id, model_settings,
                                              std::move(skill_activations), std::move(context),
                                              event_bus, resume_state, "agent.run");
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
    auto session = get_session(invocation.session_id);
    const RunnerRetrievalOptions retrieval =
        merge_runner_retrieval_options(kernel_->config->retrieval_options, retrieval_options);
    const RunnerWritebackOptions writeback =
        MemoryWriteback::merge_options(kernel_->config->writeback_options, writeback_options);
    const RunnerKnowledgeRetrievalOptions knowledge_retrieval =
        knowledge_retrieval_options ? *knowledge_retrieval_options : kernel_->config->knowledge_retrieval_options;
    auto prepared = prepare_runner_state(this, *kernel_->config, kernel_->tool_registry, *session, invocation,
                                         retrieval, knowledge_retrieval, event_bus,
                                         cancellation, enable_planning, resume_state);

    ToolExecutor executor(kernel_->tool_registry, kernel_->config->permission_policy, kernel_->config->approval_handler, event_bus,
                          kernel_->config->execution_policies, kernel_->config->hooks);
    std::optional<AgentLoopDurableState> latest_loop_state = resume_state ? resume_state->loop : std::nullopt;
    auto checkpoint_runner = [&](AgentRunnerDurableStatus status,
                                 std::optional<AgentLoopDurableState> loop_state) {
      latest_loop_state = std::move(loop_state);
      if (!durable_options.on_checkpoint) {
        return;
      }
      AgentRunnerDurableState state;
      state.version = 1;
      state.status = status;
      state.run_id = invocation.run_id;
      state.session_id = invocation.session_id;
      state.input = invocation.input;
      state.input_value = invocation.run_input_value;
      state.input_message = invocation.input_message;
      state.model_settings = invocation.effective_model_settings;
      state.effective_input = invocation.effective_input;
      state.effective_input_value = invocation.effective_input_value;
      state.effective_input_message = invocation.effective_input_message;
      state.input_text = invocation.input_text;
      state.input_parts = invocation.input_parts;
      state.plan = prepared.plan;
      state.memory_hits = prepared.memory_hits;
      state.knowledge_hits = prepared.knowledge_hits;
      state.knowledge_debug = prepared.knowledge_debug;
      state.preface_messages = prepared.preface_messages;
      state.loop = latest_loop_state;
      state.updated_at = now_iso8601();
      durable_options.on_checkpoint(state);
    };
    if (!resume_state) {
      checkpoint_runner(AgentRunnerDurableStatus::Running, std::nullopt);
    }

    auto loop_setup = build_runner_loop_setup(*kernel_->config, kernel_->scratch_store.get(), *session, invocation,
                                              prepared, cancellation);
    AgentLoopDurableOptions loop_durable_options;
    if (resume_state && resume_state->loop) {
      loop_durable_options.resume_state = resume_state->loop;
    }
    if (durable_options.on_checkpoint) {
      loop_durable_options.on_checkpoint = [&](const AgentLoopDurableState& loop_state) {
        checkpoint_runner(AgentRunnerDurableStatus::Running, loop_state);
      };
    }
    AgentLoopRunResult base;
    switch (kernel_->config->tool_calling_strategy) {
      case AgentToolCallingStrategy::TextReAct: {
        auto loop = create_text_react_runner_loop(*kernel_->config, kernel_->tool_registry, executor, kernel_->context_manager, event_bus,
                                                  invocation.run_trace,
                                                  [this](const ContextStatsSnapshot& snapshot) {
                                                    store_last_context_stats(snapshot);
                                                  });
        base = invocation.effective_input_value.is_string()
                   ? loop.run(*session, invocation.effective_input_message.content,
                              invocation.effective_model_settings, prepared.preface_messages,
                              std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                              std::move(loop_durable_options), cancellation)
                   : loop.run(*session, invocation.effective_input_message,
                              invocation.effective_model_settings, prepared.preface_messages,
                              std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                              std::move(loop_durable_options), cancellation);
        break;
      }
      case AgentToolCallingStrategy::NativeToolCalling: {
        auto loop = create_native_tool_calling_runner_loop(*kernel_->config, kernel_->tool_registry, executor, kernel_->context_manager,
                                                           event_bus, invocation.run_trace,
                                                           [this](const ContextStatsSnapshot& snapshot) {
                                                             store_last_context_stats(snapshot);
                                                           });
        base = invocation.effective_input_value.is_string()
                   ? loop.run(*session, invocation.effective_input_message.content,
                              invocation.effective_model_settings, prepared.preface_messages,
                              std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                              std::move(loop_durable_options), cancellation)
                   : loop.run(*session, invocation.effective_input_message,
                              invocation.effective_model_settings, prepared.preface_messages,
                              std::move(loop_setup.tool_context), std::move(loop_setup.loop_context),
                              std::move(loop_durable_options), cancellation);
        break;
      }
    }
    if (kernel_->memory_store) {
      kernel_->memory_store->flush(invocation.session_id);
    }

    MemoryWriteback(kernel_->config->long_term_memory.get(), writeback)
        .apply_conversation_turn(invocation.session_id, invocation.input_text, base.text, prepared.plan);

    auto result = runner_result_from_loop(std::move(base), std::move(prepared.memory_hits),
                                          std::move(prepared.knowledge_hits),
                                          std::move(prepared.knowledge_debug), prepared.plan);
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
    checkpoint_runner(AgentRunnerDurableStatus::Completed, latest_loop_state);
    return result;
  } catch (const CancellationError& error) {
    if (event_bus) {
      event_bus->publish("run.cancelled", ExecutionTarget::Run,
                         run_cancelled_event_payload(invocation.run_input_value, invocation.session_id,
                                                     error, invocation.run_id),
                         invocation.run_trace);
    }
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
    throw;
  }
}

}  // namespace agent
