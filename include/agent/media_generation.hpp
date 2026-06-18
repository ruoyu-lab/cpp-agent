#pragma once

#include "agent/orchestration.hpp"
#include "agent/tools.hpp"
#include "agent/tool_services_io.hpp"

#include <mutex>

namespace agent {

enum class MediaGenerationKind {
  Image,
  Video,
  Audio,
};

std::string to_string(MediaGenerationKind kind);
MediaGenerationKind media_generation_kind_from_string(const std::string& value);

struct MediaGenerationCapabilities {
  std::vector<MediaGenerationKind> kinds;
  std::vector<std::string> models;
  std::vector<std::string> mime_types;
  std::vector<std::string> features;
  std::size_t max_count = 1;
  bool supports_references = false;
  bool supports_seed = false;
  bool supports_negative_prompt = false;
  bool supports_streaming = false;
  Value metadata = Value::object({});
};

struct MediaGenerationProviderMetadata {
  std::string name;
  std::string tier = "portable";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
  Value metadata = Value::object({});
};

struct MediaGenerationInputAsset {
  MediaGenerationKind kind = MediaGenerationKind::Image;
  MediaSource source;
  std::string role;
  Value metadata = Value::object({});
};

struct MediaGenerationRequest {
  MediaGenerationKind kind = MediaGenerationKind::Image;
  std::string prompt;
  std::string negative_prompt;
  std::string model;
  std::string format;
  std::optional<int> width;
  std::optional<int> height;
  std::optional<double> duration_seconds;
  std::optional<int> sample_rate_hz;
  std::optional<int> seed;
  std::size_t count = 1;
  std::vector<MediaGenerationInputAsset> inputs;
  Value metadata = Value::object({});
};

struct MediaGenerationProgress {
  std::string stage;
  std::string message;
  std::optional<double> fraction;
  std::string provider;
  MediaGenerationKind kind = MediaGenerationKind::Image;
  std::string asset_id;
  Value metadata = Value::object({});
};

struct MediaGenerationContext {
  CancellationToken* cancellation = nullptr;
  std::function<void(const MediaGenerationProgress&)> progress;
  Value metadata = Value::object({});
};

class MediaGenerationProvider {
 public:
  virtual ~MediaGenerationProvider() = default;
  [[nodiscard]] virtual const MediaGenerationProviderMetadata& metadata() const = 0;
  [[nodiscard]] std::string name() const { return metadata().name; }
  [[nodiscard]] virtual MediaGenerationCapabilities capabilities() const = 0;
  [[nodiscard]] bool supports(MediaGenerationKind kind) const;
  [[nodiscard]] virtual std::vector<SchemaValidationIssue> validate(
      const MediaGenerationRequest& request) const;
  virtual AgentOutput generate(const MediaGenerationRequest& request,
                               const MediaGenerationContext& context = {}) = 0;
};

class MediaGenerationProviderRegistry {
 public:
  explicit MediaGenerationProviderRegistry(std::vector<std::shared_ptr<MediaGenerationProvider>> providers = {});
  MediaGenerationProviderRegistry(const MediaGenerationProviderRegistry& other);
  MediaGenerationProviderRegistry& operator=(const MediaGenerationProviderRegistry& other);
  MediaGenerationProviderRegistry(MediaGenerationProviderRegistry&& other) noexcept;
  MediaGenerationProviderRegistry& operator=(MediaGenerationProviderRegistry&& other) noexcept;
  std::shared_ptr<MediaGenerationProvider> register_provider(std::shared_ptr<MediaGenerationProvider> provider);
  [[nodiscard]] std::shared_ptr<MediaGenerationProvider> get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<MediaGenerationProvider> require(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<MediaGenerationProvider> find_by_kind(MediaGenerationKind kind) const;
  [[nodiscard]] std::shared_ptr<MediaGenerationProvider> require_by_kind(
      MediaGenerationKind kind,
      const std::string& preferred_provider = {}) const;
  [[nodiscard]] std::vector<std::shared_ptr<MediaGenerationProvider>> list() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<MediaGenerationProvider>> providers_;
};

std::shared_ptr<MediaGenerationProvider> resolve_media_generation_provider(
    const MediaGenerationProviderRegistry* registry,
    const std::string& preferred,
    MediaGenerationKind kind);
std::shared_ptr<MediaGenerationProvider> require_media_generation_provider(
    const MediaGenerationProviderRegistry* registry,
    const std::string& preferred,
    MediaGenerationKind kind);

class DeterministicMediaGenerationProvider final : public MediaGenerationProvider {
 public:
  DeterministicMediaGenerationProvider(std::string name = "deterministic",
                                       std::string model = "deterministic-media-v1",
                                       std::vector<MediaGenerationKind> supported_kinds = {
                                           MediaGenerationKind::Image,
                                           MediaGenerationKind::Video,
                                           MediaGenerationKind::Audio,
                                       });
  [[nodiscard]] const MediaGenerationProviderMetadata& metadata() const override;
  [[nodiscard]] MediaGenerationCapabilities capabilities() const override;
  AgentOutput generate(const MediaGenerationRequest& request,
                       const MediaGenerationContext& context = {}) override;

 private:
  MediaGenerationProviderMetadata metadata_;
  std::string model_;
  std::vector<MediaGenerationKind> supported_kinds_;
};

struct MediaGenerationOutputValueOptions;
using MediaArtifactWriter = std::function<AgentArtifact(
    const AgentArtifact& artifact,
    const AgentOutput& output,
    const MediaGenerationOutputValueOptions& options)>;

struct MediaGenerationOutputValueOptions {
  MediaArtifactWriter artifact_writer;
  std::string artifact_key_prefix = "media/generated";
  bool include_inline_data = false;
};

struct MediaGenerationToolOptions {
  std::string default_provider;
  MediaGenerationOutputValueOptions output_options;
};

ToolDefinition create_media_generation_tool(MediaGenerationKind kind,
                                            MediaGenerationProviderRegistry* registry = nullptr,
                                            MediaGenerationToolOptions options = {});
std::vector<ToolDefinition> create_media_generation_tools(
    MediaGenerationProviderRegistry* registry = nullptr,
    MediaGenerationToolOptions options = {});

MediaArtifactWriter create_in_memory_media_artifact_writer(InMemoryArtifactStore* store);
MediaArtifactWriter create_file_media_artifact_writer(FileArtifactStore* store);

Value media_generation_capabilities_to_value(const MediaGenerationCapabilities& capabilities);
Value media_generation_artifact_to_value(const AgentArtifact& artifact);
MediaSource media_generation_artifact_to_media_source(const AgentArtifact& artifact,
                                                      bool allow_inline_data = false);
AgentOutput materialize_media_generation_output(
    AgentOutput output,
    const MediaGenerationOutputValueOptions& options = {});
Value media_generation_output_to_value(
    const AgentOutput& output,
    const MediaGenerationOutputValueOptions& options = {});
std::vector<MessageContentPart> media_generation_output_to_content(
    const AgentOutput& output,
    bool allow_inline_data = false);

}  // namespace agent
