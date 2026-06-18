#pragma once

namespace agent {

enum class ProviderRequestProtocol {
  Unknown,
  OpenAIChatCompletions,
  OpenAIResponses,
  QwenChatCompletions,
  QwenResponses,
  AnthropicMessages,
  DeepSeekMessages,
  OllamaChat,
  GeminiGenerateContent,
};

enum class ProviderReasoningMode {
  None,
  OpenAIReasoningEffort,
  ResponsesReasoning,
  QwenThinkingBudget,
  AnthropicThinkingBudget,
  DeepSeekThinkingToggle,
  OllamaThinkToggle,
  OllamaThinkEffort,
  OmlxChatTemplateReasoningEffort,
  GeminiThinkingConfig,
};

enum class ProviderReasoningDisableStrategy {
  Unknown,
  ExplicitParam,
  Omission,
  MinimizeOnly,
  Unsupported,
};

}  // namespace agent
