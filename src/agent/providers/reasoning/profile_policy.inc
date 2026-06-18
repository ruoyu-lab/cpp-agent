namespace {

struct ReasoningPolicyDescriptor {
  const char* provider;
  ProviderRequestProtocol protocol;
  ProviderReasoningMode mode;
  bool default_for_profile;
  bool supports_budget;
  bool supports_visibility;
  ProviderReasoningDisableStrategy disable_strategy;
};

constexpr ReasoningPolicyDescriptor kReasoningPolicies[] = {
    {"openai", ProviderRequestProtocol::OpenAIChatCompletions,
     ProviderReasoningMode::OpenAIReasoningEffort, true, true, true,
     ProviderReasoningDisableStrategy::MinimizeOnly},
    {"*", ProviderRequestProtocol::OpenAIResponses,
     ProviderReasoningMode::ResponsesReasoning, true, true, true,
     ProviderReasoningDisableStrategy::Unknown},
    {"*", ProviderRequestProtocol::QwenResponses,
     ProviderReasoningMode::ResponsesReasoning, true, true, true,
     ProviderReasoningDisableStrategy::Unknown},
    {"qwen", ProviderRequestProtocol::QwenChatCompletions,
     ProviderReasoningMode::QwenThinkingBudget, true, true, true,
     ProviderReasoningDisableStrategy::ExplicitParam},
    {"anthropic", ProviderRequestProtocol::AnthropicMessages,
     ProviderReasoningMode::AnthropicThinkingBudget, true, true, false,
     ProviderReasoningDisableStrategy::Omission},
    {"deepseek", ProviderRequestProtocol::DeepSeekMessages,
     ProviderReasoningMode::DeepSeekThinkingToggle, true, false, false,
     ProviderReasoningDisableStrategy::ExplicitParam},
    {"ollama", ProviderRequestProtocol::OllamaChat,
     ProviderReasoningMode::OllamaThinkToggle, true, false, false,
     ProviderReasoningDisableStrategy::ExplicitParam},
    {"ollama", ProviderRequestProtocol::OllamaChat,
     ProviderReasoningMode::OllamaThinkEffort, false, true, false,
     ProviderReasoningDisableStrategy::MinimizeOnly},
    {"omlx", ProviderRequestProtocol::OpenAIChatCompletions,
     ProviderReasoningMode::OmlxChatTemplateReasoningEffort, false, true, false,
     ProviderReasoningDisableStrategy::MinimizeOnly},
    {"gemini", ProviderRequestProtocol::GeminiGenerateContent,
     ProviderReasoningMode::GeminiThinkingConfig, true, true, true,
     ProviderReasoningDisableStrategy::Unknown},
};

bool provider_matches_policy(const ReasoningPolicyDescriptor& policy,
                             const std::string& provider) {
  return std::string(policy.provider) == "*" || provider == policy.provider;
}

const ReasoningPolicyDescriptor* find_reasoning_policy(const std::string& provider,
                                                       ProviderRequestProtocol protocol,
                                                       ProviderReasoningMode mode) {
  for (const auto& policy : kReasoningPolicies) {
    if (policy.protocol == protocol &&
        policy.mode == mode &&
        provider_matches_policy(policy, provider)) {
      return &policy;
    }
  }
  return nullptr;
}

const ReasoningPolicyDescriptor* find_default_reasoning_policy(
    const std::string& provider,
    ProviderRequestProtocol protocol) {
  for (const auto& policy : kReasoningPolicies) {
    if (policy.default_for_profile &&
        policy.protocol == protocol &&
        provider_matches_policy(policy, provider)) {
      return &policy;
    }
  }
  return nullptr;
}

bool protocol_has_reasoning_policy(ProviderRequestProtocol protocol) {
  for (const auto& policy : kReasoningPolicies) {
    if (policy.protocol == protocol) {
      return true;
    }
  }
  return false;
}

ProviderReasoningDisableStrategy resolve_policy_disable_strategy(
    const ReasoningPolicyDescriptor& policy,
    const std::string& provider,
    const std::string& model) {
  if (policy.mode == ProviderReasoningMode::GeminiThinkingConfig) {
    switch (gemini_thinking_budget_zero_support(model)) {
      case GeminiThinkingBudgetZeroSupport::Supported:
        return ProviderReasoningDisableStrategy::ExplicitParam;
      case GeminiThinkingBudgetZeroSupport::Unsupported:
        return ProviderReasoningDisableStrategy::Unsupported;
      case GeminiThinkingBudgetZeroSupport::Unknown:
        return ProviderReasoningDisableStrategy::Unknown;
    }
    return ProviderReasoningDisableStrategy::Unknown;
  }
  if (policy.mode == ProviderReasoningMode::ResponsesReasoning) {
    return provider == "openai" ? ProviderReasoningDisableStrategy::MinimizeOnly
                                : ProviderReasoningDisableStrategy::Unknown;
  }
  return policy.disable_strategy;
}

}  // namespace

ProviderReasoningMode default_reasoning_mode_for_profile(const std::string& provider,
                                                         ProviderRequestProtocol protocol) {
  const auto* policy = find_default_reasoning_policy(provider, protocol);
  return policy ? policy->mode : ProviderReasoningMode::None;
}

bool reasoning_mode_supports_budget(ProviderReasoningMode mode) {
  for (const auto& policy : kReasoningPolicies) {
    if (policy.mode == mode && policy.supports_budget) {
      return true;
    }
  }
  return false;
}

bool reasoning_mode_supports_visibility(ProviderReasoningMode mode) {
  for (const auto& policy : kReasoningPolicies) {
    if (policy.mode == mode && policy.supports_visibility) {
      return true;
    }
  }
  return false;
}

ProviderReasoningDisableStrategy reasoning_disable_strategy_for_mode(const std::string& provider,
                                                                     const std::string& model,
                                                                     ProviderReasoningMode mode) {
  for (const auto& policy : kReasoningPolicies) {
    if (policy.mode == mode && provider_matches_policy(policy, provider)) {
      return resolve_policy_disable_strategy(policy, provider, model);
    }
  }
  return mode == ProviderReasoningMode::None
             ? ProviderReasoningDisableStrategy::Unsupported
             : ProviderReasoningDisableStrategy::Unknown;
}

bool reasoning_mode_matches_profile(const ProviderRequestProfile& profile) {
  return find_reasoning_policy(profile.provider, profile.protocol, profile.reasoning_mode) != nullptr;
}

bool protocol_supports_reasoning_without_native_mode(ProviderRequestProtocol protocol) {
  return protocol_has_reasoning_policy(protocol);
}

ProviderRequestProfile make_provider_request_profile(
    std::string provider,
    std::string model,
    ProviderRequestProtocol protocol,
    std::string runtime,
    ProviderReasoningMode reasoning_mode,
    ProviderReasoningDisableStrategy reasoning_disable_strategy) {
  if (runtime.empty()) {
    runtime = provider;
  }
  const bool explicit_reasoning_mode = reasoning_mode != ProviderReasoningMode::None;
  if (!explicit_reasoning_mode) {
    reasoning_mode = default_reasoning_mode_for_profile(provider, protocol);
  }
  ProviderRequestProfile profile{
      .provider = std::move(provider),
      .runtime = std::move(runtime),
      .protocol = protocol,
      .reasoning_mode = reasoning_mode,
  };
  const auto* native_policy = find_reasoning_policy(profile.provider, protocol, profile.reasoning_mode);
  profile.supports_reasoning =
      native_policy != nullptr ||
      (!explicit_reasoning_mode && protocol_supports_reasoning_without_native_mode(protocol));
  profile.supports_reasoning_budget =
      native_policy != nullptr && native_policy->supports_budget;
  profile.supports_reasoning_visibility =
      native_policy != nullptr && native_policy->supports_visibility;
  profile.reasoning_disable_strategy =
      reasoning_disable_strategy != ProviderReasoningDisableStrategy::Unknown
          ? reasoning_disable_strategy
          : native_policy
                ? resolve_policy_disable_strategy(*native_policy, profile.provider, model)
                : ProviderReasoningDisableStrategy::Unsupported;
  return profile;
}
