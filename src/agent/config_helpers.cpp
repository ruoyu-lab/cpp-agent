#include "config_helpers.hpp"

#include <algorithm>
#include <cstdlib>

namespace agent::config_detail {

std::string env_value(const std::string& key) {
  if (key.empty()) {
    return {};
  }
  if (const char* value = std::getenv(key.c_str())) {
    return value;
  }
  return {};
}

std::string resolve_env_config_value(const Value& value, const Value& env_key) {
  const auto direct = value.as_string();
  return direct.empty() ? env_value(env_key.as_string()) : direct;
}

ProviderReasoningMode reasoning_mode_from_string(const std::string& value, const std::string& path) {
  if (value.empty()) {
    return ProviderReasoningMode::None;
  }
  if (value == "openai.reasoning_effort") {
    return ProviderReasoningMode::OpenAIReasoningEffort;
  }
  if (value == "responses.reasoning") {
    return ProviderReasoningMode::ResponsesReasoning;
  }
  if (value == "qwen.thinking_budget") {
    return ProviderReasoningMode::QwenThinkingBudget;
  }
  if (value == "anthropic.thinking") {
    return ProviderReasoningMode::AnthropicThinkingBudget;
  }
  if (value == "deepseek.thinking") {
    return ProviderReasoningMode::DeepSeekThinkingToggle;
  }
  if (value == "ollama.think_toggle") {
    return ProviderReasoningMode::OllamaThinkToggle;
  }
  if (value == "ollama.think_effort") {
    return ProviderReasoningMode::OllamaThinkEffort;
  }
  if (value == "omlx.chat_template_kwargs.reasoning_effort") {
    return ProviderReasoningMode::OmlxChatTemplateReasoningEffort;
  }
  if (value == "gemini.thinking_config") {
    return ProviderReasoningMode::GeminiThinkingConfig;
  }
  throw ConfigurationError(path + " must be one of: openai.reasoning_effort, responses.reasoning, "
                           "qwen.thinking_budget, anthropic.thinking, deepseek.thinking, "
                           "ollama.think_toggle, ollama.think_effort, "
                           "omlx.chat_template_kwargs.reasoning_effort, gemini.thinking_config.");
}

ProviderReasoningMode reasoning_mode_from_model_config(const Value& model, const std::string& path) {
  if (!model.contains("reasoningMode")) {
    return ProviderReasoningMode::None;
  }
  const auto& value = model.at("reasoningMode");
  if (!value.is_string() || value.as_string().empty()) {
    throw ConfigurationError(path + ".reasoningMode must be a non-empty string.");
  }
  return reasoning_mode_from_string(value.as_string(), path + ".reasoningMode");
}

int embedding_dimensions_from_config(const Value& value, int fallback) {
  if (value.is_object() && value.at("dimensions").is_number()) {
    return static_cast<int>(std::max<long long>(0, value.at("dimensions").as_integer(fallback)));
  }
  return fallback;
}

std::string resolve_defaulted_env_config_value(const Value& value,
                                               const Value& explicit_env_key,
                                               const std::string& default_env_key,
                                               const std::string& fallback) {
  const auto direct = value.as_string();
  if (!direct.empty()) {
    return direct;
  }
  const auto explicit_env_value = env_value(explicit_env_key.as_string());
  if (!explicit_env_value.empty()) {
    return explicit_env_value;
  }
  const auto default_env_value = env_value(default_env_key);
  return default_env_value.empty() ? fallback : default_env_value;
}

}  // namespace agent::config_detail
