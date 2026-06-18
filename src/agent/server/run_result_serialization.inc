std::string input_to_text(const Value& value) {
  return value.is_string() ? value.as_string() : safe_json_stringify(value);
}

ModelSettings model_settings_from_server_value(const Value& value) {
  ModelSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  settings.model = value.at("model").as_string();
  if (value.contains("temperature")) {
    settings.temperature = value.at("temperature").as_number();
  }
  if (value.contains("maxOutputTokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("maxOutputTokens").as_integer());
  } else if (value.contains("max_output_tokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("max_output_tokens").as_integer());
  }
  settings.extra = value.at("extra").is_object() ? value.at("extra") : Value::object({});
  if (value.contains("reasoning")) {
    assert_reasoning_settings(value.at("reasoning"), "modelSettings.reasoning");
    settings.reasoning = reasoning_settings_from_json_value(value.at("reasoning"));
  }
  return settings;
}

void validate_server_model_settings(const Value& value) {
  if (!value.is_object() || !value.contains("reasoning")) {
    return;
  }
  try {
    assert_reasoning_settings(value.at("reasoning"), "modelSettings.reasoning");
  } catch (const ConfigurationError& error) {
    throw HttpRequestError(400, error.what(), Value::object({{"error", error.what()},
                                                            {"field", "modelSettings.reasoning"}}));
  }
}

Value model_response_summary(const AgentOutput& response) {
  return agent_output_to_value(response);
}

Value structured_tool_output_summary(const ToolExecutionResult& result) {
  if (!result.result) {
    return Value();
  }
  if (std::holds_alternative<Value>(*result.result)) {
    return std::get<Value>(*result.result);
  }
  const auto& envelope = std::get<ToolResultEnvelope>(*result.result);
  return envelope.value ? *envelope.value : Value();
}

Value runner_result_summary(const AgentRunnerRunResult& result) {
  Value::Array messages;
  for (const auto& message : result.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array tool_calls;
  for (const auto& entry : result.trace) {
    for (const auto& tool_result : entry.tool_results) {
      tool_calls.push_back(Value::object({{"id", tool_result.tool_call.id},
                                          {"name", tool_result.tool_call.name},
                                          {"ok", tool_result.ok},
                                          {"structuredOutput", structured_tool_output_summary(tool_result)},
                                          {"error", tool_result.error.empty() ? Value() : Value(tool_result.error)}}));
    }
  }
  Value::Array memory_hits;
  Value::Array knowledge_hits;
  Value::Array react_trace;
  for (const auto& entry : result.react_trace) {
    react_trace.push_back(react_trace_entry_to_value(entry));
  }
  for (const auto& hit : result.memory_hits) {
    memory_hits.push_back(retrieved_memory_to_value(hit));
    if (result.knowledge_hits.empty() && hit.metadata.at("citation").is_object()) {
      knowledge_hits.push_back(Value::object({{"id", hit.id},
                                              {"content", hit.content},
                                              {"score", hit.score},
                                              {"citation", hit.metadata.at("citation")},
                                              {"metadata", hit.metadata}}));
    }
  }
  if (!result.knowledge_hits.empty()) {
    for (const auto& hit : result.knowledge_hits) {
      knowledge_hits.push_back(knowledge_search_hit_to_value(hit));
    }
  }
  Value summary = Value::object({{"sessionId", result.session_id},
                                 {"iterationCount", result.iteration_count},
                                 {"text", result.text},
                                 {"response", model_response_summary(result.response)},
                                 {"reactTrace", Value(std::move(react_trace))},
                                 {"messages", Value(std::move(messages))},
                                 {"terminationReason", to_string(result.termination_reason)},
                                 {"toolCalls", Value(std::move(tool_calls))},
                                 {"memoryHits", Value(std::move(memory_hits))},
                                 {"knowledgeHits", Value(std::move(knowledge_hits))},
                                 {"knowledgeDebug", result.knowledge_debug}});
  if (result.plan) {
    summary["plan"] = execution_plan_to_value(*result.plan);
  }
  return summary;
}

std::vector<Value> replay_events_from_result(const AgentRunnerRunResult& result, const Value& governed_result) {
  std::vector<Value> events;
  for (const auto& entry : result.trace) {
    if (entry.type == "model") {
      events.push_back(Value::object({{"type", "model"},
                                      {"iteration", entry.iteration},
                                      {"response", model_response_summary(entry.response)}}));
      continue;
    }
    if (entry.type == "tools") {
      for (const auto& tool_result : entry.tool_results) {
        events.push_back(Value::object({
            {"type", "tool-call"},
            {"iteration", entry.iteration},
            {"toolCall", Value::object({{"id", tool_result.tool_call.id},
                                        {"name", tool_result.tool_call.name},
                                        {"arguments", tool_result.tool_call.arguments}})},
            {"ok", tool_result.ok},
            {"output", tool_result.output},
            {"structuredOutput", structured_tool_output_summary(tool_result)},
            {"error", tool_result.error.empty() ? Value() : Value(tool_result.error)},
        }));
      }
    }
  }
  events.push_back(Value::object({{"type", "done"}, {"result", governed_result}}));
  return events;
}

Value status_codes_to_value(const std::map<std::string, int>& status_codes) {
  Value::Object object;
  for (const auto& [key, value] : status_codes) {
    object[key] = value;
  }
  return Value(std::move(object));
}

Value double_map_to_value(const std::map<std::string, double>& values) {
  Value::Object object;
  for (const auto& [key, value] : values) {
    object[key] = value;
  }
  return Value(std::move(object));
}

Value buckets_to_value(const std::map<std::string, AgentServerMetricsBucket>& buckets) {
  Value::Object object;
  for (const auto& [key, bucket] : buckets) {
    object[key] = agent_server_metrics_bucket_to_value(bucket);
  }
  return Value(std::move(object));
}

JsonSchema json_schema_from_server_value(const Value& value) {
  return json_schema_from_value(value);
}

Value json_schema_to_server_value(const JsonSchema& schema) {
  return json_schema_to_value(schema);
}
