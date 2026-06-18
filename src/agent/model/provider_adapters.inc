namespace {

ModelProviderStreamContract provider_stream_contract_for(const ChatModelAdapter& adapter,
                                                         ModelStreamFrameBoundary boundary) {
  return ModelProviderStreamContract{
      .provider = adapter.provider(),
      .model = adapter.model(),
      .frame_boundary = boundary,
      .text_delta = true,
      .reasoning_delta = adapter.supports("reasoning") || adapter.supports("reasoning.includeThoughts"),
      .tool_call_delta = adapter.supports("tool.use") || adapter.supports("tool.calls"),
      .content_part = adapter.supports("input.image") || adapter.supports("input.file") ||
                      adapter.supports("input.audio") || adapter.supports("input.video"),
  };
}

void emit_collected_model_stream_events(std::vector<ModelStreamEvent> events,
                                        const ChatModelAdapter::StreamEventHandler& on_event) {
  std::uint64_t sequence = 0;
  for (auto& event : events) {
    event.schema_version = kAgentStreamEventSchemaVersion;
    if (event.sequence == 0) {
      event.sequence = ++sequence;
    } else {
      sequence = event.sequence;
    }
    if (on_event) {
      on_event(event);
    }
  }
}

std::vector<ModelStreamEvent> provider_generated_response_stream_events(ChatModelAdapter& adapter,
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

void emit_incremental_provider_stream(const NativeProviderStreamingTransport& streaming_transport,
                                      const NativeProviderRequest& request,
                                      IncrementalParsedStreamEmitter emitter,
                                      CancellationToken* cancellation,
                                      const ChatModelAdapter::StreamEventHandler& on_event) {
  throw_if_model_cancelled(cancellation);
  streaming_transport(request, [&](std::string_view chunk) {
    throw_if_model_cancelled(cancellation);
    emitter.feed(chunk, on_event);
  });
  throw_if_model_cancelled(cancellation);
  emitter.finish(on_event);
}

ProviderReasoningApplyResult apply_request_reasoning(
    NativeProviderRequest& request,
    const std::string& provider,
    const std::string& model,
    ProviderRequestProtocol protocol,
    const std::optional<ReasoningSettings>& reasoning,
    ProviderReasoningMode reasoning_mode = ProviderReasoningMode::None) {
  return apply_provider_reasoning(request, ProviderReasoningContext{
      .provider = provider,
      .model = model,
      .reasoning = reasoning,
      .profile = make_provider_request_profile(provider, model, protocol, {}, reasoning_mode),
  });
}

void attach_reasoning_metadata(AgentOutput& output, const Value& metadata) {
  if (!metadata.is_object() || !metadata.at("reasoningRequest").is_object()) {
    return;
  }
  if (!output.metadata.is_object()) {
    output.metadata = Value::object({});
  }
  output.metadata["reasoningRequest"] = metadata.at("reasoningRequest");
}

void attach_request_reasoning_metadata(AgentOutput& output,
                                       const NativeProviderRequest& request) {
  attach_reasoning_metadata(output, request.metadata);
}

void attach_reasoning_metadata(std::vector<ModelStreamEvent>& events,
                               const Value& metadata) {
  for (auto& event : events) {
    if (event.type == ModelStreamEventType::Response) {
      attach_reasoning_metadata(event.response, metadata);
    }
  }
}

void attach_request_reasoning_metadata(std::vector<ModelStreamEvent>& events,
                                       const NativeProviderRequest& request) {
  attach_reasoning_metadata(events, request.metadata);
}

void require_openai_compatible_api_key(const OpenAICompatibleRuntimeOptions& options,
                                       const std::string& provider,
                                       const std::string& api_key) {
  if (!options.require_api_key) {
    return;
  }
  require_chat_api_key(api_key,
                       options.service_name.empty() ? provider : options.service_name,
                       options.api_key_env_name.empty() ? "apiKey" : options.api_key_env_name);
}

}  // namespace

OpenAICompatibleChatModelAdapter::OpenAICompatibleChatModelAdapter(std::string provider, std::string model,
                                                                   NativeProviderTransport transport,
                                                                   std::string endpoint,
                                                                   std::string base_url,
                                                                   std::string api_key,
                                                                   NativeProviderStreamTransport stream_transport,
                                                                   std::string organization,
                                                                   NativeProviderStreamingTransport streaming_transport,
                                                                   ProviderReasoningMode reasoning_mode,
                                                                   OpenAICompatibleRuntimeOptions runtime_options)
    : ChatModelAdapter(provider, std::move(model), 0.2, 1024,
                       runtime_options.capabilities.empty()
                           ? std::set<std::string>{"input.text", "transport.inline"}
                           : runtime_options.capabilities),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      endpoint_(std::move(endpoint)),
      base_url_(std::move(base_url)),
      api_key_(std::move(api_key)),
      organization_(std::move(organization)),
      reasoning_mode_(reasoning_mode),
      runtime_options_(std::move(runtime_options)) {
  if (!transport_) {
    throw ConfigurationError("OpenAICompatibleChatModelAdapter requires a transport callback.");
  }
}

ModelProviderStreamContract LlamaCppNativeChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::NativeCallback);
}

AgentOutput OpenAICompatibleChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_openai_compatible_api_key(runtime_options_, provider(), api_key_);
  const bool use_responses = runtime_options_.supports_responses_api &&
      ((settings.reasoning && settings.reasoning->enabled == true) || has_non_text_content(params.messages));
  auto request = use_responses
                     ? build_openai_responses_request(
                           resolved_params,
                           settings.model,
                           responses_endpoint_from_chat_endpoint(endpoint_),
                           base_url_)
                     : build_openai_chat_request(resolved_params, settings.model, endpoint_, base_url_);
  request.provider = provider();
  if (runtime_options_.supports_prompt_cache_key) {
    apply_openai_prompt_cache_key(request, resolved_params);
  }
  apply_request_reasoning(request,
                          provider(),
                          settings.model,
                          use_responses ? ProviderRequestProtocol::OpenAIResponses
                                        : ProviderRequestProtocol::OpenAIChatCompletions,
                          resolved_params.settings.reasoning,
                          reasoning_mode_);
  if (!api_key_.empty()) {
    request.headers["authorization"] = "Bearer " + api_key_;
  }
  if (!organization_.empty()) {
    request.headers["openai-organization"] = organization_;
  }
  throw_if_model_cancelled(params.cancellation);
  const Value raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  AgentOutput response;
  if (use_responses) {
    response = build_output(apply_tagged_reasoning(
        parse_openai_responses_response(raw, provider(), settings.model),
        settings));
  } else {
    response = build_output(apply_tagged_reasoning(
        parse_openai_chat_response(raw, provider(), settings.model),
        settings));
  }
  attach_request_reasoning_metadata(response, request);
  return response;
}

std::vector<ModelStreamEvent> OpenAICompatibleChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_openai_compatible_api_key(runtime_options_, provider(), api_key_);
  const bool use_responses = runtime_options_.supports_responses_api &&
      ((settings.reasoning && settings.reasoning->enabled == true) || has_non_text_content(params.messages));
  auto request = use_responses
                     ? build_openai_responses_stream_request(
                           resolved_params,
                           settings.model,
                           responses_endpoint_from_chat_endpoint(endpoint_),
                           base_url_)
                     : build_openai_chat_stream_request(resolved_params, settings.model, endpoint_, base_url_);
  request.provider = provider();
  if (runtime_options_.supports_prompt_cache_key) {
    apply_openai_prompt_cache_key(request, resolved_params);
  }
  apply_request_reasoning(request,
                          provider(),
                          settings.model,
                          use_responses ? ProviderRequestProtocol::OpenAIResponses
                                        : ProviderRequestProtocol::OpenAIChatCompletions,
                          resolved_params.settings.reasoning,
                          reasoning_mode_);
  if (!api_key_.empty()) {
    request.headers["authorization"] = "Bearer " + api_key_;
  }
  if (!organization_.empty()) {
    request.headers["openai-organization"] = organization_;
  }
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto events = apply_tagged_reasoning_stream(
      use_responses
          ? parse_openai_responses_stream_events(chunks, provider(), settings.model)
          : parse_openai_chat_stream_events(chunks, provider(), settings.model),
      settings);
  attach_request_reasoning_metadata(events, request);
  return events;
}

void OpenAICompatibleChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_openai_compatible_api_key(runtime_options_, provider(), api_key_);
  const bool use_responses = runtime_options_.supports_responses_api &&
      ((settings.reasoning && settings.reasoning->enabled == true) || has_non_text_content(params.messages));
  auto request = use_responses
                     ? build_openai_responses_stream_request(
                           resolved_params,
                           settings.model,
                           responses_endpoint_from_chat_endpoint(endpoint_),
                           base_url_)
                     : build_openai_chat_stream_request(resolved_params, settings.model, endpoint_, base_url_);
  request.provider = provider();
  if (runtime_options_.supports_prompt_cache_key) {
    apply_openai_prompt_cache_key(request, resolved_params);
  }
  apply_request_reasoning(request,
                          provider(),
                          settings.model,
                          use_responses ? ProviderRequestProtocol::OpenAIResponses
                                        : ProviderRequestProtocol::OpenAIChatCompletions,
                          resolved_params.settings.reasoning,
                          reasoning_mode_);
  if (!api_key_.empty()) {
    request.headers["authorization"] = "Bearer " + api_key_;
  }
  if (!organization_.empty()) {
    request.headers["openai-organization"] = organization_;
  }
  IncrementalParsedStreamEmitter emitter(
      [use_responses,
       provider = provider(),
       model = settings.model,
       request_metadata = request.metadata](const std::vector<std::string>& chunks) {
        auto events = use_responses
                          ? parse_openai_responses_stream_events(chunks, provider, model)
                          : parse_openai_chat_stream_events(chunks, provider, model);
        attach_reasoning_metadata(events, request_metadata);
        return events;
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::SseEvent);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract OpenAICompatibleChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::SseEvent);
}

QwenChatModelAdapter::QwenChatModelAdapter(std::string model, NativeProviderTransport transport,
                                           std::string api_key, std::string base_url,
                                           NativeProviderStreamTransport stream_transport,
                                           std::string reasoning_api,
                                           NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("qwen", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "input.audio", "input.video",
                        "output.structuredContent", "reasoning", "reasoning.budget",
                        "reasoning.includeThoughts"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      reasoning_api_(std::move(reasoning_api)) {
  if (!transport_) {
    throw ConfigurationError("QwenChatModelAdapter requires a transport callback.");
  }
  if (reasoning_api_.empty()) {
    reasoning_api_ = "chat-completions";
  }
  if (reasoning_api_ != "chat-completions" && reasoning_api_ != "responses") {
    throw ConfigurationError("Qwen reasoningApi must be \"chat-completions\" or \"responses\".");
  }
}

AgentOutput QwenChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "Qwen", "QWEN_API_KEY");
  const bool use_responses = has_non_text_content(params.messages) ||
      ((settings.reasoning && settings.reasoning->enabled == true) && reasoning_api_ == "responses");
  const auto request = use_responses
                           ? build_qwen_responses_request(resolved_params, settings.model, api_key_, base_url_)
                           : build_qwen_chat_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto response = build_output(apply_tagged_reasoning(
      use_responses ? parse_qwen_responses_response(raw, settings.model)
                    : parse_qwen_chat_response(raw, settings.model),
      settings));
  attach_request_reasoning_metadata(response, request);
  return response;
}

std::vector<ModelStreamEvent> QwenChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "Qwen", "QWEN_API_KEY");
  const bool use_responses = has_non_text_content(params.messages) ||
      ((settings.reasoning && settings.reasoning->enabled == true) && reasoning_api_ == "responses");
  const auto request = use_responses
                           ? build_qwen_responses_stream_request(resolved_params, settings.model, api_key_, base_url_)
                           : build_qwen_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto events = apply_tagged_reasoning_stream(
      use_responses
          ? parse_qwen_responses_stream_events(chunks, settings.model)
          : parse_openai_chat_stream_events(chunks, "qwen", settings.model),
      settings);
  attach_request_reasoning_metadata(events, request);
  return events;
}

void QwenChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "Qwen", "QWEN_API_KEY");
  const bool use_responses = has_non_text_content(params.messages) ||
      ((settings.reasoning && settings.reasoning->enabled == true) && reasoning_api_ == "responses");
  const auto request = use_responses
                           ? build_qwen_responses_stream_request(resolved_params, settings.model, api_key_, base_url_)
                           : build_qwen_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [use_responses,
       model = settings.model,
       request_metadata = request.metadata](const std::vector<std::string>& chunks) {
        auto events = use_responses
                          ? parse_qwen_responses_stream_events(chunks, model)
                          : parse_openai_chat_stream_events(chunks, "qwen", model);
        attach_reasoning_metadata(events, request_metadata);
        return events;
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::SseEvent);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract QwenChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::SseEvent);
}

MiMoChatModelAdapter::MiMoChatModelAdapter(std::string model, NativeProviderTransport transport,
                                           std::string api_key, std::string base_url,
                                           NativeProviderStreamTransport stream_transport,
                                           NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("mimo", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "output.structuredContent",
                        "transport.inline", "transport.reference"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("MiMoChatModelAdapter requires a transport callback.");
  }
}

AgentOutput MiMoChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "MiMo", "MIMO_API_KEY");
  const auto request = build_mimo_chat_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return build_output(apply_tagged_reasoning(
      parse_mimo_chat_response(raw, settings.model), settings));
}

std::vector<ModelStreamEvent> MiMoChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "MiMo", "MIMO_API_KEY");
  const auto request = build_mimo_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  return apply_tagged_reasoning_stream(
      parse_openai_chat_stream_events(chunks, "mimo", settings.model), settings);
}

void MiMoChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  require_chat_api_key(api_key_, "MiMo", "MIMO_API_KEY");
  const auto request = build_mimo_chat_stream_request(resolved_params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model](const std::vector<std::string>& chunks) {
        return parse_openai_chat_stream_events(chunks, "mimo", model);
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::SseEvent);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract MiMoChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::SseEvent);
}

AnthropicChatModelAdapter::AnthropicChatModelAdapter(std::string model, NativeProviderTransport transport,
                                                     std::string api_key, std::string base_url,
                                                     NativeProviderStreamTransport stream_transport,
                                                     NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("anthropic", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "output.structuredContent", "reasoning",
                        "reasoning.budget"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("AnthropicChatModelAdapter requires a transport callback.");
  }
}

AgentOutput AnthropicChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Anthropic", "ANTHROPIC_API_KEY");
  const auto request = build_anthropic_messages_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto response = build_output(apply_tagged_reasoning(
      parse_anthropic_messages_response(raw, "anthropic", settings.model),
      settings));
  attach_request_reasoning_metadata(response, request);
  return response;
}

std::vector<ModelStreamEvent> AnthropicChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Anthropic", "ANTHROPIC_API_KEY");
  const auto request = build_anthropic_messages_stream_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto events = apply_tagged_reasoning_stream(
      parse_anthropic_messages_stream_events(chunks, "anthropic", settings.model),
      settings);
  attach_request_reasoning_metadata(events, request);
  return events;
}

void AnthropicChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Anthropic", "ANTHROPIC_API_KEY");
  const auto request = build_anthropic_messages_stream_request(params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model,
       request_metadata = request.metadata](const std::vector<std::string>& chunks) {
        auto events = parse_anthropic_messages_stream_events(chunks, "anthropic", model);
        attach_reasoning_metadata(events, request_metadata);
        return events;
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::SseEvent);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract AnthropicChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::SseEvent);
}

DeepSeekChatModelAdapter::DeepSeekChatModelAdapter(std::string model, NativeProviderTransport transport,
                                                   std::string api_key, std::string base_url,
                                                   NativeProviderStreamTransport stream_transport,
                                                   NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("deepseek", std::move(model), 0.2, 1024,
                       {"input.text", "output.structuredContent", "reasoning"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("DeepSeekChatModelAdapter requires a transport callback.");
  }
}

AgentOutput DeepSeekChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "DeepSeek", "DEEPSEEK_API_KEY");
  const auto request = build_deepseek_messages_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto response = build_output(apply_tagged_reasoning(parse_deepseek_messages_response(raw, settings.model), settings));
  attach_request_reasoning_metadata(response, request);
  return response;
}

std::vector<ModelStreamEvent> DeepSeekChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "DeepSeek", "DEEPSEEK_API_KEY");
  const auto request = build_deepseek_messages_stream_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto events = apply_tagged_reasoning_stream(
      parse_anthropic_messages_stream_events(chunks, "deepseek", settings.model),
      settings);
  attach_request_reasoning_metadata(events, request);
  return events;
}

void DeepSeekChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "DeepSeek", "DEEPSEEK_API_KEY");
  const auto request = build_deepseek_messages_stream_request(params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model,
       request_metadata = request.metadata](const std::vector<std::string>& chunks) {
        auto events = parse_anthropic_messages_stream_events(chunks, "deepseek", model);
        attach_reasoning_metadata(events, request_metadata);
        return events;
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::SseEvent);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract DeepSeekChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::SseEvent);
}

OllamaChatModelAdapter::OllamaChatModelAdapter(std::string model, NativeProviderTransport transport,
                                               std::string base_url,
                                               NativeProviderStreamTransport stream_transport,
                                               NativeProviderStreamingTransport streaming_transport,
                                               ProviderReasoningMode reasoning_mode)
    : ChatModelAdapter("ollama", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "output.structuredContent", "reasoning"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      base_url_(std::move(base_url)),
      reasoning_mode_(reasoning_mode) {
  if (!transport_) {
    throw ConfigurationError("OllamaChatModelAdapter requires a transport callback.");
  }
}

AgentOutput OllamaChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  auto request = build_ollama_chat_request(resolved_params, settings.model, base_url_);
  if (reasoning_mode_ != ProviderReasoningMode::None) {
    apply_request_reasoning(request,
                            "ollama",
                            request.body.at("model").as_string(settings.model),
                            ProviderRequestProtocol::OllamaChat,
                            settings.reasoning,
                            reasoning_mode_);
  }
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto response = build_output(apply_tagged_reasoning(parse_ollama_chat_response(raw, settings.model), settings));
  attach_request_reasoning_metadata(response, request);
  return response;
}

std::vector<ModelStreamEvent> OllamaChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  auto request = build_ollama_chat_stream_request(resolved_params, settings.model, base_url_);
  if (reasoning_mode_ != ProviderReasoningMode::None) {
    apply_request_reasoning(request,
                            "ollama",
                            request.body.at("model").as_string(settings.model),
                            ProviderRequestProtocol::OllamaChat,
                            settings.reasoning,
                            reasoning_mode_);
  }
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto events = apply_tagged_reasoning_stream(
      parse_ollama_chat_stream_events(chunks, settings.model),
      settings);
  attach_request_reasoning_metadata(events, request);
  return events;
}

void OllamaChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  const auto resolved_params = params_with_resolved_settings(params, settings);
  auto request = build_ollama_chat_stream_request(resolved_params, settings.model, base_url_);
  if (reasoning_mode_ != ProviderReasoningMode::None) {
    apply_request_reasoning(request,
                            "ollama",
                            request.body.at("model").as_string(settings.model),
                            ProviderRequestProtocol::OllamaChat,
                            settings.reasoning,
                            reasoning_mode_);
  }
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model,
       request_metadata = request.metadata](const std::vector<std::string>& chunks) {
        auto events = parse_ollama_chat_stream_events(chunks, model);
        attach_reasoning_metadata(events, request_metadata);
        return events;
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::JsonLine);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract OllamaChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::JsonLine);
}

GeminiChatModelAdapter::GeminiChatModelAdapter(std::string model, NativeProviderTransport transport,
                                               std::string api_key, std::string base_url,
                                               NativeProviderStreamTransport stream_transport,
                                               NativeProviderStreamingTransport streaming_transport)
    : ChatModelAdapter("gemini", std::move(model), 0.2, 1024,
                       {"input.text", "input.image", "input.file", "input.audio", "input.video",
                        "output.structuredContent", "reasoning", "reasoning.budget",
                        "reasoning.includeThoughts"}),
      transport_(std::move(transport)),
      stream_transport_(std::move(stream_transport)),
      streaming_transport_(std::move(streaming_transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("GeminiChatModelAdapter requires a transport callback.");
  }
}

AgentOutput GeminiChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Gemini", "GEMINI_API_KEY");
  const auto request = build_gemini_generate_content_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto raw = transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto response = build_output(apply_tagged_reasoning(
      parse_gemini_generate_content_response(raw, settings.model),
      settings));
  attach_request_reasoning_metadata(response, request);
  return response;
}

std::vector<ModelStreamEvent> GeminiChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  if (!stream_transport_) {
    return provider_generated_response_stream_events(*this, params);
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Gemini", "GEMINI_API_KEY");
  const auto request = build_gemini_generate_content_stream_request(params, settings.model, api_key_, base_url_);
  throw_if_model_cancelled(params.cancellation);
  const auto chunks = stream_transport_(request);
  throw_if_model_cancelled(params.cancellation);
  auto events = apply_tagged_reasoning_stream(
      parse_gemini_generate_content_stream_events(chunks, settings.model),
      settings);
  attach_request_reasoning_metadata(events, request);
  return events;
}

void GeminiChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  if (!streaming_transport_) {
    emit_collected_model_stream_events(collect_stream_events(params), on_event);
    return;
  }
  const auto settings = resolve_settings(params.settings);
  require_chat_api_key(api_key_, "Gemini", "GEMINI_API_KEY");
  const auto request = build_gemini_generate_content_stream_request(params, settings.model, api_key_, base_url_);
  IncrementalParsedStreamEmitter emitter(
      [model = settings.model,
       request_metadata = request.metadata](const std::vector<std::string>& chunks) {
        auto events = parse_gemini_generate_content_stream_events(chunks, model);
        attach_reasoning_metadata(events, request_metadata);
        return events;
      },
      settings,
      IncrementalParsedStreamEmitter::FrameBoundary::SseEvent);
  emit_incremental_provider_stream(streaming_transport_,
                                   request,
                                   std::move(emitter),
                                   params.cancellation,
                                   on_event);
}

ModelProviderStreamContract GeminiChatModelAdapter::stream_contract() const {
  return provider_stream_contract_for(*this, ModelStreamFrameBoundary::SseEvent);
}

LlamaCppNativeChatModelAdapter::LlamaCppNativeChatModelAdapter(
    LlamaCppNativeChatModelAdapterConfig config)
    : ChatModelAdapter("llamacpp-native",
                       default_llama_model_name(config.model, config.model_path),
                       config.temperature.value_or(0.2),
                       config.max_output_tokens.value_or(1024),
                       llama_model_capabilities(config),
                       config.reasoning),
      native_config_(config),
      sampling_(config),
      chat_template_(std::move(config.chat_template)),
      strict_template_(config.strict_template),
      tool_mode_(config.tool_mode.empty() ? "auto" : std::move(config.tool_mode)),
      grammar_(std::move(config.grammar)),
      grammar_root_(std::move(config.grammar_root)),
      session_id_(std::move(config.session_id)),
      binding_(config.binding ? std::move(config.binding) : create_llama_cpp_native_binding()) {
  if (native_config_.model_path.empty()) {
    throw ConfigurationError("LlamaCppNativeChatModelAdapter requires model_path.");
  }
  if (!binding_ || !binding_->generate_chat) {
    throw ConfigurationError("LlamaCppNativeChatModelAdapter requires a native binding.");
  }
}

const LlamaCppNativeRuntimeConfig& LlamaCppNativeChatModelAdapter::native_config() const noexcept {
  return native_config_;
}

LlamaCppNativeChatRequest LlamaCppNativeChatModelAdapter::build_request(
    const GenerateParams& params,
    const ModelSettings& settings) const {
  const auto tool_mode = resolve_llama_tool_mode(tool_mode_, settings.extra);
  const bool tools_active = !params.tools.empty() && tool_mode != "ignore";
  if (!params.tools.empty() && tool_mode == "error") {
    throw AdapterError("llama.cpp native provider does not support tool calling.");
  }

  LlamaCppNativeChatRequest request;
  request.request_id = generate_uuid();
  request.model = settings.model;
  request.tools = tools_active ? params.tools : std::vector<ChatToolDescriptor>{};
  request.tool_mode = tool_mode;
  request.chat_template = chat_template_;
  request.strict_template = strict_template_;
  request.grammar_root = optional_string_from_value(settings.extra, "grammarRoot",
                         optional_string_from_value(settings.extra, "grammar_root", grammar_root_));
  request.session_id = optional_string_from_value(settings.extra, "sessionId",
                       optional_string_from_value(settings.extra, "session_id", session_id_));
  request.reasoning_tag_name = settings.reasoning && !settings.reasoning->tag_name.empty()
                                   ? settings.reasoning->tag_name
                                   : "think";
  request.output_schema = resolve_llama_output_schema(settings.extra);
  request.temperature = settings.temperature ? settings.temperature : sampling_.temperature;
  request.max_output_tokens = settings.max_output_tokens ? settings.max_output_tokens
                                                         : sampling_.max_output_tokens;
  request.top_k = optional_int_from_value(settings.extra, "topK");
  request.top_p = optional_number_from_value(settings.extra, "topP");
  request.min_p = optional_number_from_value(settings.extra, "minP");
  request.repeat_penalty = optional_number_from_value(settings.extra, "repeatPenalty");
  request.seed = optional_int_from_value(settings.extra, "seed");
  if (!request.top_k) request.top_k = sampling_.top_k;
  if (!request.top_p) request.top_p = sampling_.top_p;
  if (!request.min_p) request.min_p = sampling_.min_p;
  if (!request.repeat_penalty) request.repeat_penalty = sampling_.repeat_penalty;
  if (!request.seed) request.seed = sampling_.seed;

  std::vector<std::string> instructions;
  if (tools_active) {
    instructions.push_back(build_llama_tool_instruction(request.tools, request.tool_mode));
  }
  if (request.output_schema) {
    instructions.push_back(build_llama_structured_output_instruction(*request.output_schema));
  }
  if (!instructions.empty()) {
    request.messages.push_back(LlamaCppNativeChatMessage{
        .role = "system",
        .content = join_strings(instructions, "\n\n"),
    });
  }

  for (const auto& message : params.messages) {
    auto serialized = serialize_llama_message(message,
                                              !native_config_.mmproj_path.empty(),
                                              native_config_.media_marker);
    request.messages.push_back(std::move(serialized.message));
    request.media.insert(request.media.end(),
                         std::make_move_iterator(serialized.media.begin()),
                         std::make_move_iterator(serialized.media.end()));
  }

  request.grammar = optional_string_from_value(settings.extra, "grammar");
  if (request.grammar.empty()) {
    if (tools_active) {
      request.grammar = llama_tool_envelope_gbnf(request.tools, request.tool_mode == "required");
    } else if (request.output_schema) {
      request.grammar = json_schema_to_gbnf(*request.output_schema);
    } else {
      request.grammar = grammar_;
    }
  }
  return request;
}

AgentOutput LlamaCppNativeChatModelAdapter::build_model_output(
    const LlamaCppNativeChatResult& raw,
    const LlamaCppNativeChatRequest& request,
    bool reasoning_enabled,
    const std::string& fallback_text) const {
  const auto raw_text = raw.text.empty() ? fallback_text : raw.text;
  const auto extracted = reasoning_enabled
                             ? extract_tagged_reasoning(raw_text, request.reasoning_tag_name.empty()
                                                                      ? std::string("think")
                                                                      : request.reasoning_tag_name)
                             : TaggedReasoningExtraction{.text = raw_text};
  const auto parsed = parse_llama_tool_envelope(extracted.text,
                                                request.tools,
                                                request.tool_mode,
                                                request.output_schema);
  AgentOutput response;
  response.id = raw.id;
  response.provider = "llamacpp-native";
  response.model = raw.model.empty() ? model() : raw.model;
  response.text = parsed.text;
  response.content = response.text.empty() ? std::vector<MessageContentPart>{}
                                           : std::vector<MessageContentPart>{text_part(response.text)};
  response.reasoning = merge_model_reasoning(raw.reasoning, extracted.reasoning);
  response.tool_calls = parsed.tool_calls;
  response.finish_reason = normalize_finish_reason(parsed.finish_reason.empty()
                                                       ? raw.finish_reason
                                                       : parsed.finish_reason);
  response.raw = raw.raw.is_null() ? llama_chat_result_to_value(raw) : raw.raw;
  return build_output(std::move(response));
}

AgentOutput LlamaCppNativeChatModelAdapter::generate(const GenerateParams& params) {
  const auto settings = resolve_settings(params.settings);
  const auto request = build_request(params, settings);
  CancellationCallbackScope cancellation(params.cancellation, binding_, request.request_id);
  cancellation.throw_if_cancelled();
  const auto raw = binding_->generate_chat(native_config_, request, {});
  cancellation.throw_if_cancelled();
  return build_model_output(raw,
                              request,
                              settings.reasoning && settings.reasoning->enabled == true);
}

std::vector<ModelStreamEvent> LlamaCppNativeChatModelAdapter::collect_stream_events(const GenerateParams& params) {
  std::vector<ModelStreamEvent> events;
  stream(params, [&](const ModelStreamEvent& event) {
    events.push_back(event);
  });
  return events;
}

void LlamaCppNativeChatModelAdapter::stream(const GenerateParams& params, StreamEventHandler on_event) {
  const auto settings = resolve_settings(params.settings);
  const auto request = build_request(params, settings);
  CancellationCallbackScope cancellation(params.cancellation, binding_, request.request_id);
  cancellation.throw_if_cancelled();
  const bool tools_active = !request.tools.empty() && request.tool_mode != "ignore";
  const bool reasoning_enabled = !tools_active && settings.reasoning && settings.reasoning->enabled == true;
  std::uint64_t event_sequence = 0;
  const auto emit_event = [&](ModelStreamEvent event) {
    if (!on_event) {
      return;
    }
    event.schema_version = kAgentStreamEventSchemaVersion;
    if (event.sequence == 0) {
      event.sequence = ++event_sequence;
    } else {
      event_sequence = event.sequence;
    }
    on_event(event);
  };
  if (on_event) {
    emit_event(ModelStreamEvent{
        .type = ModelStreamEventType::ResponseStart,
        .provider = "llamacpp-native",
        .model = request.model,
    });
  }
  TaggedReasoningStreamParser parser(request.reasoning_tag_name.empty()
                                         ? std::string("think")
                                         : request.reasoning_tag_name);
  std::string visible_text;
  std::string reasoning_text;
  Utf8StreamBuffer visible_utf8("llama.cpp native text delta");
  Utf8StreamBuffer reasoning_utf8("llama.cpp native reasoning delta");
  const auto emit_reasoning_delta = [&](std::string piece) {
    if (piece.empty()) {
      return;
    }
    reasoning_text += piece;
    if (on_event) {
      emit_event(ModelStreamEvent{
          .type = ModelStreamEventType::ReasoningDelta,
          .provider = "llamacpp-native",
          .model = request.model,
          .delta = piece,
          .reasoning = reasoning_text,
      });
    }
  };
  const auto emit_visible_delta = [&](std::string piece) {
    if (piece.empty()) {
      return;
    }
    visible_text += piece;
    if (!tools_active && on_event) {
      emit_event(ModelStreamEvent{
          .type = ModelStreamEventType::TextDelta,
          .provider = "llamacpp-native",
          .model = request.model,
          .delta = piece,
          .text = visible_text,
      });
    }
  };
  const auto feed_tagged_delta = [&](const std::string& piece) {
    if (piece.empty()) {
      return;
    }
    for (const auto& parsed : parser.feed(piece)) {
      if (parsed.type == TaggedReasoningDeltaType::Reasoning) {
        if (on_event) {
          emit_event(ModelStreamEvent{
              .type = ModelStreamEventType::ReasoningDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .reasoning = parsed.reasoning_text,
          });
        }
      } else {
        visible_text = parsed.text;
        if (on_event) {
          emit_event(ModelStreamEvent{
              .type = ModelStreamEventType::TextDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .text = parsed.text,
          });
        }
      }
    }
  };
  const auto append_delta = [&](const LlamaCppNativeChatDelta& delta) {
    if (delta.type == "reasoning") {
      emit_reasoning_delta(reasoning_utf8.push(delta.delta));
      return;
    }
    const auto piece = visible_utf8.push(delta.delta);
    if (!reasoning_enabled) {
      emit_visible_delta(piece);
      return;
    }
    feed_tagged_delta(piece);
  };
  const auto raw = binding_->generate_chat(native_config_, request, append_delta);
  cancellation.throw_if_cancelled();
  emit_reasoning_delta(reasoning_utf8.finish());
  const auto visible_tail = visible_utf8.finish();
  if (!reasoning_enabled) {
    emit_visible_delta(visible_tail);
  } else {
    feed_tagged_delta(visible_tail);
  }
  if (reasoning_enabled) {
    for (const auto& parsed : parser.finish()) {
      if (parsed.type == TaggedReasoningDeltaType::Reasoning) {
        if (on_event) {
          emit_event(ModelStreamEvent{
              .type = ModelStreamEventType::ReasoningDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .reasoning = parsed.reasoning_text,
          });
        }
      } else {
        visible_text = parsed.text;
        if (on_event) {
          emit_event(ModelStreamEvent{
              .type = ModelStreamEventType::TextDelta,
              .provider = "llamacpp-native",
              .model = request.model,
              .delta = parsed.delta,
              .text = parsed.text,
          });
        }
      }
    }
  }
  if (on_event) {
    emit_event(ModelStreamEvent{
        .type = ModelStreamEventType::Response,
        .provider = "llamacpp-native",
        .model = raw.model.empty() ? request.model : raw.model,
        .response = build_model_output(raw,
                                         request,
                                         reasoning_enabled,
                                         visible_text),
    });
  }
}
