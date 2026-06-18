namespace {

class NoopReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile&) const override { return true; }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest&,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but this request profile has no native reasoning disable parameter.");
      return result;
    }
    if (result.requested) {
      result.degraded = true;
      result.unsupported = true;
      result.message = "reasoning is enabled for response parsing, but this request profile has no native reasoning parameter.";
    }
    if (context.profile.strict_reasoning && result.degraded) {
      throw ConfigurationError("Reasoning is enabled, but provider \"" + context.provider +
                               "\" does not support a native reasoning request parameter for " +
                               to_string(context.profile.protocol) + ".");
    }
    return result;
  }
};

class OpenAIChatReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "openai" &&
           profile.protocol == ProviderRequestProtocol::OpenAIChatCompletions &&
           profile.reasoning_mode == ProviderReasoningMode::OpenAIReasoningEffort;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled && !request.body.is_object()) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but openai.reasoning_effort could not be evaluated because the request body is not an object.");
      return result;
    }
    if (result.disabled) {
      mark_minimize_only(
          result,
          "reasoning disabled was requested, but openai.reasoning_effort only supports effort adjustment; no provider field can guarantee disable.");
      return result;
    }
    if (!result.requested || !request.body.is_object()) {
      return result;
    }
    request.body["reasoning_effort"] = effort_from_reasoning(context.reasoning);
    result.applied = true;
    result.mode = ProviderReasoningMode::OpenAIReasoningEffort;
    return result;
  }
};

class ResponsesReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return (profile.protocol == ProviderRequestProtocol::OpenAIResponses ||
            profile.protocol == ProviderRequestProtocol::QwenResponses) &&
           profile.reasoning_mode == ProviderReasoningMode::ResponsesReasoning;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled && !request.body.is_object()) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but responses.reasoning could not be evaluated because the request body is not an object.");
      return result;
    }
    if (result.disabled) {
      mark_minimize_only(
          result,
          "reasoning disabled was requested, but responses.reasoning only supports effort adjustment; no provider field can guarantee disable.");
      return result;
    }
    if (!result.requested || !request.body.is_object()) {
      return result;
    }
    request.body["reasoning"] = responses_reasoning_payload(context.reasoning);
    result.applied = true;
    result.mode = ProviderReasoningMode::ResponsesReasoning;
    return result;
  }
};

class QwenChatReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "qwen" &&
           profile.protocol == ProviderRequestProtocol::QwenChatCompletions &&
           profile.reasoning_mode == ProviderReasoningMode::QwenThinkingBudget;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (!context.reasoning) {
      return result;
    }
    if (result.disabled && !request.body.is_object()) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but qwen.thinking_budget could not be applied because the request body is not an object.");
      return result;
    }
    if (!request.body.is_object()) {
      return result;
    }
    if (result.disabled) {
      request.body["enable_thinking"] = false;
      result.mode = ProviderReasoningMode::QwenThinkingBudget;
      mark_disabled_by_explicit_param(result);
      return result;
    }
    if (!result.requested) {
      return result;
    }
    request.body["enable_thinking"] = true;
    request.body["thinking_budget"] = tokens_from_reasoning(
        context.reasoning, ReasoningBudgetTokenTable{1024, 4096, 16384});
    result.applied = true;
    result.mode = ProviderReasoningMode::QwenThinkingBudget;
    return result;
  }
};

class AnthropicReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "anthropic" &&
           profile.protocol == ProviderRequestProtocol::AnthropicMessages &&
           profile.reasoning_mode == ProviderReasoningMode::AnthropicThinkingBudget;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled) {
      mark_disabled_by_omission(result);
      return result;
    }
    if (!result.requested || !request.body.is_object()) {
      return result;
    }
    request.body["thinking"] = Value::object({
        {"type", "enabled"},
        {"budget_tokens", tokens_from_reasoning(context.reasoning,
                                                ReasoningBudgetTokenTable{1024, 4096, 16384})},
    });
    result.applied = true;
    result.mode = ProviderReasoningMode::AnthropicThinkingBudget;
    return result;
  }
};

class DeepSeekReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "deepseek" &&
           profile.protocol == ProviderRequestProtocol::DeepSeekMessages &&
           profile.reasoning_mode == ProviderReasoningMode::DeepSeekThinkingToggle;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled && !request.body.is_object()) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but deepseek.thinking could not be applied because the request body is not an object.");
      return result;
    }
    if (result.disabled) {
      request.body["thinking"] = Value::object({{"type", "disabled"}});
      result.mode = ProviderReasoningMode::DeepSeekThinkingToggle;
      mark_disabled_by_explicit_param(result);
      return result;
    }
    if (!result.requested || !request.body.is_object()) {
      return result;
    }
    request.body["thinking"] = Value::object({{"type", "enabled"}});
    result.applied = true;
    result.mode = ProviderReasoningMode::DeepSeekThinkingToggle;
    return result;
  }
};

class OllamaReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "ollama" &&
           profile.protocol == ProviderRequestProtocol::OllamaChat &&
           (profile.reasoning_mode == ProviderReasoningMode::OllamaThinkToggle ||
            profile.reasoning_mode == ProviderReasoningMode::OllamaThinkEffort);
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (!context.reasoning) {
      return result;
    }
    if (result.disabled && !request.body.is_object()) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but ollama thinking could not be applied because the request body is not an object.");
      return result;
    }
    if (!request.body.is_object()) {
      return result;
    }
    if (result.disabled) {
      if (context.profile.reasoning_mode == ProviderReasoningMode::OllamaThinkToggle) {
        request.body["think"] = false;
        result.mode = ProviderReasoningMode::OllamaThinkToggle;
        mark_disabled_by_explicit_param(result);
      } else {
        request.body["think"] = "low";
        result.mode = ProviderReasoningMode::OllamaThinkEffort;
        mark_minimize_only(
            result,
            "reasoning disabled was requested, but ollama.think_effort only supports effort adjustment; minimized with think=\"low\".");
      }
      return result;
    }
    if (!result.requested) {
      return result;
    }
    if (context.profile.reasoning_mode == ProviderReasoningMode::OllamaThinkEffort) {
      request.body["think"] = effort_from_reasoning(context.reasoning);
      result.mode = ProviderReasoningMode::OllamaThinkEffort;
    } else {
      request.body["think"] = true;
      result.mode = ProviderReasoningMode::OllamaThinkToggle;
      if (!std::holds_alternative<std::monostate>(context.reasoning->budget)) {
        result.degraded = true;
        result.message = "ollama thinking for this model is a boolean toggle; budget was ignored.";
      }
    }
    result.applied = true;
    return result;
  }
};

class OmlxGPTOSSReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "omlx" &&
           profile.protocol == ProviderRequestProtocol::OpenAIChatCompletions &&
           profile.reasoning_mode == ProviderReasoningMode::OmlxChatTemplateReasoningEffort;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled && request.body.is_object()) {
      ensure_object(request.body["chat_template_kwargs"]);
      request.body["chat_template_kwargs"]["reasoning_effort"] = "low";
      result.mode = ProviderReasoningMode::OmlxChatTemplateReasoningEffort;
      mark_minimize_only(
          result,
          "reasoning disabled was requested, but omlx.chat_template_kwargs.reasoning_effort only supports effort adjustment; minimized with reasoning_effort=\"low\".");
      return result;
    }
    if (result.disabled) {
      mark_minimize_only(
          result,
          "reasoning disabled was requested, but omlx.chat_template_kwargs.reasoning_effort only supports effort adjustment; no provider field can guarantee disable.");
      return result;
    }
    if (!result.requested || !request.body.is_object()) {
      return result;
    }
    ensure_object(request.body["chat_template_kwargs"]);
    request.body["chat_template_kwargs"]["reasoning_effort"] = effort_from_reasoning(context.reasoning);
    result.applied = true;
    result.mode = ProviderReasoningMode::OmlxChatTemplateReasoningEffort;
    return result;
  }
};

class GeminiReasoningMapper final : public ProviderReasoningMapper {
 public:
  bool matches(const ProviderRequestProfile& profile) const override {
    return profile.provider == "gemini" &&
           profile.protocol == ProviderRequestProtocol::GeminiGenerateContent &&
           profile.reasoning_mode == ProviderReasoningMode::GeminiThinkingConfig;
  }

  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const override {
    auto result = no_request_result(context);
    if (result.disabled && request.body.is_object()) {
      const auto disable_support = gemini_thinking_budget_zero_support(context.model);
      if (disable_support != GeminiThinkingBudgetZeroSupport::Supported) {
        mark_unavailable_disabled(
            result,
            disable_support == GeminiThinkingBudgetZeroSupport::Unsupported
                ? ProviderReasoningDisableStrategy::Unsupported
                : ProviderReasoningDisableStrategy::Unknown,
            disable_support == GeminiThinkingBudgetZeroSupport::Unsupported
                ? "reasoning disabled was requested, but gemini.thinking_config cannot disable thinking for this model."
                : "reasoning disabled was requested, but gemini.thinking_config disable support is unknown for this model.");
        result.mode = ProviderReasoningMode::GeminiThinkingConfig;
        return result;
      }
      ensure_object(request.body["generationConfig"]);
      request.body["generationConfig"]["thinkingConfig"] = Value::object({
          {"thinkingBudget", 0},
      });
      result.mode = ProviderReasoningMode::GeminiThinkingConfig;
      mark_disabled_by_explicit_param(result);
      return result;
    }
    if (result.disabled) {
      mark_unsupported_disabled(
          result,
          "reasoning disabled was requested, but gemini.thinking_config could not be applied because the request body is not an object.");
      return result;
    }
    if (!result.requested || !request.body.is_object()) {
      return result;
    }
    ensure_object(request.body["generationConfig"]);
    request.body["generationConfig"]["thinkingConfig"] = Value::object({
        {"includeThoughts", context.reasoning && context.reasoning->include_thoughts
                                ? *context.reasoning->include_thoughts
                                : true},
        {"thinkingBudget", tokens_from_reasoning(context.reasoning,
                                                 ReasoningBudgetTokenTable{1024, 8192, 24576})},
    });
    result.applied = true;
    result.mode = ProviderReasoningMode::GeminiThinkingConfig;
    return result;
  }
};

}  // namespace
