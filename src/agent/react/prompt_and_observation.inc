std::string render_managed_react_rules_text() {
  return "Use the internal tool-use format for every response.\n"
         "The first non-whitespace text in every assistant response must be Thought:.\n"
         "Do not output analysis, user-intent summaries, reply strategies, or any text before Thought:.\n"
         "Return exactly one of these shapes:\n\n"
         "Thought: brief private reasoning for trace/debug\n"
         "Actions:\n"
         "[{\"tool\":\"tool_name\",\"input\":{}}]\n\n"
         "Thought: brief private reasoning for trace/debug\n"
         "Final Answer: final user-facing answer\n\n"
         "Rules:\n"
         "- Actions must be a raw JSON array; do not wrap it in markdown fences.\n"
         "- Every action object must contain tool and input.\n"
         "- tool must be exactly one available tool name.\n"
         "- input must be a JSON object; use {} when the tool takes no arguments.\n"
         "- You may include multiple independent tool calls in one Actions array.\n"
         "- Do not split independent tool calls into separate thinking turns.\n"
         "- If a later tool depends on an earlier tool result, call only the prerequisite tool and wait for Observation.\n"
         "- Thought must be short; do not write long reasoning.\n"
         "- Never include chain-of-thought, detailed reasoning, or policy/protocol commentary in Thought.\n"
         "- Do not output both Actions and Final Answer in the same response.\n"
         "- Final Answer is only allowed after the requested work is actually complete, or when reporting a real blocker.\n"
         "- Do not use Final Answer to promise future tool use, future file changes, future commands, or future browser/system actions.\n"
         "- Actions is an internal tool-call format; do not explain it to the user.\n"
         "- Do not mention this format, internal reasoning, tool loop, or implementation details to the user.\n\n"
         "Available tools:\n";
}

std::string render_managed_react_tool_definitions_text(const std::vector<ChatToolDescriptor>& tools) {
  if (tools.empty()) {
    return "- No tools available.";
  }
  std::ostringstream out;
  for (const auto& tool : tools) {
    out << "- " << tool.name << ": "
        << (tool.description.empty() ? "No description" : tool.description) << "\n"
        << "  inputSchema: " << json_schema_to_value(tool.input_schema).stringify(0) << "\n"
        << "  scheduling: " << Value::object({
               {"readOnly", tool.read_only},
               {"mutatesFiles", tool.mutates_files},
               {"interactive", tool.interactive},
               {"longRunning", tool.long_running},
               {"batchable", tool.batchable},
               {"concurrencyKey", tool.concurrency_key},
               {"sideEffectLevel", tool.side_effect_level},
           }).stringify(0) << "\n";
  }
  return out.str();
}

std::string render_react_visible_message(const std::string& text) {
  return "Message: " + text;
}

AgentMessage DefaultReActPromptBuilder::build_system_message(const ReActPromptBuilderInput& input) const {
  std::ostringstream prompt;
  if (!input.system_prompt.empty()) {
    prompt << input.system_prompt << "\n\n";
  }
  prompt << render_managed_react_rules_text();
  prompt << render_managed_react_tool_definitions_text(input.tools);
  return create_message(MessageRole::System, prompt.str(),
                        Value::object({{"source", "react"}, {"reactPrompt", true}}));
}

AgentMessage DefaultReActObservationRenderer::render_tool_results(const std::vector<ToolExecutionResult>& results,
                                                                  int iteration) const {
  Value::Array values;
  values.reserve(results.size());
  bool all_ok = true;
  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto& result = results[index];
    all_ok = all_ok && result.ok;
    Value item = Value::object({
        {"index", static_cast<long long>(index)},
        {"tool", result.tool_call.name},
        {"toolCallId", result.tool_call.id},
        {"ok", result.ok},
        {"output", result.output},
    });
    if (!result.ok) {
      item["error"] = result.error.empty() ? result.output : result.error;
    }
    values.push_back(std::move(item));
  }
  const auto payload = Value::object({{"results", Value(std::move(values))}});
  return create_message(MessageRole::User, "Observation:\n" + payload.stringify(2),
                        Value::object({{"source", "react"},
                                      {"observation", true},
                                      {"iteration", iteration},
                                      {"batch", true},
                                      {"count", static_cast<long long>(results.size())},
                                      {"ok", all_ok}}));
}

AgentMessage DefaultReActObservationRenderer::render_parse_error(const std::string& error,
                                                                 int iteration) const {
  return create_message(MessageRole::User,
                        "Observation: {\n  \"ok\": false,\n  \"error\": " + Value(error).stringify(0) + "\n}",
                        Value::object({{"source", "react"},
                                      {"observation", true},
                                      {"iteration", iteration},
                                      {"toolName", Value()},
                                      {"ok", false}}));
}

AgentMessage DefaultReActObservationRenderer::render_final_rejection(const std::string& reason,
                                                                     int iteration) const {
  const auto message = "Final Answer rejected: " + reason +
                       " Continue with Actions or report a real blocker.";
  return create_message(MessageRole::User,
                        "Observation:\n" + message,
                        Value::object({{"source", "react"},
                                      {"observation", true},
                                      {"finalRejected", true},
                                      {"iteration", iteration},
                                      {"ok", false},
                                      {"error", message}}));
}

AgentMessage DefaultReActObservationRenderer::render_reasoning_protocol_leak(int iteration) const {
  const std::string message(kReasoningProtocolLeakObservation);
  return create_message(MessageRole::User,
                        "Observation:\n" + message,
                        Value::object({{"source", "react"},
                                      {"observation", true},
                                      {"reasoningProtocolLeak", true},
                                      {"iteration", iteration},
                                      {"ok", false},
                                      {"error", message}}));
}

FinalAnswerValidationResult DefaultFinalAnswerValidator::validate(
    const FinalAnswerValidationInput& input) const {
  const auto lower_answer = lower_ascii(input.final_answer);
  const auto lower_request = lower_ascii(input.user_request);
  if (final_answer_reports_blocker(lower_answer)) {
    return FinalAnswerValidationResult{.accepted = true};
  }
  const bool has_side_effect = has_successful_side_effect_tool_result(input);
  if (text_describes_future_work(lower_answer) && !has_side_effect) {
    return FinalAnswerValidationResult{
        .accepted = false,
        .reason = "requested work is not completed.",
        .metadata = Value::object({{"reason", "future-action-without-tool-proof"}}),
    };
  }
  if (request_requires_side_effect(lower_request) && !has_side_effect) {
    return FinalAnswerValidationResult{
        .accepted = false,
        .reason = "requested work requires side-effecting tool completion, but no completed side-effect tool result was found.",
        .metadata = Value::object({{"reason", "missing-side-effect-tool-proof"}}),
    };
  }
  return FinalAnswerValidationResult{.accepted = true};
}

ReActLoop::ReActLoop(ReActLoopConfig config) : config_(std::move(config)) {
  if (!config_.model) {
    throw ConfigurationError("ReActLoop requires a model adapter.");
  }
  if (!config_.tool_registry) {
    throw ConfigurationError("ReActLoop requires a tool registry.");
  }
  if (!config_.tool_executor) {
    throw ConfigurationError("ReActLoop requires a tool executor.");
  }
  if (!config_.context_manager) {
    throw ConfigurationError("ReActLoop requires a context manager.");
  }
  if (!config_.parser) {
    config_.parser = std::make_shared<DefaultReActParser>(config_.parser_options);
  }
  if (config_.prompt_mode == ReActPromptMode::Custom && !config_.prompt_builder) {
    throw ConfigurationError("ReActPromptMode::Custom requires a prompt_builder.");
  }
  if (config_.prompt_mode == ReActPromptMode::Managed && !config_.prompt_builder) {
    config_.prompt_builder = std::make_shared<DefaultReActPromptBuilder>();
  }
  if (!config_.observation_renderer) {
    config_.observation_renderer = std::make_shared<DefaultReActObservationRenderer>();
  }
  if (!config_.final_answer_validator) {
    config_.final_answer_validator = std::make_shared<DefaultFinalAnswerValidator>();
  }
  if (config_.max_parse_errors <= 0) {
    config_.max_parse_errors = 1;
  }
}

PromptAssembly ReActLoop::build_prompt_assembly(SessionMemory& session,
                                                const std::string& input,
                                                const Value& input_value,
                                                int iteration,
                                                const std::vector<AgentMessage>& preface_messages,
                                                const Value& runtime_context,
                                                EmbeddedContextAssembly* context_assembly) const {
  std::vector<AgentMessage> system_messages;
  Value system_metadata = Value::object({});
  if (config_.prompt_mode == ReActPromptMode::External) {
    if (!config_.system_prompt.empty()) {
      system_messages.push_back(create_message(MessageRole::System, config_.system_prompt,
                                               Value::object({{"source", "runner"}})));
      system_metadata = Value::object({{"source", "runner"}});
    }
  } else {
    system_messages.push_back(config_.prompt_builder->build_system_message(
        ReActPromptBuilderInput{config_.system_prompt, config_.tool_registry->descriptors()}));
    system_metadata = Value::object({{"source", "react"}});
  }
  auto built = PromptContextBuilder().build(PromptContextBuildOptions{
      .system_messages = std::move(system_messages),
      .system_metadata = std::move(system_metadata),
      .context_manager = config_.context_manager,
      .session = &session,
      .input = input,
      .input_value = input_value,
      .iteration = iteration,
      .preface_messages = preface_messages,
      .runtime_context = runtime_context,
  });
  if (context_assembly) {
    *context_assembly = std::move(built.context_assembly);
  }
  return std::move(built.assembly);
}

std::vector<AgentMessage> ReActLoop::build_prompt_messages(SessionMemory& session,
                                                           const std::string& input,
                                                           const Value& input_value,
                                                           int iteration,
                                                           const std::vector<AgentMessage>& preface_messages,
                                                           const Value& runtime_context,
                                                           EmbeddedContextAssembly* context_assembly) const {
  return build_prompt_assembly(session, input, input_value, iteration, preface_messages,
                               runtime_context, context_assembly).messages;
}

ContextStatsSnapshot ReActLoop::build_context_stats_snapshot(
    SessionMemory& session,
    const std::string& input,
    const Value& input_value,
    int iteration,
    const std::vector<AgentMessage>& preface_messages,
    const Value& runtime_context,
    const EmbeddedContextAssembly& context_assembly,
    const std::vector<AgentMessage>& prompt_messages) const {
  (void)input;
  (void)input_value;
  (void)iteration;
  (void)runtime_context;
  ContextStatsAssembly assembly;
  assembly.session_id = session.session_id();
  assembly.context_window_tokens = config_.context_window_tokens;
  if (config_.context_token_counter_resolver) {
    assembly.counter = config_.context_token_counter_resolver(session);
  }

  auto append_text_bucket = [&](std::string id,
                                std::string label,
                                ContextStatsBucketKind kind,
                                std::string text,
                                Value metadata = Value::object({})) {
    if (text.empty()) {
      return;
    }
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .text = std::move(text),
        .metadata = std::move(metadata),
    });
  };

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

  if (config_.prompt_mode == ReActPromptMode::Managed) {
    append_text_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                       config_.system_prompt,
                       Value::object({{"derived", true}, {"source", "react-managed"}}));
    append_text_bucket("rules", "ReAct Rules", ContextStatsBucketKind::Rules,
                       render_managed_react_rules_text(),
                       Value::object({{"derived", true}, {"source", "react-managed"}}));

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
    if (regular_tools.empty() && mcp_tools.empty()) {
      append_text_bucket("tool_definitions", "Tool Definitions", ContextStatsBucketKind::ToolDefinitions,
                         "- No tools available.",
                         Value::object({{"derived", true}, {"toolCount", 0}}));
    } else {
      append_tools_bucket("tool_definitions", "Tool Definitions", ContextStatsBucketKind::ToolDefinitions,
                          std::move(regular_tools),
                          Value::object({{"derived", true}}));
      append_tools_bucket("mcp_tools", "MCP Tools", ContextStatsBucketKind::Mcp,
                          std::move(mcp_tools),
                          Value::object({{"derived", true}}));
    }
  } else if (!config_.prompt_stats_buckets.empty()) {
    for (auto bucket : config_.prompt_stats_buckets) {
      append_stats_bucket(std::move(bucket));
    }
  } else if (!prompt_messages.empty() && prompt_messages.front().role == MessageRole::System) {
    append_message_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                          prompt_messages.front(),
                          Value::object({{"source", prompt_messages.front().metadata.at("source")}}));
  }

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

  (void)runtime_context;
  for (auto bucket : context_assembly.stats_buckets) {
    append_stats_bucket(std::move(bucket));
  }

  append_messages_bucket("conversation", "Conversation", ContextStatsBucketKind::Conversation,
                         session.get_messages(),
                         Value::object({{"source", "session-memory"}}));

  return estimate_context_stats(assembly);
}
