std::string to_string(ReActStepType type) {
  switch (type) {
    case ReActStepType::ActionBatch:
      return "action-batch";
    case ReActStepType::Final:
      return "final";
    case ReActStepType::FinalRejected:
      return "final-rejected";
    case ReActStepType::ReasoningProtocolLeak:
      return "reasoning-protocol-leak";
    case ReActStepType::ParseError:
      return "parse-error";
  }
  return "action-batch";
}

Value react_trace_entry_to_value(const ReActTraceEntry& entry) {
  Value value = Value::object({
      {"type", to_string(entry.type)},
      {"iteration", entry.iteration},
      {"thought", entry.thought},
  });
  if (!entry.visible_message.empty()) {
    value["visibleMessage"] = entry.visible_message;
  }
  if (!entry.actions.empty()) {
    value["actions"] = react_actions_to_value(entry.actions);
  }
  if (!entry.observation.empty()) {
    value["observation"] = entry.observation;
  }
  if (!entry.final_answer.empty()) {
    value["finalAnswer"] = entry.final_answer;
  }
  if (!entry.error.empty()) {
    value["error"] = entry.error;
  }
  value["ok"] = entry.ok;
  return value;
}

DefaultReActParser::DefaultReActParser(ReActParserOptions options)
    : options_(options) {}

namespace {

ReActParserResult react_parse_error(int iteration,
                                    std::string thought,
                                    std::string error) {
  ReActStep step;
  step.type = ReActStepType::ParseError;
  step.iteration = iteration;
  step.thought = std::move(thought);
  step.error = std::move(error);
  step.ok = false;
  const auto message = step.error;
  return ReActParserResult{.ok = false, .step = std::move(step), .error = message};
}

std::optional<std::size_t> first_non_whitespace_index(const std::string& text) {
  for (std::size_t index = 0; index < text.size(); ++index) {
    if (std::isspace(static_cast<unsigned char>(text[index])) == 0) {
      return index;
    }
  }
  return std::nullopt;
}

}  // namespace

ReActParserResult DefaultReActParser::parse(const std::string& text,
                                            const ToolRegistry& tools,
                                            int iteration) const {
  const auto thought_section = last_section(text, "Thought");
  const std::string thought = thought_section ? thought_section->second : "";
  const auto first = first_non_whitespace_index(text);
  if (!first) {
    return react_parse_error(iteration, thought, "Response must start with a Thought block.");
  }
  if (!starts_with_label_at(text, *first, "Thought")) {
    return react_parse_error(iteration, thought,
                             "Response must start with Thought and contain no leading text.");
  }
  auto final = last_section(text, "Final Answer");
  auto actions_section = last_section(text, "Actions");
  if (final && actions_section) {
    return react_parse_error(iteration, thought, "Actions and Final Answer cannot appear in the same response.");
  }
  if (final) {
    ReActStep step;
    step.type = ReActStepType::Final;
    step.iteration = iteration;
    step.thought = thought;
    if (auto visible = last_visible_section_before(text, final->first)) {
      step.visible_message = visible->second;
    }
    step.final_answer = final->second;
    return ReActParserResult{.ok = true, .final_answer = true, .step = std::move(step)};
  }

  if (!actions_section || actions_section->second.empty()) {
    return react_parse_error(iteration, thought, "Missing Actions or Final Answer block.");
  }
  if (actions_section->second.find("```") != std::string::npos) {
    return react_parse_error(iteration, thought, "Actions must be raw JSON and cannot be wrapped in markdown fences.");
  }

  try {
    const auto parsed_actions = parse_json(actions_section->second);
    if (!parsed_actions.is_array()) {
      return react_parse_error(iteration, thought, "Actions must be a JSON array.");
    }
    const auto& action_array = parsed_actions.as_array();
    if (action_array.empty()) {
      return react_parse_error(iteration, thought, "Actions must contain at least one action.");
    }
    if (action_array.size() > options_.max_actions) {
      return react_parse_error(iteration, thought,
                               "Actions contains " + std::to_string(action_array.size()) +
                                   " actions, exceeding the limit of " +
                                   std::to_string(options_.max_actions) + ".");
    }
    ReActStep step;
    step.type = ReActStepType::ActionBatch;
    step.iteration = iteration;
    step.thought = thought;
    if (auto visible = last_visible_section_before(text, actions_section->first)) {
      step.visible_message = visible->second;
    }
    step.actions.reserve(action_array.size());
    for (std::size_t index = 0; index < action_array.size(); ++index) {
      const auto& item = action_array[index];
      if (!item.is_object()) {
        return react_parse_error(iteration, thought,
                                 "Actions[" + std::to_string(index) + "] must be a JSON object.");
      }
      const auto tool = item.at("tool").as_string();
      if (tool.empty()) {
        return react_parse_error(iteration, thought,
                                 "Actions[" + std::to_string(index) + "].tool is required.");
      }
      if (!tool_available(tools, tool)) {
        return react_parse_error(iteration, thought,
                                 "Unknown tool in Actions[" + std::to_string(index) + "]: " + tool +
                                     ". Available tools: " + available_tool_names(tools) + ".");
      }
      const auto input = item.at("input");
      if (!input.is_object()) {
        return react_parse_error(iteration, thought,
                                 "Actions[" + std::to_string(index) + "].input must be a JSON object.");
      }
      step.actions.push_back(ReActAction{
          .id = "react_" + std::to_string(iteration + 1) + "_" + std::to_string(index) + "_" + tool,
          .index = static_cast<int>(index),
          .tool = tool,
          .input = input,
      });
    }
    return ReActParserResult{.ok = true, .final_answer = false, .step = std::move(step)};
  } catch (const std::exception& error) {
    return react_parse_error(iteration, thought, error.what());
  }
}
