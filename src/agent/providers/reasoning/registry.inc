ProviderReasoningMapperRegistry::ProviderReasoningMapperRegistry() {
  add(std::make_shared<OpenAIChatReasoningMapper>());
  add(std::make_shared<ResponsesReasoningMapper>());
  add(std::make_shared<QwenChatReasoningMapper>());
  add(std::make_shared<AnthropicReasoningMapper>());
  add(std::make_shared<DeepSeekReasoningMapper>());
  add(std::make_shared<OllamaReasoningMapper>());
  add(std::make_shared<OmlxGPTOSSReasoningMapper>());
  add(std::make_shared<GeminiReasoningMapper>());
  add(std::make_shared<NoopReasoningMapper>());
}

void ProviderReasoningMapperRegistry::add(std::shared_ptr<const ProviderReasoningMapper> mapper) {
  if (!mapper) {
    throw ConfigurationError("ProviderReasoningMapperRegistry cannot register a null mapper.");
  }
  mappers_.push_back(std::move(mapper));
}

const ProviderReasoningMapper& ProviderReasoningMapperRegistry::resolve(
    const ProviderRequestProfile& profile) const {
  for (const auto& mapper : mappers_) {
    if (mapper->matches(profile)) {
      return *mapper;
    }
  }
  throw ConfigurationError("No provider reasoning mapper registered.");
}

ProviderReasoningApplyResult ProviderReasoningMapperRegistry::apply(
    NativeProviderRequest& request,
    const ProviderReasoningContext& context) const {
  return resolve(context.profile).apply(request, context);
}

const ProviderReasoningMapperRegistry& ProviderReasoningMapperRegistry::default_registry() {
  static const ProviderReasoningMapperRegistry registry;
  return registry;
}
