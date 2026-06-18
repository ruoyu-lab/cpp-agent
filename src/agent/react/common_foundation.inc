namespace {

bool has_trace_values(const TraceContext& trace) {
  return !trace.trace_id.empty() || !trace.span_id.empty() || !trace.parent_span_id.empty() ||
         !trace.span_name.empty() || !trace.run_id.empty() || !trace.workflow_run_id.empty();
}

TraceContext child_or_root_trace_context(const TraceContext& parent, std::string span_name = {}) {
  const TraceContext* parent_ptr = has_trace_values(parent) ? &parent : nullptr;
  return derive_child_trace_context(parent_ptr, TraceSpanDescriptor{.name = std::move(span_name)});
}

std::string trim_copy(std::string value) {
  auto first = std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  });
  auto last = std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base();
  if (first >= last) {
    return {};
  }
  return std::string(first, last);
}

std::string lower_ascii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool contains_any(const std::string& text, const std::vector<std::string_view>& needles) {
  for (const auto needle : needles) {
    if (!needle.empty() && text.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool tool_text_contains_any(const ToolDefinition& tool, const std::vector<std::string_view>& needles) {
  std::string text = lower_ascii(tool.name + " " + tool.description + " " + tool.bundle + " " +
                                 tool.side_effect_level);
  for (const auto& item : tool.capabilities) {
    text += " " + lower_ascii(item);
  }
  for (const auto& item : tool.tags) {
    text += " " + lower_ascii(item);
  }
  return contains_any(text, needles);
}

std::string synthetic_tool_failure_message(const ToolCall& tool_call, const std::exception& error) {
  if (dynamic_cast<const ToolExecutionError*>(&error)) {
    return error.what();
  }
  return "Tool \"" + tool_call.name + "\" threw an unexpected error.";
}

ToolExecutionResult synthetic_tool_failure_result(const ToolCall& tool_call, std::string error_message) {
  const std::string output = Value::object({{"ok", false}, {"error", error_message}}).stringify(2);
  return ToolExecutionResult{
      .tool_call = tool_call,
      .ok = false,
      .error = std::move(error_message),
      .output = output,
      .message = create_tool_result_message(tool_call.id, tool_call.name, output, Value::object({{"ok", false}})),
  };
}

bool starts_with_label_at(const std::string& text, std::size_t index, const std::string& label) {
  if (index > 0 && text[index - 1] != '\n' && text[index - 1] != '\r') {
    return false;
  }
  if (text.size() < index + label.size() + 1) {
    return false;
  }
  if (text.compare(index, label.size(), label) != 0 || text[index + label.size()] != ':') {
    return false;
  }
  return true;
}

bool is_section_label_at(const std::string& text, std::size_t index) {
  static constexpr const char* labels[] = {
      "Thought",
      "Message",
      "Assistant",
      "Progress",
      "Actions",
      "Observation",
      "Final Answer",
  };
  for (const auto* label : labels) {
    if (starts_with_label_at(text, index, label)) {
      return true;
    }
  }
  return false;
}

std::optional<std::pair<std::size_t, std::string>> last_section(const std::string& text,
                                                                const std::string& label) {
  std::optional<std::size_t> found;
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (starts_with_label_at(text, index, label)) {
      found = index;
    }
  }
  if (!found) {
    return std::nullopt;
  }
  const std::size_t start = *found + label.size() + 1;
  std::size_t value_start = start;
  while (value_start < text.size() && std::isspace(static_cast<unsigned char>(text[value_start])) &&
         text[value_start] != '\n' && text[value_start] != '\r') {
    ++value_start;
  }
  std::size_t end = text.size();
  for (std::size_t index = value_start; index < text.size(); ++index) {
    if (is_section_label_at(text, index)) {
      end = index;
      break;
    }
  }
  return std::make_pair(*found, trim_copy(text.substr(value_start, end - value_start)));
}

std::optional<std::pair<std::size_t, std::string>> last_visible_section_before(const std::string& text,
                                                                                std::size_t before) {
  static constexpr std::string_view labels[] = {"Message", "Assistant", "Progress"};
  std::optional<std::pair<std::size_t, std::string>> best;
  for (const auto& label : labels) {
    auto section = last_section(text, std::string(label));
    if (!section || section->first >= before) {
      continue;
    }
    if (!best || section->first > best->first) {
      best = std::move(section);
    }
  }
  return best;
}

constexpr std::string_view kReasoningProtocolLeakObservation =
    "ReAct protocol was emitted in reasoning channel. Re-emit the required ReAct block in assistant content.";

bool text_has_react_terminal_protocol_block(const std::string& text) {
  return last_section(text, "Actions").has_value() || last_section(text, "Final Answer").has_value();
}

bool response_has_reasoning_protocol_leak(const AgentOutput& response) {
  if (text_has_react_terminal_protocol_block(response.text)) {
    return false;
  }
  return response.reasoning && text_has_react_terminal_protocol_block(response.reasoning->text);
}

bool tool_available(const ToolRegistry& tools, const std::string& name) {
  for (const auto& descriptor : tools.descriptors()) {
    if (descriptor.name == name) {
      return true;
    }
  }
  return false;
}

std::string available_tool_names(const ToolRegistry& tools) {
  std::string names;
  for (const auto& descriptor : tools.descriptors()) {
    if (!names.empty()) {
      names += ", ";
    }
    names += descriptor.name;
  }
  return names.empty() ? "(none)" : names;
}

Value model_response_hook_value(const AgentOutput& response) {
  return Value::object({
      {"provider", response.provider},
      {"model", response.model},
      {"text", response.text},
      {"finishReason", response.finish_reason},
      {"raw", response.raw},
  });
}

Value retry_scheduled_event_payload(const RetryScheduledContext& retry, Value extra = Value::object({})) {
  Value payload = extra.is_object() ? extra : Value::object({});
  payload["attempt"] = retry.attempt;
  payload["delayMs"] = retry.delay_ms;
  payload["error"] = retry.error;
  payload["target"] = to_string(retry.target);
  if (retry.metadata.is_object() && !retry.metadata.as_object().empty()) {
    payload["metadata"] = retry.metadata;
  }
  return payload;
}

ModelUsage aggregate_trace_usage(const std::vector<AgentTraceEntry>& trace) {
  std::vector<ModelUsage> per_call;
  per_call.reserve(trace.size());
  for (const auto& entry : trace) {
    if (entry.type == "model") {
      per_call.push_back(extract_model_usage(entry.response));
    }
  }
  return merge_model_usage(per_call);
}

std::vector<ToolExecutionResult> collect_completed_tool_results(const std::vector<AgentTraceEntry>& trace) {
  std::vector<ToolExecutionResult> results;
  for (const auto& entry : trace) {
    for (const auto& result : entry.tool_results) {
      results.push_back(result);
    }
  }
  return results;
}

const ToolDefinition* find_tool_definition(const std::vector<ToolDefinition>& tools,
                                           const std::string& name) {
  const auto found = std::find_if(tools.begin(), tools.end(), [&](const ToolDefinition& tool) {
    return tool.name == name;
  });
  return found == tools.end() ? nullptr : &*found;
}

}  // namespace
