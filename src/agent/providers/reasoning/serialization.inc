std::string to_string(ProviderRequestProtocol protocol) {
  switch (protocol) {
    case ProviderRequestProtocol::OpenAIChatCompletions:
      return "openai-chat-completions";
    case ProviderRequestProtocol::OpenAIResponses:
      return "openai-responses";
    case ProviderRequestProtocol::QwenChatCompletions:
      return "qwen-chat-completions";
    case ProviderRequestProtocol::QwenResponses:
      return "qwen-responses";
    case ProviderRequestProtocol::AnthropicMessages:
      return "anthropic-messages";
    case ProviderRequestProtocol::DeepSeekMessages:
      return "deepseek-messages";
    case ProviderRequestProtocol::OllamaChat:
      return "ollama-chat";
    case ProviderRequestProtocol::GeminiGenerateContent:
      return "gemini-generate-content";
    case ProviderRequestProtocol::Unknown:
      return "unknown";
  }
  return "unknown";
}

std::string to_string(ProviderReasoningMode mode) {
  switch (mode) {
    case ProviderReasoningMode::OpenAIReasoningEffort:
      return "openai.reasoning_effort";
    case ProviderReasoningMode::ResponsesReasoning:
      return "responses.reasoning";
    case ProviderReasoningMode::QwenThinkingBudget:
      return "qwen.thinking_budget";
    case ProviderReasoningMode::AnthropicThinkingBudget:
      return "anthropic.thinking";
    case ProviderReasoningMode::DeepSeekThinkingToggle:
      return "deepseek.thinking";
    case ProviderReasoningMode::OllamaThinkToggle:
      return "ollama.think_toggle";
    case ProviderReasoningMode::OllamaThinkEffort:
      return "ollama.think_effort";
    case ProviderReasoningMode::OmlxChatTemplateReasoningEffort:
      return "omlx.chat_template_kwargs.reasoning_effort";
    case ProviderReasoningMode::GeminiThinkingConfig:
      return "gemini.thinking_config";
    case ProviderReasoningMode::None:
      return "none";
  }
  return "none";
}

std::string to_string(ProviderReasoningDisableStrategy strategy) {
  switch (strategy) {
    case ProviderReasoningDisableStrategy::ExplicitParam:
      return "explicit-param";
    case ProviderReasoningDisableStrategy::Omission:
      return "omission";
    case ProviderReasoningDisableStrategy::MinimizeOnly:
      return "minimize-only";
    case ProviderReasoningDisableStrategy::Unsupported:
      return "unsupported";
    case ProviderReasoningDisableStrategy::Unknown:
      return "unknown";
  }
  return "unknown";
}

Value provider_reasoning_apply_result_to_value(const ProviderReasoningApplyResult& result) {
  return Value::object({
      {"requested", result.requested},
      {"applied", result.applied},
      {"disabled", result.disabled},
      {"degraded", result.degraded},
      {"unsupported", result.unsupported},
      {"mode", to_string(result.mode)},
      {"disableStrategy", to_string(result.disable_strategy)},
      {"message", result.message.empty() ? Value() : Value(result.message)},
      {"metadata", result.metadata},
  });
}
