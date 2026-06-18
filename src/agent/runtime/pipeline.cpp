#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace agent {




void emit_runner_stream_event(const AgentRunnerStreamEventHandler& on_event,
                              AgentRunnerStreamEvent event) {
  if (on_event) {
    on_event(event);
  }
}

RunnerPreparedState prepare_runner_state(AgentRunner* owner,
                                         const AgentRunnerResolvedConfig& config,
                                         ToolRegistry& tool_registry,
                                         SessionMemory& session,
                                         const RunnerResolvedInvocation& invocation,
                                         const RunnerRetrievalOptions& retrieval,
                                         const RunnerKnowledgeRetrievalOptions& knowledge_retrieval,
                                         EventBus* event_bus,
                                         CancellationToken* cancellation,
                                         bool enable_planning,
                                         const AgentRunnerDurableState* resume_state,
                                         AgentRunnerStreamEventHandler on_event) {
  RunnerPreparedState prepared;
  if (resume_state) {
    prepared.memory_hits = resume_state->memory_hits;
    prepared.knowledge_hits = resume_state->knowledge_hits;
    prepared.knowledge_debug = resume_state->knowledge_debug;
    prepared.preface_messages = resume_state->preface_messages;
    prepared.plan = resume_state->plan;
    return prepared;
  }

  prepared.skill_preface = build_skill_preface_messages(owner, config, invocation.skill_state,
                                                        invocation.effective_input, invocation.session_id,
                                                        invocation.run_hook_context_value);
  if (config.advertise_skills && invocation.skill_state.available_message) {
    prepared.preface_messages.push_back(*invocation.skill_state.available_message);
  }
  prepared.preface_messages.insert(prepared.preface_messages.end(),
                                   prepared.skill_preface.messages.begin(),
                                   prepared.skill_preface.messages.end());

  const bool has_knowledge_provider = config.knowledge_provider != nullptr;
  const bool will_retrieve_knowledge =
      has_knowledge_provider && knowledge_retrieval.enabled
      && !runner_knowledge_query_empty(invocation.knowledge_query);
  const bool wants_stream_events = static_cast<bool>(on_event);
  if (wants_stream_events && will_retrieve_knowledge) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "knowledge-retrieval", "start",
                                "Retrieving knowledge context."),
    });
  }

  RunnerKnowledgeRetrievalResult knowledge_result;
  if (has_knowledge_provider) {
    knowledge_result = retrieve_knowledge_context(config.knowledge_provider,
                                                  invocation.knowledge_query, knowledge_retrieval,
                                                  config.hooks, event_bus, config.execution_policies,
                                                  cancellation);
    prepared.knowledge_hits = std::move(knowledge_result.hits);
    prepared.knowledge_debug = std::move(knowledge_result.debug);
  }
  if (wants_stream_events) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::KnowledgeRetrieval,
        .knowledge_hits = prepared.knowledge_hits,
        .knowledge_message = knowledge_result.message,
        .knowledge_debug = prepared.knowledge_debug,
    });
  }
  if (knowledge_result.message) {
    prepared.preface_messages.push_back(*knowledge_result.message);
  }
  if (wants_stream_events && will_retrieve_knowledge) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status(
            "working", "knowledge-retrieval", "complete",
            prepared.knowledge_hits.empty()
                ? "Knowledge retrieval completed with no matches."
                : "Retrieved " + std::to_string(prepared.knowledge_hits.size()) + " knowledge hits.",
            Value::object({{"hits", prepared.knowledge_hits.size()}})),
    });
  }

  const bool will_retrieve_memory =
      config.long_term_memory && runner_retrieval_enabled(retrieval)
      && !trim_copy(invocation.effective_input).empty();
  if (wants_stream_events && will_retrieve_memory) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "memory-retrieval", "start",
                                "Retrieving long-term memory."),
    });
  }

  RunnerMemoryRetrievalResult memory_result;
  if (will_retrieve_memory) {
    memory_result = retrieve_long_term_memory_context(*config.long_term_memory,
                                                      invocation.effective_input,
                                                      retrieval,
                                                      config.hooks,
                                                      event_bus,
                                                      config.execution_policies,
                                                      cancellation);
    prepared.memory_hits = std::move(memory_result.hits);
  }
  if (wants_stream_events) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::MemoryRetrieval,
        .memory_hits = prepared.memory_hits,
        .memory_message = memory_result.message,
    });
  }
  if (memory_result.message) {
    prepared.preface_messages.push_back(*memory_result.message);
  }
  if (wants_stream_events && will_retrieve_memory) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status(
            "working", "memory-retrieval", "complete",
            prepared.memory_hits.empty()
                ? "Memory retrieval completed with no matches."
                : "Retrieved " + std::to_string(prepared.memory_hits.size()) + " memory hits.",
            Value::object({{"hits", prepared.memory_hits.size()}})),
    });
  }

  const bool planning_enabled = enable_planning && config.enable_planning && config.planner;
  if (wants_stream_events && planning_enabled) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "planning", "start",
                                "Building an execution plan."),
    });
  }

  prepared.plan = create_runner_execution_plan(config, invocation.effective_input, &session,
                                               invocation.run_hook_context_value, &tool_registry,
                                               prepared.memory_hits, prepared.knowledge_hits, event_bus,
                                               cancellation, enable_planning);
  if (wants_stream_events && planning_enabled) {
    if (prepared.plan) {
      emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
          .type = AgentRunnerStreamEventType::Planning,
          .plan = prepared.plan,
      });
    }
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = runner_status("working", "planning", "complete",
                                prepared.plan ? "Execution plan is ready."
                                              : "Planning completed without a structured plan.",
                                Value::object({{"hasPlan", prepared.plan.has_value()}})),
    });
  }
  if (prepared.plan) {
    prepared.preface_messages.push_back(create_plan_message(*prepared.plan));
  }

  return prepared;
}

RunnerLoopSetup build_runner_loop_setup(const AgentRunnerResolvedConfig& config,
                                        ScratchStore* scratch_store,
                                        SessionMemory& session,
                                        const RunnerResolvedInvocation& invocation,
                                        const RunnerPreparedState& prepared,
                                        CancellationToken* cancellation) {
  RunnerLoopSetup setup;
  std::optional<Value> skill_services;
  if (!invocation.skill_state.active_skills.empty()
      || !invocation.skill_state.allowed_tools.empty()
      || !prepared.skill_preface.fork_executions.empty()) {
    skill_services = skill_services_value(invocation.skill_state,
                                          prepared.skill_preface.fork_executions);
  }

  setup.loop_context = runner_loop_context(invocation.run_hook_context_value, prepared.plan,
                                           prepared.memory_hits, prepared.knowledge_hits,
                                           prepared.knowledge_debug);
  ToolExecutionContext base_tool_context;
  base_tool_context.cancellation = cancellation;
  base_tool_context.trace_context = invocation.run_trace;
  if (setup.loop_context.at("services").is_object()) {
    base_tool_context.services = setup.loop_context.at("services");
  }
  if (setup.loop_context.is_object() && !setup.loop_context.as_object().empty()) {
    base_tool_context.attributes["context"] = setup.loop_context;
  }
  auto runtime_services = runner_runtime_services(&session, invocation.session_id,
                                                  config.long_term_memory.get(), config.knowledge_provider,
                                                  setup.loop_context,
                                                  skill_services);
  runtime_services.service_container.set(kToolServiceScratchStore, scratch_store);
  setup.tool_context = with_tool_execution_services(std::move(base_tool_context),
                                                    config.tool_services,
                                                    runtime_services);
  return setup;
}

ReActLoop create_text_react_runner_loop(const AgentRunnerResolvedConfig& config,
                                        ToolRegistry& tool_registry,
                                        ToolExecutor& executor,
                                        EmbeddedContextManager& context_manager,
                                        EventBus* event_bus,
                                        const TraceContext& trace,
                                        std::function<void(const ContextStatsSnapshot&)> on_context_stats) {
  return ReActLoop(ReActLoopConfig{.model = config.adapter,
                                   .tool_registry = &tool_registry,
                                   .tool_executor = &executor,
                                   .context_manager = &context_manager,
                                   .system_prompt = config.system_prompt,
                                   .max_iterations = config.max_iterations,
                                   .max_parse_errors = config.max_parse_errors,
                                   .event_bus = event_bus,
                                   .execution_policies = config.execution_policies,
                                   .hooks = config.hooks,
                                   .trace_context = trace,
                                   .prompt_builder = config.react_prompt_builder,
                                   .parser_options = config.react_parser_options,
                                   .prompt_mode = config.react_prompt_mode,
                                   .prompt_stats_buckets = config.prompt_stats_buckets,
                                   .persist_visible_messages = config.persist_react_visible_messages,
                                   .context_window_tokens = config.context_window_tokens,
                                   .context_token_counter_resolver =
                                       [configured = config.context_token_counter](const SessionMemory& session) {
                                         if (context_token_counter_configured(configured)) {
                                           return configured;
                                         }
                                         auto session_counter = session.token_counter();
                                         if (session_counter) {
                                           return context_token_counter_from_session_counter(
                                               std::move(session_counter));
                                         }
                                         return default_context_token_counter();
                                       },
                                   .on_context_stats = std::move(on_context_stats)});
}

internal::AgentLoop create_native_tool_calling_runner_loop(const AgentRunnerResolvedConfig& config,
                                                          ToolRegistry& tool_registry,
                                                          ToolExecutor& executor,
                                                          EmbeddedContextManager& context_manager,
                                                          EventBus* event_bus,
                                                          const TraceContext& trace,
                                                          std::function<void(const ContextStatsSnapshot&)> on_context_stats) {
  return internal::AgentLoop(internal::AgentLoopConfig{.model = config.adapter,
                                                       .tool_registry = &tool_registry,
                                                       .tool_executor = &executor,
                                                       .context_manager = &context_manager,
                                                       .system_prompt = config.system_prompt,
                                                       .max_iterations = config.max_iterations,
                                                       .event_bus = event_bus,
                                                       .execution_policies = config.execution_policies,
                                                       .hooks = config.hooks,
                                                       .trace_context = trace,
                                                       .prompt_stats_buckets = config.prompt_stats_buckets,
                                                       .context_window_tokens = config.context_window_tokens,
                                                       .context_token_counter_resolver =
                                                           [configured = config.context_token_counter](
                                                               const SessionMemory& session) {
                                                             if (context_token_counter_configured(configured)) {
                                                               return configured;
                                                             }
                                                             auto session_counter = session.token_counter();
                                                             if (session_counter) {
                                                               return context_token_counter_from_session_counter(
                                                                   std::move(session_counter));
                                                             }
                                                             return default_context_token_counter();
                                                           },
                                                       .on_context_stats = std::move(on_context_stats)});
}

void append_runner_loop_stream_event(const AgentRunnerStreamEventHandler& on_event,
                                     AgentLoopStreamEvent event) {
  auto status = status_from_loop_event(event);
  if (status) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::Status,
        .status = std::move(*status),
    });
  }
  if (event.type == AgentLoopStreamEventType::ToolCallArgumentDelta) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::ToolCallArgumentDelta,
        .tool_call_iteration = event.iteration,
        .tool_call_provider = event.provider,
        .tool_call_model = event.model,
        .tool_call_id = event.tool_call_id,
        .tool_call_name = event.tool_call_name,
        .tool_call_args_delta = event.tool_call_args_delta,
        .tool_call_args_accumulated = event.tool_call_args_accumulated,
    });
    return;
  }
  if (event.type == AgentLoopStreamEventType::UserVisibleDelta) {
    emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
        .type = AgentRunnerStreamEventType::UserVisibleDelta,
        .delta = event.delta,
        .text = event.text,
    });
  }
  emit_runner_stream_event(on_event, AgentRunnerStreamEvent{
      .type = AgentRunnerStreamEventType::Loop,
      .loop_event = std::move(event),
  });
}


}  // namespace agent
