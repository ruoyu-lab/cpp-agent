OpenAIEmbeddingAdapter::OpenAIEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                               std::string api_key, std::string base_url,
                                               std::string organization, int dimensions, std::string space_id)
    : TextEmbeddingAdapter("openai", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      organization_(std::move(organization)) {
  if (!transport_) {
    throw ConfigurationError("OpenAIEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> OpenAIEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                           const Value& settings,
                                                           CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (api_key_.empty()) {
    throw ConfigurationError("OpenAI embeddings require apiKey or OPENAI_API_KEY.");
  }
  Value body = Value::object({{"model", embedding_model_from_settings(settings, model())},
                              {"input", text_embedding_input_value(texts)},
                              {"encoding_format", "float"}});
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  if (dims > 0) {
    body["dimensions"] = dims;
  }
  std::map<std::string, std::string> headers{{"content-type", "application/json"},
                                             {"authorization", "Bearer " + api_key_}};
  if (!organization_.empty()) {
    headers["openai-organization"] = organization_;
  }
  throw_if_model_cancelled(cancellation);
  const auto raw = transport_(NativeProviderRequest{
      .provider = "openai",
      .endpoint = "/embeddings",
      .body = body,
      .headers = std::move(headers),
      .base_url = trim_provider_base_url(base_url_),
      .cancellation = cancellation,
  });
  throw_if_model_cancelled(cancellation);
  return indexed_embedding_vectors_from_response(raw);
}

QwenEmbeddingAdapter::QwenEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                           std::string api_key, std::string base_url, int dimensions,
                                           std::string space_id)
    : TextEmbeddingAdapter("qwen", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("QwenEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> QwenEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                         const Value& settings,
                                                         CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (api_key_.empty()) {
    throw ConfigurationError("Qwen embeddings require apiKey or QWEN_API_KEY.");
  }
  Value body = Value::object({{"model", embedding_model_from_settings(settings, model())},
                              {"input", text_embedding_input_value(texts)},
                              {"encoding_format", "float"}});
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  if (dims > 0) {
    body["dimensions"] = dims;
  }
  throw_if_model_cancelled(cancellation);
  const auto raw = transport_(NativeProviderRequest{
      .provider = "qwen",
      .endpoint = "/embeddings",
      .body = body,
      .headers = {{"content-type", "application/json"}, {"authorization", "Bearer " + api_key_}},
      .base_url = trim_provider_base_url(base_url_),
      .cancellation = cancellation,
  });
  throw_if_model_cancelled(cancellation);
  return indexed_embedding_vectors_from_response(raw);
}

OllamaEmbeddingAdapter::OllamaEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                               std::string base_url, int dimensions, std::string space_id)
    : TextEmbeddingAdapter("ollama", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      base_url_(std::move(base_url)) {
  if (!transport_) {
    throw ConfigurationError("OllamaEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> OllamaEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                           const Value& settings,
                                                           CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  Value body = Value::object({{"model", embedding_model_from_settings(settings, model())},
                              {"input", text_embedding_input_value(texts)},
                              {"truncate", true}});
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  if (dims > 0) {
    body["dimensions"] = dims;
  }
  auto base_url = trim_provider_base_url(base_url_);
  if (base_url.size() < 4 || base_url.substr(base_url.size() - 4) != "/api") {
    base_url += "/api";
  }
  throw_if_model_cancelled(cancellation);
  const auto raw = transport_(NativeProviderRequest{
      .provider = "ollama",
      .endpoint = "/embed",
      .body = body,
      .headers = {{"content-type", "application/json"}},
      .base_url = std::move(base_url),
      .cancellation = cancellation,
  });
  throw_if_model_cancelled(cancellation);
  return embedding_vectors_from_array_response(raw.at("embeddings"));
}

GeminiEmbeddingAdapter::GeminiEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                                               std::string api_key, std::string base_url, int dimensions,
                                               std::string task_type, std::string space_id)
    : TextEmbeddingAdapter("gemini", std::move(model), dimensions, std::move(space_id)),
      transport_(std::move(transport)),
      api_key_(std::move(api_key)),
      base_url_(std::move(base_url)),
      task_type_(std::move(task_type)) {
  if (!transport_) {
    throw ConfigurationError("GeminiEmbeddingAdapter requires a transport callback.");
  }
}

std::vector<EmbeddingVector> GeminiEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                           const Value& settings,
                                                           CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (api_key_.empty()) {
    throw ConfigurationError("Gemini embeddings require apiKey or GEMINI_API_KEY.");
  }
  const auto resolved_model = embedding_model_from_settings(settings, model());
  const auto model_resource = "models/" + resolved_model;
  const int dims = optional_dimensions_from_settings(settings, dimensions());
  const auto task_type = embedding_task_type_from_settings(settings, task_type_);
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(texts.size());
  for (const auto& text : texts) {
    throw_if_model_cancelled(cancellation);
    Value body = Value::object({
        {"model", model_resource},
        {"content", Value::object({
                        {"parts", Value::array({
                                      Value::object({{"text", text}}),
                                    })},
                    })},
    });
    if (!task_type.empty()) {
      body["taskType"] = task_type;
    }
    if (dims > 0) {
      body["outputDimensionality"] = dims;
    }
    const auto raw = transport_(NativeProviderRequest{
        .provider = "gemini",
        .endpoint = "/" + model_resource + ":embedContent",
        .body = body,
        .headers = {{"content-type", "application/json"}, {"x-goog-api-key", api_key_}},
        .base_url = trim_provider_base_url(base_url_),
        .cancellation = cancellation,
    });
    throw_if_model_cancelled(cancellation);
    vectors.push_back(embedding_vector_from_value(raw.at("embedding").at("values")));
  }
  return vectors;
}

LlamaCppNativeTextEmbeddingAdapter::LlamaCppNativeTextEmbeddingAdapter(
    LlamaCppNativeEmbeddingAdapterConfig config)
    : TextEmbeddingAdapter("llamacpp-native",
                           default_llama_model_name(config.model, config.model_path),
                           config.dimensions.value_or(0),
                           config.space_id.empty() ? "llamacpp-native:" +
                                                         default_llama_model_name(config.model, config.model_path)
                                                   : config.space_id),
      native_config_(config),
      pooling_(config.pooling.empty() ? "mean" : std::move(config.pooling)),
      normalize_(config.normalize),
      binding_(config.binding ? std::move(config.binding) : create_llama_cpp_native_binding()) {
  if (native_config_.model_path.empty()) {
    throw ConfigurationError("LlamaCppNativeTextEmbeddingAdapter requires model_path.");
  }
  if (!binding_ || !binding_->embed_texts) {
    throw ConfigurationError("LlamaCppNativeTextEmbeddingAdapter requires a native binding.");
  }
}

const LlamaCppNativeRuntimeConfig& LlamaCppNativeTextEmbeddingAdapter::native_config() const noexcept {
  return native_config_;
}

std::vector<EmbeddingVector> LlamaCppNativeTextEmbeddingAdapter::embed(
    const std::vector<std::string>& texts,
    const Value& settings,
    CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  throw_if_model_cancelled(cancellation);
  const auto resolved = resolve_settings(embedding_settings_from_value(settings));
  LlamaCppNativeEmbeddingRequest request;
  request.request_id = generate_uuid();
  request.model = resolved.model;
  request.texts = texts;
  request.pooling = optional_string_from_value(settings, "pooling", pooling_);
  request.normalize = settings.is_object() && settings.at("normalize").is_bool()
                          ? settings.at("normalize").as_bool()
                          : normalize_;
  request.dimensions = resolved.dimensions;
  CancellationCallbackScope cancellation_scope(cancellation, binding_, request.request_id);
  const auto raw = binding_->embed_texts(native_config_, request);
  cancellation_scope.throw_if_cancelled();
  const int expected_dimensions = resolved.dimensions.value_or(dimensions());
  if (expected_dimensions > 0) {
    for (std::size_t index = 0; index < raw.embeddings.size(); ++index) {
      if (raw.embeddings[index].size() != static_cast<std::size_t>(expected_dimensions)) {
        throw AdapterError("llama.cpp native embedding dimensions mismatch at index " +
                           std::to_string(index) + ": expected " +
                           std::to_string(expected_dimensions) + ", received " +
                           std::to_string(raw.embeddings[index].size()) + ".");
      }
    }
  }
  return raw.embeddings;
}
