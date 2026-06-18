namespace {

void maybe_auto_compact_session(SessionMemory& session, EventBus* event_bus, const TraceContext& trace) {
  if (!session.should_auto_compact()) {
    return;
  }
  const auto before_tokens = session.estimated_token_count();
  session.compact();
  if (event_bus) {
    event_bus->publish("session.auto_compact", ExecutionTarget::Run,
                       Value::object({
                           {"sessionId", session.session_id()},
                           {"currentTokens", before_tokens},
                           {"tokenBudget", session.token_budget()},
                       }),
                       child_or_root_trace_context(trace, "session.auto_compact"));
  }
}

void emit_cache_stats_event(EventBus* event_bus,
                            const AgentOutput& response,
                            const ModelSettings& settings,
                            const TraceContext& trace) {
  if (!event_bus) {
    return;
  }
  const auto usage = extract_model_usage(response);
  if (usage.input_tokens <= 0 && usage.cached_input_tokens <= 0 && usage.reasoning_tokens <= 0) {
    return;
  }
  const double hit_rate = usage.input_tokens > 0
                              ? static_cast<double>(usage.cached_input_tokens) /
                                    static_cast<double>(usage.input_tokens)
                              : 0.0;
  event_bus->publish("model.cache_stats", ExecutionTarget::Run,
                     Value::object({
                         {"provider", response.provider},
                         {"model", response.model},
                         {"totalInputTokens", static_cast<long long>(usage.input_tokens)},
                         {"cachedInputTokens", static_cast<long long>(usage.cached_input_tokens)},
                         {"hitRate", hit_rate},
                         {"strategy", to_string(settings.cache_strategy)},
                         {"inputTokensSource", to_string(usage.input_tokens_source)},
                         {"outputTokensSource", to_string(usage.output_tokens_source)},
                         {"totalTokensSource", to_string(usage.total_tokens_source)},
                         {"quality", to_string(usage.quality)},
                         {"cachedInputTokensSource", to_string(usage.cached_input_tokens_source)},
                         {"reasoningTokens", static_cast<long long>(usage.reasoning_tokens)},
                         {"reasoningSource", to_string(usage.reasoning_source)},
                     }),
                     trace);
}

AgentMessage input_to_user_message(AgentMessage input) {
  input.role = MessageRole::User;
  return input;
}

Value react_action_to_value(const ReActAction& action) {
  return Value::object({
      {"id", action.id},
      {"index", action.index},
      {"tool", action.tool},
      {"input", action.input},
  });
}

Value react_actions_to_value(const std::vector<ReActAction>& actions) {
  Value::Array output;
  output.reserve(actions.size());
  for (const auto& action : actions) {
    output.push_back(react_action_to_value(action));
  }
  return Value(std::move(output));
}

AgentMessage actions_message(const std::vector<ToolCall>& tool_calls) {
  Value::Array actions;
  actions.reserve(tool_calls.size());
  for (std::size_t index = 0; index < tool_calls.size(); ++index) {
    actions.push_back(Value::object({
        {"tool", tool_calls[index].name},
        {"input", tool_calls[index].arguments},
    }));
  }
  return create_message(
      MessageRole::Assistant,
      "Actions:\n" + Value(std::move(actions)).stringify(2),
      Value::object({{"source", "react"}, {"actions", true}}));
}

AgentMessage visible_message(const std::string& text, int iteration, bool final = false) {
  return create_message(
      MessageRole::Assistant, text,
      Value::object({{"source", "react"},
                     {"visible", true},
                     {"final", final},
                     {"iteration", iteration}}));
}

AgentOutput response_with_final_answer(AgentOutput response, const std::string& final_answer) {
  response.text = final_answer;
  response.content = final_answer.empty() ? std::vector<MessageContentPart>{}
                                          : std::vector<MessageContentPart>{text_part(final_answer)};
  response.tool_calls.clear();
  return response;
}

ToolExecutionContext build_tool_context(ToolExecutionContext context,
                                        int iteration,
                                        CancellationToken* cancellation,
                                        const TraceContext& trace) {
  context = normalize_tool_execution_context(std::move(context));
  if (!context.cancellation) {
    context.cancellation = cancellation;
  }
  context.iteration = iteration;
  context.trace_context = trace;
  context.attributes["iteration"] = iteration;
  return context;
}

}  // namespace
