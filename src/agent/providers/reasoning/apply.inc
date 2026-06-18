ProviderReasoningApplyResult apply_provider_reasoning(
    NativeProviderRequest& request,
    ProviderReasoningContext context) {
  if (context.profile.provider.empty()) {
    context.profile = make_provider_request_profile(
        context.provider,
        context.model,
        context.profile.protocol,
        context.profile.runtime,
        context.profile.reasoning_mode,
        context.profile.reasoning_disable_strategy);
  }
  if (context.provider.empty()) {
    context.provider = context.profile.provider;
  }
  auto result = ProviderReasoningMapperRegistry::default_registry().apply(request, context);
  if (result.mode == ProviderReasoningMode::None) {
    result.mode = context.profile.reasoning_mode;
  }
  if (result.disable_strategy == ProviderReasoningDisableStrategy::Unknown) {
    result.disable_strategy = context.profile.reasoning_disable_strategy;
  }
  result.metadata = reasoning_apply_metadata(result, context);
  attach_reasoning_apply_metadata(request, result);
  return result;
}

bool provider_request_profile_supports_reasoning_field(
    const ProviderRequestProfile& profile,
    const std::string& field) {
  if (field == "enabled") {
    return profile.supports_reasoning;
  }
  if (field == "budget") {
    return profile.supports_reasoning_budget;
  }
  if (field == "includeThoughts") {
    return profile.supports_reasoning_visibility;
  }
  return false;
}
