ChatModelAdapter::ChatModelAdapter(std::string provider, std::string model, double temperature,
                                   int max_output_tokens, std::set<std::string> capabilities,
                                   std::optional<ReasoningSettings> reasoning)
    : provider_(std::move(provider)),
      model_(std::move(model)),
      temperature_(temperature),
      max_output_tokens_(max_output_tokens),
      capabilities_(std::move(capabilities)),
      default_reasoning_(std::move(reasoning)) {
  if (provider_.empty()) {
    throw ConfigurationError("Adapter provider is required.");
  }
  if (model_.empty()) {
    throw ConfigurationError("Adapter model is required.");
  }
}

const std::string& ChatModelAdapter::provider() const noexcept {
  return provider_;
}

const std::string& ChatModelAdapter::model() const noexcept {
  return model_;
}

const std::set<std::string>& ChatModelAdapter::capabilities() const noexcept {
  return capabilities_;
}

void ChatModelAdapter::set_capabilities(std::set<std::string> capabilities) {
  capabilities_ = std::move(capabilities);
}

void assert_reasoning_settings(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be a JSON object.");
  }
  for (const auto& [key, _] : value.as_object()) {
    if (key != "enabled" && key != "budget" && key != "includeThoughts" && key != "tagName") {
      throw ConfigurationError(path + "." + key + " is not a recognized reasoning field.");
    }
  }
  if (value.contains("enabled") && !value.at("enabled").is_bool()) {
    throw ConfigurationError(path + ".enabled must be a boolean.");
  }
  if (value.contains("budget")) {
    const auto& budget = value.at("budget");
    if (budget.is_string()) {
      const auto effort = budget.as_string();
      if (effort != "low" && effort != "medium" && effort != "high") {
        throw ConfigurationError(path + ".budget must be one of \"low\", \"medium\", \"high\" "
                                 "or a positive finite number.");
      }
    } else if (budget.is_number()) {
      const auto tokens = budget.as_number();
      if (!std::isfinite(tokens) || tokens <= 0) {
        throw ConfigurationError(path + ".budget must be a positive finite number when given as a token count.");
      }
    } else {
      throw ConfigurationError(path + ".budget must be a string bucket or a positive finite number.");
    }
  }
  if (value.contains("includeThoughts") && !value.at("includeThoughts").is_bool()) {
    throw ConfigurationError(path + ".includeThoughts must be a boolean.");
  }
  if (value.contains("tagName") &&
      (!value.at("tagName").is_string() || value.at("tagName").as_string().empty())) {
    throw ConfigurationError(path + ".tagName must be a non-empty string.");
  }
}

Value reasoning_settings_to_json_value(const ReasoningSettings& settings) {
  Value::Object object;
  if (settings.enabled) {
    object["enabled"] = *settings.enabled;
  }
  if (std::holds_alternative<std::string>(settings.budget)) {
    object["budget"] = std::get<std::string>(settings.budget);
  } else if (std::holds_alternative<double>(settings.budget)) {
    object["budget"] = std::get<double>(settings.budget);
  }
  if (settings.include_thoughts) {
    object["includeThoughts"] = *settings.include_thoughts;
  }
  if (!settings.tag_name.empty()) {
    object["tagName"] = settings.tag_name;
  }
  return Value(std::move(object));
}

ReasoningSettings reasoning_settings_from_json_value(const Value& value) {
  ReasoningSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  if (value.contains("enabled")) {
    settings.enabled = value.at("enabled").as_bool();
  }
  const auto& budget = value.at("budget");
  if (budget.is_string()) {
    settings.budget = budget.as_string();
  } else if (budget.is_number()) {
    settings.budget = budget.as_number();
  }
  if (value.contains("includeThoughts")) {
    settings.include_thoughts = value.at("includeThoughts").as_bool();
  } else if (value.contains("include_thoughts")) {
    settings.include_thoughts = value.at("include_thoughts").as_bool();
  }
  settings.tag_name = value.at("tagName").as_string(value.at("tag_name").as_string());
  return settings;
}

std::optional<ReasoningSettings> merge_reasoning_settings(
    const std::optional<ReasoningSettings>& base,
    const std::optional<ReasoningSettings>& override_settings) {
  if (!base && !override_settings) {
    return std::nullopt;
  }
  ReasoningSettings merged = base ? *base : ReasoningSettings{};
  if (override_settings) {
    if (override_settings->enabled) {
      merged.enabled = override_settings->enabled;
    }
    if (!std::holds_alternative<std::monostate>(override_settings->budget)) {
      merged.budget = override_settings->budget;
    }
    if (override_settings->include_thoughts) {
      merged.include_thoughts = override_settings->include_thoughts;
    }
    if (!override_settings->tag_name.empty()) {
      merged.tag_name = override_settings->tag_name;
    }
  }
  return merged;
}

std::string normalize_reasoning_budget_to_effort(
    const std::variant<std::monostate, std::string, double>& budget) {
  if (std::holds_alternative<std::string>(budget)) {
    return std::get<std::string>(budget);
  }
  if (!std::holds_alternative<double>(budget)) {
    return "medium";
  }
  const auto tokens = std::get<double>(budget);
  if (tokens < 2560) {
    return "low";
  }
  if (tokens < 10240) {
    return "medium";
  }
  return "high";
}

int normalize_reasoning_budget_to_tokens(
    const std::variant<std::monostate, std::string, double>& budget,
    ReasoningBudgetTokenTable table) {
  if (std::holds_alternative<double>(budget)) {
    return static_cast<int>(std::get<double>(budget));
  }
  if (std::holds_alternative<std::string>(budget)) {
    const auto effort = std::get<std::string>(budget);
    if (effort == "low") {
      return table.low;
    }
    if (effort == "high") {
      return table.high;
    }
  }
  return table.medium;
}

std::optional<ModelReasoning> build_model_reasoning(std::string text, std::string format) {
  text = trim_copy(std::move(text));
  if (text.empty()) {
    return std::nullopt;
  }
  return ModelReasoning{std::move(text), std::move(format)};
}

std::optional<ModelReasoning> merge_model_reasoning(std::optional<ModelReasoning> primary,
                                                    std::optional<ModelReasoning> fallback) {
  if (primary && !primary->text.empty()) {
    return primary;
  }
  return fallback && !fallback->text.empty() ? fallback : std::nullopt;
}

TaggedReasoningExtraction extract_tagged_reasoning(const std::string& input,
                                                   const std::string& tag_name) {
  const std::string open_tag = "<" + (tag_name.empty() ? std::string("think") : tag_name) + ">";
  const std::string close_tag = "</" + (tag_name.empty() ? std::string("think") : tag_name) + ">";
  std::string text;
  std::string reasoning;
  std::size_t cursor = 0;
  while (cursor < input.size()) {
    const auto start = input.find(open_tag, cursor);
    if (start == std::string::npos) {
      text += input.substr(cursor);
      break;
    }
    text += input.substr(cursor, start - cursor);
    const auto reasoning_start = start + open_tag.size();
    const auto end = input.find(close_tag, reasoning_start);
    if (!reasoning.empty()) {
      reasoning += "\n\n";
    }
    if (end == std::string::npos) {
      reasoning += input.substr(reasoning_start);
      cursor = input.size();
      break;
    }
    reasoning += input.substr(reasoning_start, end - reasoning_start);
    cursor = end + close_tag.size();
  }
  return TaggedReasoningExtraction{std::move(text), build_model_reasoning(std::move(reasoning), "tagged")};
}

TaggedReasoningStreamParser::TaggedReasoningStreamParser(std::string tag_name) {
  if (tag_name.empty()) {
    tag_name = "think";
  }
  open_tag_ = "<" + tag_name + ">";
  close_tag_ = "</" + tag_name + ">";
}

std::vector<TaggedReasoningDelta> TaggedReasoningStreamParser::push_delta(
    TaggedReasoningDeltaType type,
    std::string delta) {
  if (delta.empty()) {
    return {};
  }
  auto& pending = type == TaggedReasoningDeltaType::Text ? pending_text_utf8_ : pending_reasoning_utf8_;
  pending += std::move(delta);
  auto complete_delta = take_complete_utf8_prefix(
      pending,
      type == TaggedReasoningDeltaType::Text
          ? std::string_view("tagged reasoning text delta")
          : std::string_view("tagged reasoning hidden delta"));
  if (complete_delta.empty()) {
    return {};
  }
  if (type == TaggedReasoningDeltaType::Text) {
    text_ += complete_delta;
  } else {
    reasoning_text_ += complete_delta;
  }
  return {TaggedReasoningDelta{type, std::move(complete_delta), text_, reasoning_text_}};
}

std::vector<TaggedReasoningDelta> TaggedReasoningStreamParser::feed(const std::string& chunk) {
  buffer_ += chunk;
  std::vector<TaggedReasoningDelta> deltas;

  while (!buffer_.empty()) {
    if (mode_ == Mode::Text) {
      const auto open_index = buffer_.find(open_tag_);
      if (open_index != std::string::npos) {
        auto safe = push_delta(TaggedReasoningDeltaType::Text, buffer_.substr(0, open_index));
        deltas.insert(deltas.end(), safe.begin(), safe.end());
        buffer_ = buffer_.substr(open_index + open_tag_.size());
        mode_ = Mode::Reasoning;
        continue;
      }

      const auto partial_length = trailing_tag_prefix_length(buffer_, open_tag_);
      auto safe = push_delta(TaggedReasoningDeltaType::Text,
                             buffer_.substr(0, buffer_.size() - partial_length));
      deltas.insert(deltas.end(), safe.begin(), safe.end());
      buffer_ = buffer_.substr(buffer_.size() - partial_length);
      break;
    }

    const auto close_index = buffer_.find(close_tag_);
    if (close_index != std::string::npos) {
      auto safe = push_delta(TaggedReasoningDeltaType::Reasoning, buffer_.substr(0, close_index));
      deltas.insert(deltas.end(), safe.begin(), safe.end());
      buffer_ = buffer_.substr(close_index + close_tag_.size());
      mode_ = Mode::Text;
      continue;
    }

    const auto partial_length = trailing_tag_prefix_length(buffer_, close_tag_);
    auto safe = push_delta(TaggedReasoningDeltaType::Reasoning,
                           buffer_.substr(0, buffer_.size() - partial_length));
    deltas.insert(deltas.end(), safe.begin(), safe.end());
    buffer_ = buffer_.substr(buffer_.size() - partial_length);
    break;
  }

  return deltas;
}

std::vector<TaggedReasoningDelta> TaggedReasoningStreamParser::finish() {
  std::vector<TaggedReasoningDelta> deltas;
  const auto type = mode_ == Mode::Text ? TaggedReasoningDeltaType::Text : TaggedReasoningDeltaType::Reasoning;
  if (!buffer_.empty()) {
    auto buffered = std::move(buffer_);
    buffer_.clear();
    deltas = push_delta(type, std::move(buffered));
  }
  const auto hidden_tail = take_complete_utf8_prefix(pending_reasoning_utf8_,
                                                     "tagged reasoning hidden delta");
  if (!hidden_tail.empty()) {
    reasoning_text_ += hidden_tail;
    deltas.push_back(TaggedReasoningDelta{
        TaggedReasoningDeltaType::Reasoning,
        hidden_tail,
        text_,
        reasoning_text_,
    });
  }
  const auto text_tail = take_complete_utf8_prefix(pending_text_utf8_, "tagged reasoning text delta");
  if (!text_tail.empty()) {
    text_ += text_tail;
    deltas.push_back(TaggedReasoningDelta{
        TaggedReasoningDeltaType::Text,
        text_tail,
        text_,
        reasoning_text_,
    });
  }
  require_complete_utf8(pending_text_utf8_, "tagged reasoning text delta");
  require_complete_utf8(pending_reasoning_utf8_, "tagged reasoning hidden delta");
  return deltas;
}

std::optional<ModelReasoning> TaggedReasoningStreamParser::to_reasoning() const {
  return build_model_reasoning(reasoning_text_, "tagged");
}

const std::string& TaggedReasoningStreamParser::text() const noexcept {
  return text_;
}

const std::string& TaggedReasoningStreamParser::reasoning_text() const noexcept {
  return reasoning_text_;
}

std::string to_string(CacheStrategy strategy) {
  switch (strategy) {
    case CacheStrategy::None:
      return "none";
    case CacheStrategy::Explicit:
      return "explicit";
  }
  return "none";
}

std::string to_string(CacheScope scope) {
  switch (scope) {
    case CacheScope::SystemOnly:
      return "system-only";
    case CacheScope::SystemAndTools:
      return "system-and-tools";
    case CacheScope::SystemToolsAndSkills:
      return "system-tools-and-skills";
  }
  return "system-and-tools";
}

CacheStrategy cache_strategy_from_string(const std::string& value) {
  if (value == "explicit") {
    return CacheStrategy::Explicit;
  }
  return CacheStrategy::None;
}

CacheScope cache_scope_from_string(const std::string& value) {
  if (value == "system-only" || value == "systemOnly") {
    return CacheScope::SystemOnly;
  }
  if (value == "system-tools-and-skills" || value == "systemToolsAndSkills") {
    return CacheScope::SystemToolsAndSkills;
  }
  return CacheScope::SystemAndTools;
}

Value model_settings_to_json_value(const ModelSettings& settings) {
  Value value = Value::object({{"extra", settings.extra}});
  if (!settings.model.empty()) {
    value["model"] = settings.model;
  }
  if (settings.temperature) {
    value["temperature"] = *settings.temperature;
  }
  if (settings.max_output_tokens) {
    value["maxOutputTokens"] = *settings.max_output_tokens;
  }
  if (settings.reasoning) {
    value["reasoning"] = reasoning_settings_to_json_value(*settings.reasoning);
  }
  value["cacheStrategy"] = to_string(settings.cache_strategy);
  value["cacheScope"] = to_string(settings.cache_scope);
  if (!settings.cache_key.empty()) {
    value["cacheKey"] = settings.cache_key;
  }
  return value;
}

ModelSettings model_settings_from_json_value(const Value& value) {
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
  if (value.at("reasoning").is_object()) {
    settings.reasoning = reasoning_settings_from_json_value(value.at("reasoning"));
  }
  if (value.contains("cacheStrategy") && value.at("cacheStrategy").is_string()) {
    settings.cache_strategy = cache_strategy_from_string(value.at("cacheStrategy").as_string());
  } else if (value.contains("cache_strategy") && value.at("cache_strategy").is_string()) {
    settings.cache_strategy = cache_strategy_from_string(value.at("cache_strategy").as_string());
  }
  if (value.contains("cacheScope") && value.at("cacheScope").is_string()) {
    settings.cache_scope = cache_scope_from_string(value.at("cacheScope").as_string());
  } else if (value.contains("cache_scope") && value.at("cache_scope").is_string()) {
    settings.cache_scope = cache_scope_from_string(value.at("cache_scope").as_string());
  }
  if (value.contains("cacheKey") && value.at("cacheKey").is_string()) {
    settings.cache_key = value.at("cacheKey").as_string();
  } else if (value.contains("cache_key") && value.at("cache_key").is_string()) {
    settings.cache_key = value.at("cache_key").as_string();
  }
  settings.extra = value.at("extra").is_object() ? value.at("extra") : Value::object({});
  return settings;
}

bool ChatModelAdapter::supports(const std::string& capability) const {
  return capabilities_.find(capability) != capabilities_.end();
}

ModelSettings ChatModelAdapter::resolve_settings(const ModelSettings& settings) const {
  ModelSettings resolved = settings;
  if (resolved.model.empty()) {
    resolved.model = model_;
  }
  if (!resolved.temperature) {
    resolved.temperature = temperature_;
  }
  if (!resolved.max_output_tokens) {
    resolved.max_output_tokens = max_output_tokens_;
  }
  resolved.reasoning = merge_reasoning_settings(default_reasoning_, resolved.reasoning);
  return resolved;
}

AgentOutput ChatModelAdapter::build_output(AgentOutput payload) const {
  payload.provider = payload.provider.empty() ? provider_ : payload.provider;
  payload.model = payload.model.empty() ? model_ : payload.model;
  if (payload.content.empty() && !payload.text.empty()) {
    payload.content = {text_part(payload.text)};
  }
  if (payload.text.empty()) {
    payload.text = extract_text_content(payload.content);
  }
  if (payload.finish_reason.empty()) {
    payload.finish_reason = "stop";
  }
  return payload;
}

std::string to_string(ModelStreamEventType type) {
  switch (type) {
    case ModelStreamEventType::ResponseStart:
      return "response-start";
    case ModelStreamEventType::TextDelta:
      return "text-delta";
    case ModelStreamEventType::ReasoningDelta:
      return "reasoning-delta";
    case ModelStreamEventType::ContentPart:
      return "content-part";
    case ModelStreamEventType::Response:
      return "response";
    case ModelStreamEventType::ToolCallDelta:
      return "tool-call-delta";
  }
  return "response-start";
}

std::string to_string(ModelStreamFrameBoundary boundary) {
  switch (boundary) {
    case ModelStreamFrameBoundary::None:
      return "none";
    case ModelStreamFrameBoundary::SseEvent:
      return "sse-event";
    case ModelStreamFrameBoundary::JsonLine:
      return "json-line";
    case ModelStreamFrameBoundary::NativeCallback:
      return "native-callback";
  }
  return "none";
}

Value model_provider_stream_contract_to_value(const ModelProviderStreamContract& contract) {
  return Value::object({
      {"schemaVersion", contract.schema_version},
      {"provider", contract.provider},
      {"model", contract.model},
      {"frameBoundary", to_string(contract.frame_boundary)},
      {"textDelta", contract.text_delta},
      {"reasoningDelta", contract.reasoning_delta},
      {"toolCallDelta", contract.tool_call_delta},
      {"contentPart", contract.content_part},
  });
}

ModelProviderStreamContract ChatModelAdapter::stream_contract() const {
  return ModelProviderStreamContract{
      .provider = provider_,
      .model = model_,
      .frame_boundary = ModelStreamFrameBoundary::None,
      .text_delta = true,
      .reasoning_delta = supports("reasoning") || supports("reasoning.includeThoughts"),
      .tool_call_delta = supports("tool.use") || supports("tool.calls"),
      .content_part = supports("input.image") || supports("input.file") ||
                      supports("input.audio") || supports("input.video"),
  };
}

namespace {

std::vector<ModelStreamEvent> generated_response_stream_events(ChatModelAdapter& adapter,
                                                               const GenerateParams& params) {
  const auto resolved = adapter.resolve_settings(params.settings);
  std::vector<ModelStreamEvent> events;
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::ResponseStart,
      .provider = adapter.provider(),
      .model = resolved.model,
  });

  auto response = adapter.generate(params);
  if (response.reasoning && !response.reasoning->text.empty()) {
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ReasoningDelta,
        .provider = response.provider,
        .model = response.model,
        .delta = response.reasoning->text,
        .reasoning = response.reasoning->text,
    });
  }
  if (!response.text.empty()) {
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::TextDelta,
        .provider = response.provider,
        .model = response.model,
        .delta = response.text,
        .text = response.text,
    });
  }
  for (const auto& part : response.content) {
    if (part.type == ContentPartType::Text) {
      continue;
    }
    events.push_back(ModelStreamEvent{
        .type = ModelStreamEventType::ContentPart,
        .provider = response.provider,
        .model = response.model,
        .part = part,
    });
  }
  events.push_back(ModelStreamEvent{
      .type = ModelStreamEventType::Response,
      .response = response,
  });
  return events;
}

void emit_generated_response_stream(ChatModelAdapter& adapter,
                                    const GenerateParams& params,
                                    const ChatModelAdapter::StreamEventHandler& on_event) {
  std::uint64_t sequence = 0;
  for (auto event : generated_response_stream_events(adapter, params)) {
    event.schema_version = kAgentStreamEventSchemaVersion;
    event.sequence = ++sequence;
    if (on_event) {
      on_event(event);
    }
  }
}

}  // namespace

void ChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  emit_generated_response_stream(*this, params, on_event);
}

EchoChatModelAdapter::EchoChatModelAdapter() : ChatModelAdapter("echo", "echo", 0, 1024, {"input.text"}) {}

AgentOutput EchoChatModelAdapter::generate(const GenerateParams& params) {
  std::string text;
  bool react_prompt = false;
  for (auto it = params.messages.rbegin(); it != params.messages.rend(); ++it) {
    if (it->role == MessageRole::User || it->role == MessageRole::Tool) {
      text = extract_text_content(it->content);
      break;
    }
  }
  for (const auto& message : params.messages) {
    if (message.role == MessageRole::System &&
        extract_text_content(message.content).find("internal tool-use format") != std::string::npos) {
      react_prompt = true;
      break;
    }
  }
  if (react_prompt) {
    text = "Thought: Echo the user-facing text.\nFinal Answer: " + text;
  }
  return build_output(AgentOutput{.text = text});
}

namespace {

std::string describe_model_adapter(const ChatModelAdapter& adapter) {
  return adapter.provider() + ":" + (adapter.model().empty() ? "unknown" : adapter.model());
}

std::string fallback_model_error_message(const std::vector<std::shared_ptr<ChatModelAdapter>>& adapters,
                                         const std::string& last_error) {
  std::string chain;
  for (const auto& adapter : adapters) {
    if (!adapter) {
      continue;
    }
    if (!chain.empty()) {
      chain += " -> ";
    }
    chain += describe_model_adapter(*adapter);
  }
  std::string message = "All configured model candidates failed: " + chain + ".";
  if (!last_error.empty()) {
    message += " Last error: " + last_error;
  }
  return message;
}

}  // namespace

FallbackChatModelAdapter::FallbackChatModelAdapter(std::vector<std::shared_ptr<ChatModelAdapter>> adapters)
    : ChatModelAdapter(
          adapters.empty() || !adapters.front() ? "fallback" : adapters.front()->provider(),
          adapters.empty() || !adapters.front() ? "fallback" : adapters.front()->model(),
          adapters.empty() || !adapters.front() || !adapters.front()->resolve_settings().temperature
              ? 0.2
              : *adapters.front()->resolve_settings().temperature,
          adapters.empty() || !adapters.front() || !adapters.front()->resolve_settings().max_output_tokens
              ? 1024
              : *adapters.front()->resolve_settings().max_output_tokens,
          adapters.empty() || !adapters.front() ? std::set<std::string>{"input.text"}
                                                : adapters.front()->capabilities()),
      adapters_(std::move(adapters)) {
  if (adapters_.empty()) {
    throw ConfigurationError("FallbackChatModelAdapter requires at least one model candidate.");
  }
}

GenerateParams FallbackChatModelAdapter::params_for_adapter(const ChatModelAdapter& adapter,
                                                            const GenerateParams& params) const {
  GenerateParams candidate = params;
  if (!model().empty() && candidate.settings.model == model() && adapter.model() != model()) {
    candidate.settings.model.clear();
  }
  return candidate;
}

AgentOutput FallbackChatModelAdapter::generate(const GenerateParams& params) {
  std::string last_error;
  for (const auto& adapter : adapters_) {
    if (!adapter) {
      continue;
    }
    try {
      return adapter->generate(params_for_adapter(*adapter, params));
    } catch (const std::exception& error) {
      last_error = error.what();
    }
  }
  throw AdapterError(fallback_model_error_message(adapters_, last_error));
}

void FallbackChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  std::string last_error;
  for (const auto& adapter : adapters_) {
    if (!adapter) {
      continue;
    }
    bool emitted = false;
    try {
      adapter->stream(params_for_adapter(*adapter, params), [&](const ModelStreamEvent& event) {
        emitted = true;
        if (on_event) {
          on_event(event);
        }
      });
      return;
    } catch (const std::exception& error) {
      if (emitted) {
        throw;
      }
      last_error = error.what();
    }
  }
  throw AdapterError(fallback_model_error_message(adapters_, last_error));
}

const std::vector<std::shared_ptr<ChatModelAdapter>>& FallbackChatModelAdapter::adapters() const noexcept {
  return adapters_;
}
