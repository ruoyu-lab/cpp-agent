#include "agent/app_api.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <cctype>

namespace agent {

namespace {

template <typename T>
T* get_tool_service(ToolExecutionContext& context, const ToolServiceToken<T>& token) {
  return context.service_refs.service_view.get(token);
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool contains_kind(const std::vector<MediaGenerationKind>& kinds, MediaGenerationKind kind) {
  return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

std::string default_mime_type(MediaGenerationKind kind, const std::string& format = {}) {
  const auto normalized = lower_copy(format);
  if (!normalized.empty() && normalized.find('/') != std::string::npos) {
    return normalized;
  }
  if (kind == MediaGenerationKind::Image) {
    if (normalized == "jpg" || normalized == "jpeg") return "image/jpeg";
    if (normalized == "webp") return "image/webp";
    if (normalized == "gif") return "image/gif";
    if (normalized == "svg") return "image/svg+xml";
    return "image/png";
  }
  if (kind == MediaGenerationKind::Video) {
    if (normalized == "webm") return "video/webm";
    if (normalized == "mov") return "video/quicktime";
    return "video/mp4";
  }
  if (normalized == "mp3") return "audio/mpeg";
  if (normalized == "ogg") return "audio/ogg";
  if (normalized == "aac") return "audio/aac";
  return "audio/wav";
}

std::string default_extension(const std::string& mime_type) {
  if (mime_type == "image/jpeg") return ".jpg";
  if (mime_type == "image/webp") return ".webp";
  if (mime_type == "image/gif") return ".gif";
  if (mime_type == "image/svg+xml") return ".svg";
  if (mime_type == "video/webm") return ".webm";
  if (mime_type == "video/quicktime") return ".mov";
  if (mime_type == "video/mp4") return ".mp4";
  if (mime_type == "audio/mpeg") return ".mp3";
  if (mime_type == "audio/ogg") return ".ogg";
  if (mime_type == "audio/aac") return ".aac";
  if (mime_type == "audio/wav") return ".wav";
  return ".bin";
}

std::string default_filename(MediaGenerationKind kind,
                             std::size_t index,
                             const std::string& mime_type) {
  return to_string(kind) + "-" + std::to_string(index + 1) + default_extension(mime_type);
}

std::string sanitize_artifact_component(std::string value) {
  std::string output;
  output.reserve(value.size());
  bool previous_dash = false;
  for (const unsigned char ch : value) {
    const char lower = static_cast<char>(std::tolower(ch));
    if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9') ||
        lower == '.' || lower == '_' || lower == '-') {
      output.push_back(lower);
      previous_dash = false;
    } else if (!previous_dash) {
      output.push_back('-');
      previous_dash = true;
    }
  }
  while (!output.empty() && output.front() == '-') {
    output.erase(output.begin());
  }
  while (!output.empty() && output.back() == '-') {
    output.pop_back();
  }
  return output.empty() ? "generated" : output;
}

std::string sanitize_artifact_prefix(const std::string& value) {
  std::string output;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto slash = value.find('/', start);
    const auto end = slash == std::string::npos ? value.size() : slash;
    const auto segment = value.substr(start, end - start);
    if (!segment.empty()) {
      if (!output.empty()) {
        output += "/";
      }
      output += sanitize_artifact_component(segment);
    }
    if (slash == std::string::npos) {
      break;
    }
    start = slash + 1;
  }
  return output.empty() ? "media/generated" : output;
}

bool has_media_source_payload(const MediaSource& source) {
  switch (source.kind) {
    case MediaSourceKind::Inline:
      return !source.data.empty();
    case MediaSourceKind::Url:
      return !source.url.empty();
    case MediaSourceKind::Path:
      return !source.path.empty();
    case MediaSourceKind::Artifact:
      return !source.key.empty();
  }
  return false;
}

Value string_vector_value(const std::vector<std::string>& values) {
  Value::Array output;
  output.reserve(values.size());
  for (const auto& value : values) {
    output.push_back(value);
  }
  return Value(std::move(output));
}

Value kind_vector_value(const std::vector<MediaGenerationKind>& kinds) {
  Value::Array output;
  output.reserve(kinds.size());
  for (const auto kind : kinds) {
    output.push_back(to_string(kind));
  }
  return Value(std::move(output));
}

MediaSource media_source_from_tool_value(const Value& value) {
  if (!value.is_object()) {
    throw ConfigurationError("Media input source must be an object.");
  }
  MediaSource source;
  const std::string kind = value.at("kind").as_string();
  if (kind == "url") {
    source.kind = MediaSourceKind::Url;
    source.url = value.at("url").as_string();
  } else if (kind == "path") {
    source.kind = MediaSourceKind::Path;
    source.path = value.at("path").as_string();
  } else if (kind == "artifact") {
    source.kind = MediaSourceKind::Artifact;
    source.key = value.at("key").as_string();
  } else {
    source.kind = MediaSourceKind::Inline;
    source.data = value.at("data").as_string();
  }
  source.mime_type = value.at("mimeType").as_string();
  source.filename = value.at("filename").as_string();
  return source;
}

std::vector<MediaGenerationInputAsset> media_generation_inputs_from_value(const Value& value) {
  std::vector<MediaGenerationInputAsset> inputs;
  if (!value.is_array()) {
    return inputs;
  }
  for (const auto& item : value.as_array()) {
    if (!item.is_object()) {
      continue;
    }
    inputs.push_back(MediaGenerationInputAsset{
        .kind = media_generation_kind_from_string(item.at("kind").as_string("image")),
        .source = media_source_from_tool_value(item.at("source")),
        .role = item.at("role").as_string(),
        .metadata = item.at("metadata").is_object() ? item.at("metadata") : Value::object({}),
    });
  }
  return inputs;
}

MediaGenerationRequest media_generation_request_from_tool_input(
    MediaGenerationKind kind,
    const Value& input) {
  MediaGenerationRequest request;
  request.kind = kind;
  request.prompt = input.at("prompt").as_string();
  request.negative_prompt = input.at("negativePrompt").as_string();
  request.model = input.at("model").as_string();
  request.format = input.at("format").as_string();
  if (input.at("width").is_number()) {
    request.width = static_cast<int>(input.at("width").as_integer());
  }
  if (input.at("height").is_number()) {
    request.height = static_cast<int>(input.at("height").as_integer());
  }
  if (input.at("durationSeconds").is_number()) {
    request.duration_seconds = input.at("durationSeconds").as_number();
  }
  if (input.at("sampleRateHz").is_number()) {
    request.sample_rate_hz = static_cast<int>(input.at("sampleRateHz").as_integer());
  }
  if (input.at("seed").is_number()) {
    request.seed = static_cast<int>(input.at("seed").as_integer());
  }
  if (input.at("count").is_number()) {
    request.count = static_cast<std::size_t>(std::max<long long>(0, input.at("count").as_integer()));
  }
  request.inputs = media_generation_inputs_from_value(input.at("inputs"));
  request.metadata = input.at("metadata").is_object() ? input.at("metadata") : Value::object({});
  return request;
}

void normalize_media_generation_output(AgentOutput& result,
                                       const MediaGenerationProvider& provider,
                                       const MediaGenerationRequest& request) {
  if (result.id.empty()) {
    result.id = generate_uuid();
  }
  if (result.provider.empty()) {
    result.provider = provider.metadata().name;
  }
  if (result.model.empty()) {
    result.model = request.model;
  }
  if (!result.metadata.is_object()) {
    result.metadata = Value::object({});
  }
  result.metadata["kind"] = to_string(request.kind);
  for (std::size_t index = 0; index < result.artifacts.size(); ++index) {
    auto& asset = result.artifacts[index];
    asset.kind = to_string(request.kind);
    if (asset.id.empty()) {
      asset.id = generate_uuid();
    }
    if (asset.mime_type.empty()) {
      asset.mime_type = default_mime_type(request.kind, request.format);
    }
    if (asset.filename.empty()) {
      asset.filename = default_filename(request.kind, index, asset.mime_type);
    }
  }
}

JsonSchema media_generation_tool_schema(MediaGenerationKind kind) {
  JsonSchema schema;
  schema.type = JsonSchemaType::Object;
  schema.required = {"prompt"};
  schema.properties["prompt"].type = JsonSchemaType::String;
  schema.properties["negativePrompt"].type = JsonSchemaType::String;
  schema.properties["provider"].type = JsonSchemaType::String;
  schema.properties["model"].type = JsonSchemaType::String;
  schema.properties["format"].type = JsonSchemaType::String;
  schema.properties["count"].type = JsonSchemaType::Integer;
  schema.properties["seed"].type = JsonSchemaType::Integer;
  schema.properties["metadata"].type = JsonSchemaType::Object;
  schema.properties["inputs"].type = JsonSchemaType::Array;
  schema.properties["inputs"].items = std::make_shared<JsonSchema>();
  schema.properties["inputs"].items->type = JsonSchemaType::Object;
  if (kind == MediaGenerationKind::Image || kind == MediaGenerationKind::Video) {
    schema.properties["width"].type = JsonSchemaType::Integer;
    schema.properties["height"].type = JsonSchemaType::Integer;
  }
  if (kind == MediaGenerationKind::Video || kind == MediaGenerationKind::Audio) {
    schema.properties["durationSeconds"].type = JsonSchemaType::Number;
  }
  if (kind == MediaGenerationKind::Audio) {
    schema.properties["sampleRateHz"].type = JsonSchemaType::Integer;
  }
  return schema;
}

Value media_generation_progress_value(const MediaGenerationProgress& progress) {
  Value payload = Value::object({
      {"stage", progress.stage},
      {"message", progress.message},
      {"provider", progress.provider},
      {"kind", to_string(progress.kind)},
      {"assetId", progress.asset_id},
      {"metadata", progress.metadata},
  });
  if (progress.fraction) {
    payload["fraction"] = *progress.fraction;
  }
  return payload;
}

std::vector<MessageContentPart> resolvable_media_generation_content(
    const AgentOutput& result,
    bool allow_inline_data) {
  try {
    return media_generation_output_to_content(result, allow_inline_data);
  } catch (const ConfigurationError&) {
    return {};
  }
}

std::string preferred_media_artifact_key(
    const AgentArtifact& asset,
    const MediaGenerationOutputValueOptions& options) {
  if (asset.source.kind == MediaSourceKind::Artifact && !asset.source.key.empty()) {
    return asset.source.key;
  }
  const std::string prefix = sanitize_artifact_prefix(options.artifact_key_prefix.empty()
                                                         ? "media/generated"
                                                         : options.artifact_key_prefix);
  const std::string id = sanitize_artifact_component(asset.id.empty() ? generate_uuid() : asset.id);
  const std::string kind = sanitize_artifact_component(asset.kind.empty() ? "media" : asset.kind);
  return prefix + "/" + kind + "/" + id;
}

std::vector<std::uint8_t> asset_bytes(const AgentArtifact& asset) {
  if (!asset.bytes.empty()) {
    return asset.bytes;
  }
  if (asset.source.kind == MediaSourceKind::Inline && !asset.source.data.empty()) {
    return decode_base64(asset.source.data);
  }
  return {};
}

MediaGenerationKind artifact_media_generation_kind(const AgentArtifact& asset) {
  return media_generation_kind_from_string(asset.kind);
}

AgentArtifact store_media_artifact(InMemoryArtifactStore* store,
                                          const AgentArtifact& asset,
                                          const AgentOutput& result,
                                          const MediaGenerationOutputValueOptions& options) {
  if (!store) {
    throw ConfigurationError("Media artifact writer requires an InMemoryArtifactStore.");
  }
  const auto key = preferred_media_artifact_key(asset, options);
  store->set(key, media_generation_artifact_to_value(asset));
  store->append_log(key, "media.generated", Value::object({
      {"generationId", result.id},
      {"provider", result.provider},
      {"model", result.model},
      {"kind", asset.kind},
      {"assetId", asset.id},
  }));
  AgentArtifact written = asset;
  written.source.kind = MediaSourceKind::Artifact;
  written.source.key = key;
  written.source.mime_type = written.mime_type;
  written.source.filename = written.filename;
  written.source = media_generation_artifact_to_media_source(written, false);
  return written;
}

AgentArtifact store_file_media_artifact(FileArtifactStore* store,
                                               const AgentArtifact& asset,
                                               const AgentOutput& result,
                                               const MediaGenerationOutputValueOptions& options) {
  if (!store) {
    throw ConfigurationError("Media artifact writer requires a FileArtifactStore.");
  }
  const auto key = preferred_media_artifact_key(asset, options);
  store->set(key, media_generation_artifact_to_value(asset));
  store->append_log(key, "media.generated", Value::object({
      {"generationId", result.id},
      {"provider", result.provider},
      {"model", result.model},
      {"kind", asset.kind},
      {"assetId", asset.id},
  }));
  AgentArtifact written = asset;
  written.source.kind = MediaSourceKind::Artifact;
  written.source.key = key;
  written.source.mime_type = written.mime_type;
  written.source.filename = written.filename;
  written.source = media_generation_artifact_to_media_source(written, false);
  return written;
}

}  // namespace

std::string to_string(MediaGenerationKind kind) {
  switch (kind) {
    case MediaGenerationKind::Image:
      return "image";
    case MediaGenerationKind::Video:
      return "video";
    case MediaGenerationKind::Audio:
      return "audio";
  }
  return "image";
}

MediaGenerationKind media_generation_kind_from_string(const std::string& value) {
  const auto normalized = lower_copy(value);
  if (normalized == "video") return MediaGenerationKind::Video;
  if (normalized == "audio") return MediaGenerationKind::Audio;
  return MediaGenerationKind::Image;
}

bool MediaGenerationProvider::supports(MediaGenerationKind kind) const {
  return contains_kind(capabilities().kinds, kind);
}

std::vector<SchemaValidationIssue> MediaGenerationProvider::validate(
    const MediaGenerationRequest& request) const {
  std::vector<SchemaValidationIssue> issues;
  if (!supports(request.kind)) {
    issues.push_back(SchemaValidationIssue{
        .path = "$.kind",
        .message = "Provider \"" + metadata().name + "\" does not support " +
                   to_string(request.kind) + ".",
    });
  }
  if (trim_copy(request.prompt).empty()) {
    issues.push_back(SchemaValidationIssue{.path = "$.prompt", .message = "Prompt is required."});
  }
  if (request.count == 0) {
    issues.push_back(SchemaValidationIssue{.path = "$.count", .message = "Count must be at least 1."});
  }
  if (request.width && *request.width <= 0) {
    issues.push_back(SchemaValidationIssue{.path = "$.width", .message = "Width must be positive."});
  }
  if (request.height && *request.height <= 0) {
    issues.push_back(SchemaValidationIssue{.path = "$.height", .message = "Height must be positive."});
  }
  if (request.duration_seconds && *request.duration_seconds <= 0) {
    issues.push_back(SchemaValidationIssue{
        .path = "$.durationSeconds",
        .message = "Duration must be positive.",
    });
  }
  if (request.sample_rate_hz && *request.sample_rate_hz <= 0) {
    issues.push_back(SchemaValidationIssue{
        .path = "$.sampleRateHz",
        .message = "Sample rate must be positive.",
    });
  }
  return issues;
}

MediaGenerationProviderRegistry::MediaGenerationProviderRegistry(
    std::vector<std::shared_ptr<MediaGenerationProvider>> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

MediaGenerationProviderRegistry::MediaGenerationProviderRegistry(
    const MediaGenerationProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
}

MediaGenerationProviderRegistry& MediaGenerationProviderRegistry::operator=(
    const MediaGenerationProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  return *this;
}

MediaGenerationProviderRegistry::MediaGenerationProviderRegistry(
    MediaGenerationProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
}

MediaGenerationProviderRegistry& MediaGenerationProviderRegistry::operator=(
    MediaGenerationProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  return *this;
}

std::shared_ptr<MediaGenerationProvider> MediaGenerationProviderRegistry::register_provider(
    std::shared_ptr<MediaGenerationProvider> provider) {
  if (!provider) {
    throw ConfigurationError("Media generation provider cannot be null.");
  }
  if (provider->metadata().name.empty()) {
    throw ConfigurationError("Media generation provider metadata.name is required.");
  }
  const auto name = provider->metadata().name;
  std::lock_guard<std::mutex> lock(mutex_);
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

std::shared_ptr<MediaGenerationProvider> MediaGenerationProviderRegistry::get(
    const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : found->second;
}

std::shared_ptr<MediaGenerationProvider> MediaGenerationProviderRegistry::require(
    const std::string& name) const {
  auto provider = get(name);
  if (!provider) {
    throw ConfigurationError("Unknown media generation provider: " + name);
  }
  return provider;
}

std::shared_ptr<MediaGenerationProvider> MediaGenerationProviderRegistry::find_by_kind(
    MediaGenerationKind kind) const {
  const auto providers = list();
  for (const auto& provider : providers) {
    if (provider->supports(kind)) {
      return provider;
    }
  }
  return nullptr;
}

std::shared_ptr<MediaGenerationProvider> MediaGenerationProviderRegistry::require_by_kind(
    MediaGenerationKind kind,
    const std::string& preferred_provider) const {
  auto provider = preferred_provider.empty() ? find_by_kind(kind) : get(preferred_provider);
  if (!provider) {
    if (preferred_provider.empty()) {
      throw ConfigurationError("No media generation provider supports " + to_string(kind) + ".");
    }
    throw ConfigurationError("Unknown media generation provider: " + preferred_provider);
  }
  if (!provider->supports(kind)) {
    throw ConfigurationError("Media generation provider \"" + provider->metadata().name +
                             "\" does not support " + to_string(kind) + ".");
  }
  return provider;
}

std::vector<std::shared_ptr<MediaGenerationProvider>> MediaGenerationProviderRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<MediaGenerationProvider>> providers;
  providers.reserve(providers_.size());
  for (const auto& [_, provider] : providers_) {
    providers.push_back(provider);
  }
  return providers;
}

std::shared_ptr<MediaGenerationProvider> resolve_media_generation_provider(
    const MediaGenerationProviderRegistry* registry,
    const std::string& preferred,
    MediaGenerationKind kind) {
  if (!registry) {
    return nullptr;
  }
  if (!preferred.empty()) {
    auto provider = registry->get(preferred);
    return provider && provider->supports(kind) ? provider : nullptr;
  }
  return registry->find_by_kind(kind);
}

std::shared_ptr<MediaGenerationProvider> require_media_generation_provider(
    const MediaGenerationProviderRegistry* registry,
    const std::string& preferred,
    MediaGenerationKind kind) {
  auto provider = resolve_media_generation_provider(registry, preferred, kind);
  if (!provider) {
    std::string message = "Media generation provider is not configured for " + to_string(kind) + ".";
    if (!preferred.empty()) {
      message = "Media generation provider \"" + preferred + "\" is not configured for " +
                to_string(kind) + ".";
    }
    throw ConfigurationError(message);
  }
  return provider;
}

DeterministicMediaGenerationProvider::DeterministicMediaGenerationProvider(
    std::string name,
    std::string model,
    std::vector<MediaGenerationKind> supported_kinds)
    : metadata_(MediaGenerationProviderMetadata{
          .name = std::move(name),
          .tier = "test",
          .title = "Deterministic Media Generation",
      }),
      model_(std::move(model)),
      supported_kinds_(std::move(supported_kinds)) {}

const MediaGenerationProviderMetadata& DeterministicMediaGenerationProvider::metadata() const {
  return metadata_;
}

MediaGenerationCapabilities DeterministicMediaGenerationProvider::capabilities() const {
  std::vector<std::string> mime_types;
  for (const auto kind : supported_kinds_) {
    mime_types.push_back(default_mime_type(kind));
  }
  return MediaGenerationCapabilities{
      .kinds = supported_kinds_,
      .models = {model_},
      .mime_types = std::move(mime_types),
      .features = {"seed", "progress", "artifact-output"},
      .max_count = 16,
      .supports_references = true,
      .supports_seed = true,
      .supports_negative_prompt = true,
      .supports_streaming = false,
  };
}

AgentOutput DeterministicMediaGenerationProvider::generate(
    const MediaGenerationRequest& request,
    const MediaGenerationContext& context) {
  if (context.cancellation) {
    context.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
  }
  if (context.progress) {
    context.progress(MediaGenerationProgress{
        .stage = "started",
        .message = "Deterministic media generation started.",
        .fraction = 0.0,
        .provider = metadata_.name,
        .kind = request.kind,
    });
  }

  const auto count = std::min<std::size_t>(std::max<std::size_t>(1, request.count), 16);
  std::vector<AgentArtifact> artifacts;
  artifacts.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto mime_type = default_mime_type(request.kind, request.format);
    const auto id = generate_uuid();
    const std::string payload = "{\"provider\":\"" + metadata_.name + "\",\"kind\":\"" +
                                to_string(request.kind) + "\",\"prompt\":\"" +
                                request.prompt + "\",\"index\":" + std::to_string(index) + "}";
    artifacts.push_back(AgentArtifact{
        .id = id,
        .kind = to_string(request.kind),
        .mime_type = mime_type,
        .filename = default_filename(request.kind, index, mime_type),
        .bytes = text_to_bytes(payload),
        .metadata = Value::object({{"deterministic", true}, {"index", static_cast<long long>(index)}}),
    });
  }

  if (context.progress) {
    context.progress(MediaGenerationProgress{
        .stage = "completed",
        .message = "Deterministic media generation completed.",
        .fraction = 1.0,
        .provider = metadata_.name,
        .kind = request.kind,
    });
  }

  return AgentOutput{
      .id = generate_uuid(),
      .provider = metadata_.name,
      .model = request.model.empty() ? model_ : request.model,
      .finish_reason = "stop",
      .artifacts = std::move(artifacts),
      .metadata = Value::object({{"kind", to_string(request.kind)}, {"deterministic", true}}),
  };
}

MediaArtifactWriter create_in_memory_media_artifact_writer(InMemoryArtifactStore* store) {
  return [store](const AgentArtifact& asset,
                 const AgentOutput& result,
                 const MediaGenerationOutputValueOptions& options) {
    return store_media_artifact(store, asset, result, options);
  };
}

MediaArtifactWriter create_file_media_artifact_writer(FileArtifactStore* store) {
  return [store](const AgentArtifact& asset,
                 const AgentOutput& result,
                 const MediaGenerationOutputValueOptions& options) {
    return store_file_media_artifact(store, asset, result, options);
  };
}

Value media_generation_capabilities_to_value(const MediaGenerationCapabilities& capabilities) {
  return Value::object({
      {"kinds", kind_vector_value(capabilities.kinds)},
      {"models", string_vector_value(capabilities.models)},
      {"mimeTypes", string_vector_value(capabilities.mime_types)},
      {"features", string_vector_value(capabilities.features)},
      {"maxCount", static_cast<long long>(capabilities.max_count)},
      {"supportsReferences", capabilities.supports_references},
      {"supportsSeed", capabilities.supports_seed},
      {"supportsNegativePrompt", capabilities.supports_negative_prompt},
      {"supportsStreaming", capabilities.supports_streaming},
      {"metadata", capabilities.metadata},
  });
}

Value media_generation_artifact_to_value(const AgentArtifact& asset) {
  const auto bytes = asset_bytes(asset);
  return Value::object({
      {"type", "generated-media"},
      {"kind", asset.kind},
      {"id", asset.id},
      {"mimeType", asset.mime_type},
      {"filename", asset.filename},
      {"data", base64_encode(bytes)},
      {"base64", true},
      {"byteLength", static_cast<long long>(bytes.size())},
      {"providerAssetId", asset.provider_asset_id},
      {"metadata", asset.metadata},
  });
}

MediaSource media_generation_artifact_to_media_source(const AgentArtifact& asset,
                                                   bool allow_inline_data) {
  if (asset.source.kind == MediaSourceKind::Artifact && !asset.source.key.empty()) {
    MediaSource source;
    source.kind = MediaSourceKind::Artifact;
    source.key = asset.source.key;
    source.mime_type = asset.mime_type;
    source.filename = asset.filename;
    return source;
  }
  if (has_media_source_payload(asset.source)) {
    MediaSource source = asset.source;
    if (source.mime_type.empty()) source.mime_type = asset.mime_type;
    if (source.filename.empty()) source.filename = asset.filename;
    if (!allow_inline_data && source.kind == MediaSourceKind::Inline) {
      source.data.clear();
    }
    return source;
  }
  const auto bytes = asset_bytes(asset);
  if (!bytes.empty()) {
    MediaSource source;
    source.kind = MediaSourceKind::Inline;
    source.mime_type = asset.mime_type;
    source.filename = asset.filename;
    if (allow_inline_data) {
      source.data = base64_encode(bytes);
    }
    return source;
  }
  MediaSource source;
  source.kind = MediaSourceKind::Inline;
  source.mime_type = asset.mime_type;
  source.filename = asset.filename;
  return source;
}

AgentOutput materialize_media_generation_output(
    AgentOutput result,
    const MediaGenerationOutputValueOptions& options) {
  if (!options.artifact_writer) {
    return result;
  }
  for (auto& asset : result.artifacts) {
    if (!asset.bytes.empty() || (asset.source.kind == MediaSourceKind::Inline && !asset.source.data.empty())) {
      asset = options.artifact_writer(asset, result, options);
    }
  }
  return result;
}

Value media_generation_output_to_value(
    const AgentOutput& result,
    const MediaGenerationOutputValueOptions& options) {
  auto materialized = materialize_media_generation_output(result, options);
  if (materialized.content.empty()) {
    materialized.content = media_generation_output_to_content(materialized, options.include_inline_data);
  }
  return agent_output_to_value(materialized, options.include_inline_data);
}

ToolDefinition create_media_generation_tool(MediaGenerationKind kind,
                                            MediaGenerationProviderRegistry* registry,
                                            MediaGenerationToolOptions options) {
  const auto make_name = [](MediaGenerationKind tool_kind) {
    if (tool_kind == MediaGenerationKind::Image) return std::string("media.generateImage");
    if (tool_kind == MediaGenerationKind::Video) return std::string("media.generateVideo");
    return std::string("media.generateAudio");
  };
  const std::string kind_name = to_string(kind);
  return define_tool(ToolDefinition{
      .name = make_name(kind),
      .description = "Generate one or more " + kind_name +
                     " assets through a registered media generation provider.",
      .input_schema = media_generation_tool_schema(kind),
      .capabilities = {"media.generate", "media.generate." + kind_name},
      .service_requirements = {tool_service_requirement(kToolServiceMediaGenerationRegistry, true),
                               tool_service_requirement(kToolServiceDefaultMediaGenerationProvider, true)},
      .risk_level = ToolRiskLevel::Medium,
      .tags = {"media", "generation", kind_name},
      .bundle = "media",
      .builtin = true,
      .long_running = true,
      .side_effect_level = "external",
      .execute = [registry, options, kind](const Value& input,
                                           ToolExecutionContext& context) -> ToolInvokeResult {
        MediaGenerationProviderRegistry* effective_registry =
            registry ? registry : get_tool_service(
                context, kToolServiceMediaGenerationRegistry);
        auto* configured_default_provider = get_tool_service(
            context, kToolServiceDefaultMediaGenerationProvider);
        const std::string requested_provider = input.at("provider").as_string();
        const std::string default_provider = !requested_provider.empty()
                                                 ? requested_provider
                                                 : (!options.default_provider.empty()
                                                        ? options.default_provider
                                                        : (configured_default_provider
                                                               ? *configured_default_provider
                                                               : std::string{}));
        auto provider = require_media_generation_provider(effective_registry, default_provider, kind);
        auto request = media_generation_request_from_tool_input(kind, input);
        auto issues = provider->validate(request);
        if (!issues.empty()) {
          throw SchemaValidationError("Invalid media generation request.", std::move(issues));
        }
        if (context.cancellation) {
          context.cancellation->throw_if_cancelled(ExecutionTarget::Tool);
        }

        MediaGenerationContext generation_context;
        generation_context.cancellation = context.cancellation;
        generation_context.metadata = Value::object({
            {"toolName", context.tool_call ? context.tool_call->name : std::string{}},
            {"toolCallId", context.tool_call ? context.tool_call->id : std::string{}},
        });
        generation_context.progress = [&context](const MediaGenerationProgress& progress) {
          emit_tool_progress(context, "media.generation.progress", media_generation_progress_value(progress));
        };

        auto result = provider->generate(request, generation_context);
        normalize_media_generation_output(result, *provider, request);
        auto materialized = materialize_media_generation_output(result, options.output_options);
        const auto value = media_generation_output_to_value(
            materialized,
            MediaGenerationOutputValueOptions{
                .include_inline_data = options.output_options.include_inline_data,
            });
        auto content = resolvable_media_generation_content(
            materialized,
            options.output_options.include_inline_data);

        ToolResultEnvelope envelope;
        if (!content.empty()) {
          envelope.content = std::move(content);
        }
        envelope.value = value;
        envelope.metadata = Value::object({
            {"provider", materialized.provider},
            {"model", materialized.model},
            {"kind", to_string(kind)},
            {"artifactCount", static_cast<long long>(materialized.artifacts.size())},
        });
        return envelope;
      },
  });
}

std::vector<ToolDefinition> create_media_generation_tools(
    MediaGenerationProviderRegistry* registry,
    MediaGenerationToolOptions options) {
  return {
      create_media_generation_tool(MediaGenerationKind::Image, registry, options),
      create_media_generation_tool(MediaGenerationKind::Video, registry, options),
      create_media_generation_tool(MediaGenerationKind::Audio, registry, options),
  };
}

std::vector<MessageContentPart> media_generation_output_to_content(
    const AgentOutput& result,
    bool allow_inline_data) {
  std::vector<MessageContentPart> content;
  content.reserve(result.artifacts.size());
  for (const auto& asset : result.artifacts) {
    auto source = media_generation_artifact_to_media_source(asset, allow_inline_data);
    if (source.kind == MediaSourceKind::Inline && source.data.empty()) {
      throw ConfigurationError("Generated media content requires artifact, URL, path, or inline data.");
    }
    const auto kind = artifact_media_generation_kind(asset);
    if (kind == MediaGenerationKind::Image) {
      content.push_back(image_part(source, asset.filename, {}, asset.metadata));
    } else if (kind == MediaGenerationKind::Audio) {
      content.push_back(audio_part(source, asset.filename, {}, asset.metadata));
    } else if (kind == MediaGenerationKind::Video) {
      content.push_back(video_part(source, asset.filename, {}, {}, std::nullopt, std::nullopt,
                                   std::nullopt, asset.metadata));
    } else {
      content.push_back(file_part(source, asset.filename, asset.kind, asset.metadata));
    }
  }
  return content;
}

}  // namespace agent
