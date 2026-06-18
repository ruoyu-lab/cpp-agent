#include "internal.hpp"

#include <utility>

namespace agent {

namespace {

KnowledgeSearchOptions knowledge_search_options_from_runner(const RunnerKnowledgeRetrievalOptions& options) {
  KnowledgeSearchOptions search;
  search.top_k = options.top_k;
  search.vector_top_k = options.vector_top_k;
  search.lexical_top_k = options.lexical_top_k;
  search.min_score = options.min_score;
  search.hybrid_alpha = options.hybrid_alpha;
  search.rerank_top_k = options.rerank_top_k;
  search.retrieval_mode = options.retrieval_mode;
  search.oversample_factor = options.oversample_factor;
  search.fusion = options.fusion;
  search.modality_weights = options.modality_weights;
  search.uri_prefix = options.uri_prefix;
  search.document_ids = options.document_ids;
  search.asset_types = options.asset_types;
  search.space_id = options.space_id;
  search.source_types = options.source_types;
  search.chunk_ids = options.chunk_ids;
  search.metadata = options.metadata;
  search.tenant_id = options.tenant_id;
  search.knowledge_base_ids = options.knowledge_base_ids;
  return search;
}

}  // namespace

RunnerRetrievalOptions merge_runner_retrieval_options(const RunnerRetrievalOptions& base,
                                                      const RunnerRetrievalOptions& override) {
  RunnerRetrievalOptions merged = base;
  if (override.enabled.has_value()) {
    merged.enabled = override.enabled;
  }
  if (override.top_k.has_value()) {
    merged.top_k = override.top_k;
  }
  if (override.min_score.has_value()) {
    merged.min_score = override.min_score;
  }
  if (!override.namespace_id.empty()) {
    merged.namespace_id = override.namespace_id;
  }
  return merged;
}

bool runner_retrieval_enabled(const RunnerRetrievalOptions& options) {
  return !options.enabled.has_value() || *options.enabled;
}




RunnerKnowledgeQuery text_knowledge_query(std::string text) {
  if (text.empty()) {
    return {};
  }
  RunnerKnowledgeQuery query;
  query.kind = RunnerKnowledgeQuery::Kind::Text;
  query.text = std::move(text);
  return query;
}

RunnerKnowledgeQuery image_knowledge_query(const AgentMessage& message,
                                           const MessageContentPart& part) {
  RunnerKnowledgeQuery query;
  query.kind = RunnerKnowledgeQuery::Kind::Image;
  query.image.source = part.source;
  query.image.alt_text = part.alt_text;
  query.image.title = message.metadata.at("title").as_string();
  query.image.text_hint = message.metadata.at("textHint").as_string();
  query.image.metadata = message.metadata.is_object() ? message.metadata : Value::object({});
  return query;
}

RunnerKnowledgeQuery resolve_runner_knowledge_query(const AgentMessage& message) {
  const std::string text = extract_text_content(message.content);
  if (!trim_copy(text).empty()) {
    return text_knowledge_query(text);
  }
  for (const auto& part : message.content) {
    if (part.type == ContentPartType::Image) {
      return image_knowledge_query(message, part);
    }
  }
  return {};
}

std::string runner_knowledge_query_text(const RunnerKnowledgeQuery& query) {
  if (query.kind == RunnerKnowledgeQuery::Kind::Text) {
    return query.text;
  }
  if (query.kind == RunnerKnowledgeQuery::Kind::Image) {
    std::string text;
    for (const auto& part : std::vector<std::string>{query.image.title, query.image.alt_text,
                                                     query.image.text_hint}) {
      if (part.empty()) {
        continue;
      }
      if (!text.empty()) {
        text += " ";
      }
      text += part;
    }
    return text.empty() ? "[image]" : text;
  }
  return {};
}

bool runner_knowledge_query_empty(const RunnerKnowledgeQuery& query) {
  return query.kind == RunnerKnowledgeQuery::Kind::None || runner_knowledge_query_text(query).empty();
}

RunnerMemoryRetrievalResult retrieve_long_term_memory_context(LongTermMemoryPort& memory,
                                                              const std::string& query,
                                                              const RunnerRetrievalOptions& options,
                                                              const HookSet& hooks,
                                                              EventBus* event_bus,
                                                              const ExecutionPolicies& execution_policies,
                                                              CancellationToken* cancellation) {
  constexpr const char* source = "long-term-memory";
  if (hooks.before_knowledge_retrieval) {
    RetrievalHookContext hook_context;
    hook_context.target = ExecutionTarget::Retrieval;
    hook_context.query = query;
    hook_context.source = source;
    hooks.before_knowledge_retrieval(hook_context);
  }
  if (event_bus) {
    event_bus->publish("retrieval.started", ExecutionTarget::Retrieval,
                       Value::object({{"query", query}, {"source", source}}));
  }

  try {
    LongTermMemoryContextResult result_context = execute_with_policies(
        ExecutionTarget::Retrieval, execution_policies,
        Value::object({{"query", query}, {"source", source}}), cancellation,
        [&]() {
          return memory.build_context_message(query,
                                              SearchMemoryOptions{
                                                  .top_k = options.top_k,
                                                  .min_score = options.min_score,
                                                  .namespace_id = options.namespace_id,
                                              },
                                              cancellation);
        },
        [&](const RetryScheduledContext& retry) {
          if (event_bus) {
            event_bus->publish("retry.scheduled", ExecutionTarget::Retrieval,
                               retry_scheduled_event_payload(
                                   retry, Value::object({{"query", query}, {"source", source}})));
          }
        });
    RunnerMemoryRetrievalResult result;
    result.hits = std::move(result_context.hits);
    result.message = std::move(result_context.message);
    Value hook_result = Value::object({
        {"hits", retrieved_memories_hook_value(result.hits)},
        {"hitCount", result.hits.size()},
        {"hasMessage", result.message.has_value()},
    });
    if (hooks.after_knowledge_retrieval) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query;
      hook_context.source = source;
      hook_context.result = hook_result;
      hooks.after_knowledge_retrieval(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.completed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query}, {"source", source},
                                       {"hits", result.hits.size()}}));
    }
    return result;
  } catch (const std::exception& error) {
    if (hooks.on_knowledge_retrieval_error) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query;
      hook_context.source = source;
      hook_context.error = error.what();
      hooks.on_knowledge_retrieval_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.failed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query}, {"source", source},
                                       {"error", error.what()}}));
    }
    throw;
  }
}

RunnerKnowledgeRetrievalResult retrieve_knowledge_context(KnowledgeContextProvider* knowledge_provider,
                                                          const RunnerKnowledgeQuery& query,
                                                          const RunnerKnowledgeRetrievalOptions& options,
                                                          const HookSet& hooks,
                                                          EventBus* event_bus,
                                                          const ExecutionPolicies& execution_policies,
                                                          CancellationToken* cancellation) {
  RunnerKnowledgeRetrievalResult empty;
  if (runner_knowledge_query_empty(query) || !options.enabled || !knowledge_provider) {
    return empty;
  }

  const std::string query_text = runner_knowledge_query_text(query);
  const std::string source = knowledge_provider->knowledge_context_provider_name();
  if (hooks.before_knowledge_retrieval) {
    RetrievalHookContext hook_context;
    hook_context.target = ExecutionTarget::Retrieval;
    hook_context.query = query_text;
    hook_context.source = source;
    hooks.before_knowledge_retrieval(hook_context);
  }
  if (event_bus) {
    event_bus->publish("retrieval.started", ExecutionTarget::Retrieval,
                       Value::object({{"query", query_text}, {"source", source}}));
  }

  try {
    RunnerKnowledgeRetrievalResult result = execute_with_policies(
        ExecutionTarget::Retrieval, execution_policies,
        Value::object({{"query", query_text}, {"source", source}}), cancellation,
        [&]() {
          RunnerKnowledgeRetrievalResult current;
          auto search_options = knowledge_search_options_from_runner(options);
          search_options.cancellation = cancellation;
          auto context = query.kind == RunnerKnowledgeQuery::Kind::Image
                             ? knowledge_provider->build_context_message(query.image, std::move(search_options))
                             : knowledge_provider->build_context_message(query.text, std::move(search_options));
          current.hits = std::move(context.hits);
          current.message = std::move(context.message);
          current.debug = std::move(context.debug);
          return current;
        },
        [&](const RetryScheduledContext& retry) {
          if (event_bus) {
            event_bus->publish("retry.scheduled", ExecutionTarget::Retrieval,
                               retry_scheduled_event_payload(
                                   retry, Value::object({{"query", query_text}, {"source", source}})));
          }
        });

    Value hook_result = Value::object({
        {"hits", knowledge_hits_value(result.hits)},
        {"hitCount", result.hits.size()},
        {"hasMessage", result.message.has_value()},
        {"debug", result.debug},
    });
    if (hooks.after_knowledge_retrieval) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query_text;
      hook_context.source = source;
      hook_context.result = hook_result;
      hooks.after_knowledge_retrieval(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.completed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query_text}, {"source", source},
                                       {"hits", result.hits.size()}}));
    }
    return result;
  } catch (const std::exception& error) {
    if (hooks.on_knowledge_retrieval_error) {
      RetrievalHookContext hook_context;
      hook_context.target = ExecutionTarget::Retrieval;
      hook_context.query = query_text;
      hook_context.source = source;
      hook_context.error = error.what();
      hooks.on_knowledge_retrieval_error(hook_context);
    }
    if (event_bus) {
      event_bus->publish("retrieval.failed", ExecutionTarget::Retrieval,
                         Value::object({{"query", query_text}, {"source", source},
                                       {"error", error.what()}}));
    }
    throw;
  }
}

std::optional<ExecutionPlan> create_runner_execution_plan(const AgentRunnerResolvedConfig& config,
                                                          const std::string& input,
                                                          SessionMemory* session,
                                                          const Value& context,
                                                          ToolRegistry* tools,
                                                          const std::vector<RetrievedMemory>& memory_hits,
                                                          const std::vector<KnowledgeSearchHit>& knowledge_hits,
                                                          EventBus* event_bus,
                                                          CancellationToken* cancellation,
                                                          bool enable_planning = true) {
  if (!enable_planning || !config.enable_planning || !config.planner) {
    return std::nullopt;
  }
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }

  if (event_bus) {
    event_bus->publish("planning.started", ExecutionTarget::Run,
                       Value::object({{"input", input},
                                     {"memoryHitCount", memory_hits.size()},
                                     {"knowledgeHitCount", knowledge_hits.size()}}));
  }

  try {
    auto plan = config.planner->plan(PlannerParams{
        .input = input,
        .session = session,
        .context = context,
        .tools = tools,
        .memory_hits = memory_hits,
        .knowledge_hits = knowledge_hits,
        .cancellation = cancellation,
    });
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    if (plan && plan->steps.empty()) {
      plan = std::nullopt;
    }
    if (event_bus) {
      Value payload = Value::object({{"hasPlan", static_cast<bool>(plan)}});
      if (plan) {
        payload["goal"] = plan->goal;
        payload["stepCount"] = plan->steps.size();
      }
      event_bus->publish("planning.completed", ExecutionTarget::Run, std::move(payload));
    }
    return plan;
  } catch (const std::exception& error) {
    if (event_bus) {
      event_bus->publish("planning.failed", ExecutionTarget::Run,
                         Value::object({{"input", input}, {"error", error.what()}}));
    }
    throw;
  }
}

AgentRunnerRunResult runner_result_from_loop(AgentLoopRunResult base,
                                             std::vector<RetrievedMemory> memory_hits,
                                             std::vector<KnowledgeSearchHit> knowledge_hits,
                                             Value knowledge_debug,
                                             std::optional<ExecutionPlan> plan) {
  AgentRunnerRunResult result;
  result.session_id = base.session_id;
  result.iteration_count = base.iteration_count;
  result.text = base.text;
  result.response = base.response;
  result.trace = base.trace;
  result.react_trace = base.react_trace;
  result.messages = base.messages;
  result.termination_reason = base.termination_reason;
  result.usage = base.usage;
  result.latest_non_empty_reasoning = base.latest_non_empty_reasoning;
  result.memory_hits = std::move(memory_hits);
  result.knowledge_hits = std::move(knowledge_hits);
  result.knowledge_debug = std::move(knowledge_debug);
  result.plan = std::move(plan);
  return result;
}

}  // namespace agent
