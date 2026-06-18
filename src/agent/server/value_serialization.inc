Value strings_to_value(const std::vector<std::string>& values) {
  Value::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.push_back(value);
  }
  return Value(std::move(array));
}

std::vector<std::string> strings_from_value(const Value& value) {
  std::vector<std::string> output;
  for (const auto& item : value.as_array()) {
    if (item.is_string()) {
      output.push_back(item.as_string());
    }
  }
  return output;
}

Value tool_call_to_value(const ToolCall& tool_call) {
  return Value::object({{"id", tool_call.id},
                        {"name", tool_call.name},
                        {"arguments", tool_call.arguments}});
}

ToolCall tool_call_from_value(const Value& value) {
  return ToolCall{
      .id = value.at("id").as_string(),
      .name = value.at("name").as_string(),
      .arguments = value.at("arguments").is_object() ? value.at("arguments") : Value::object({}),
  };
}

TraceContext trace_context_from_value(const Value& value) {
  return TraceContext{
      .trace_id = value.at("traceId").as_string(value.at("trace_id").as_string()),
      .span_id = value.at("spanId").as_string(value.at("span_id").as_string()),
      .parent_span_id = value.at("parentSpanId").as_string(value.at("parent_span_id").as_string()),
      .span_name = value.at("spanName").as_string(value.at("span_name").as_string()),
      .run_id = value.at("runId").as_string(value.at("run_id").as_string()),
      .workflow_run_id = value.at("workflowRunId").as_string(value.at("workflow_run_id").as_string()),
  };
}

Value tool_execution_context_to_value(const ToolExecutionContext& context) {
  return Value::object({{"services", context.services},
                        {"attributes", context.attributes},
                        {"iteration", context.iteration},
                        {"trace", trace_context_to_value(context.trace_context)},
                        {"toolCall", context.tool_call ? tool_call_to_value(*context.tool_call) : Value()}});
}

ToolExecutionContext tool_execution_context_from_value(const Value& value) {
  ToolExecutionContext context;
  if (!value.is_object()) {
    return context;
  }
  context.services = value.at("services").is_object() ? value.at("services") : value;
  context.attributes = value.at("attributes").is_object() ? value.at("attributes") : Value::object({});
  context.iteration = static_cast<int>(value.at("iteration").as_integer());
  context.trace_context = trace_context_from_value(value.at("trace"));
  if (value.at("toolCall").is_object()) {
    context.tool_call = tool_call_from_value(value.at("toolCall"));
  }
  return normalize_tool_execution_context(std::move(context));
}
