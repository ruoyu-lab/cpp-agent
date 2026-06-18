namespace {

bool is_reasoning_requested(const std::optional<ReasoningSettings>& reasoning) {
  return reasoning && reasoning->enabled == true;
}

bool is_reasoning_disabled(const std::optional<ReasoningSettings>& reasoning) {
  return reasoning && reasoning->enabled == false;
}

ProviderReasoningApplyResult no_request_result(const ProviderReasoningContext& context) {
  return ProviderReasoningApplyResult{
      .requested = is_reasoning_requested(context.reasoning),
      .disabled = is_reasoning_disabled(context.reasoning),
      .mode = context.profile.reasoning_mode,
      .disable_strategy = context.profile.reasoning_disable_strategy,
      .metadata = Value::object({
          {"provider", context.provider},
          {"model", context.model},
          {"protocol", to_string(context.profile.protocol)},
          {"reasoningMode", to_string(context.profile.reasoning_mode)},
          {"reasoningDisableStrategy", to_string(context.profile.reasoning_disable_strategy)},
      }),
  };
}

std::string effort_from_reasoning(const std::optional<ReasoningSettings>& reasoning) {
  return reasoning ? normalize_reasoning_budget_to_effort(reasoning->budget) : "medium";
}

int tokens_from_reasoning(const std::optional<ReasoningSettings>& reasoning,
                          ReasoningBudgetTokenTable table) {
  return reasoning ? normalize_reasoning_budget_to_tokens(reasoning->budget, table)
                   : table.medium;
}

Value responses_reasoning_payload(const std::optional<ReasoningSettings>& reasoning) {
  return Value::object({
      {"effort", effort_from_reasoning(reasoning)},
      {"summary", reasoning && reasoning->include_thoughts == true ? "detailed" : "auto"},
  });
}

void ensure_object(Value& value) {
  if (!value.is_object()) {
    value = Value::object({});
  }
}

void mark_disabled(ProviderReasoningApplyResult& result,
                   ProviderReasoningDisableStrategy strategy,
                   bool applied,
                   bool degraded,
                   std::string message) {
  result.disabled = true;
  result.disable_strategy = strategy;
  result.applied = applied;
  result.degraded = degraded;
  result.message = std::move(message);
}

void mark_minimize_only(ProviderReasoningApplyResult& result,
                        std::string message) {
  mark_disabled(result,
                ProviderReasoningDisableStrategy::MinimizeOnly,
                false,
                true,
                std::move(message));
  result.unsupported = true;
}

void mark_unsupported_disabled(ProviderReasoningApplyResult& result,
                               std::string message) {
  mark_disabled(result,
                ProviderReasoningDisableStrategy::Unsupported,
                false,
                true,
                std::move(message));
  result.unsupported = true;
}

void mark_unavailable_disabled(ProviderReasoningApplyResult& result,
                               ProviderReasoningDisableStrategy strategy,
                               std::string message) {
  mark_disabled(result, strategy, false, true, std::move(message));
  result.unsupported = true;
}

void mark_disabled_by_omission(ProviderReasoningApplyResult& result) {
  mark_disabled(result,
                ProviderReasoningDisableStrategy::Omission,
                true,
                false,
                "disabled by omission");
}

void mark_disabled_by_explicit_param(ProviderReasoningApplyResult& result) {
  mark_disabled(result,
                ProviderReasoningDisableStrategy::ExplicitParam,
                true,
                false,
                "disabled by explicit provider parameter");
}

Value reasoning_apply_metadata(const ProviderReasoningApplyResult& result,
                               const ProviderReasoningContext& context) {
  const auto strategy = result.disable_strategy == ProviderReasoningDisableStrategy::Unknown
                            ? context.profile.reasoning_disable_strategy
                            : result.disable_strategy;
  const auto mode = result.mode == ProviderReasoningMode::None
                        ? context.profile.reasoning_mode
                        : result.mode;
  return Value::object({
      {"provider", context.provider},
      {"model", context.model},
      {"protocol", to_string(context.profile.protocol)},
      {"reasoningMode", to_string(mode)},
      {"requested", result.requested},
      {"applied", result.applied},
      {"disabled", result.disabled},
      {"degraded", result.degraded},
      {"unsupported", result.unsupported},
      {"disableStrategy", to_string(strategy)},
      {"reasoningDisableStrategy", to_string(strategy)},
      {"message", result.message.empty() ? Value() : Value(result.message)},
  });
}

void attach_reasoning_apply_metadata(NativeProviderRequest& request,
                                     const ProviderReasoningApplyResult& result) {
  if (!result.requested && !result.disabled && !result.applied && !result.degraded) {
    return;
  }
  ensure_object(request.metadata);
  request.metadata["reasoningRequest"] = provider_reasoning_apply_result_to_value(result);
}

enum class GeminiThinkingBudgetZeroSupport {
  Supported,
  Unsupported,
  Unknown,
};

GeminiThinkingBudgetZeroSupport gemini_thinking_budget_zero_support(
    const std::string& model) {
  std::string normalized = model;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (normalized.find("flash") != std::string::npos) {
    return GeminiThinkingBudgetZeroSupport::Supported;
  }
  if (normalized.find("pro") != std::string::npos) {
    return GeminiThinkingBudgetZeroSupport::Unsupported;
  }
  return GeminiThinkingBudgetZeroSupport::Unknown;
}

}  // namespace
