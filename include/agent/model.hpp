#pragma once

#include "agent/messages.hpp"
#include "agent/streaming.hpp"

#include <cstdint>
#include <mutex>

namespace agent {

class CancellationToken;

struct TaggedReasoningExtraction {
  std::string text;
  std::optional<ModelReasoning> reasoning;
};

enum class TaggedReasoningDeltaType {
  Text,
  Reasoning,
};

struct TaggedReasoningDelta {
  TaggedReasoningDeltaType type = TaggedReasoningDeltaType::Text;
  std::string delta;
  std::string text;
  std::string reasoning_text;
};

std::string normalize_finish_reason(std::string raw);
bool is_incomplete_finish_reason(const std::string& value);

enum class ReasoningSource {
  Provider = 0,   // provider directly reported the count
  Estimated = 1,  // we estimated from thinking-content chars / 4
  Unknown = 2,    // no data; model may not have reasoned, or provider doesn't expose it
};

enum class TokenUsageSource {
  Provider = 0,   // provider/runtime directly reported the count
  Derived = 1,    // deterministically computed from other token fields
  Estimated = 2,  // tokenizer/heuristic estimate
  Unknown = 3,    // no trustworthy count
};

enum class TokenUsageQuality {
  Provider = 0,   // input/output/total are provider-reported or exact derived values
  Mixed = 1,      // at least one field has weaker provenance than the others
  Estimated = 2,  // all known token fields are estimates
  Unknown = 3,    // no usable token counts
};

std::string to_string(ReasoningSource source);
ReasoningSource reasoning_source_from_string(std::string_view text,
                                             ReasoningSource fallback = ReasoningSource::Unknown);
ReasoningSource merge_reasoning_source(ReasoningSource a, ReasoningSource b);
std::string to_string(TokenUsageSource source);
TokenUsageSource token_usage_source_from_string(
    std::string_view text,
    TokenUsageSource fallback = TokenUsageSource::Unknown);
TokenUsageSource merge_token_usage_source(TokenUsageSource a, TokenUsageSource b);
std::string to_string(TokenUsageQuality quality);
TokenUsageQuality token_usage_quality_from_sources(
    const std::vector<TokenUsageSource>& sources);

struct ModelUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  int total_tokens = 0;
  TokenUsageSource input_tokens_source = TokenUsageSource::Unknown;
  TokenUsageSource output_tokens_source = TokenUsageSource::Unknown;
  TokenUsageSource total_tokens_source = TokenUsageSource::Unknown;
  TokenUsageQuality quality = TokenUsageQuality::Unknown;
  int cached_input_tokens = 0;  // tokens billed at cache-hit rate (provider-reported)
  TokenUsageSource cached_input_tokens_source = TokenUsageSource::Unknown;
  int reasoning_tokens = 0;
  ReasoningSource reasoning_source = ReasoningSource::Unknown;
  std::string provider;
};

struct UsagePricing {
  std::optional<double> input_per_1k_tokens;
  std::optional<double> output_per_1k_tokens;
  std::string currency = "USD";
};

struct UsageCostEstimate {
  std::string provider;
  std::string model;
  std::string currency = "USD";
  double input_cost = 0;
  double output_cost = 0;
  double total_cost = 0;
};

ModelUsage extract_model_usage(const Value& response_or_raw, std::string provider = {});
ModelUsage extract_model_usage(const AgentOutput& response);
UsageCostEstimate estimate_usage_cost(const ModelUsage& usage, const UsagePricing& pricing,
                                      std::string provider, std::string model);
ModelUsage merge_model_usage(const std::vector<ModelUsage>& usages);

struct ReasoningSettings {
  std::optional<bool> enabled;
  std::variant<std::monostate, std::string, double> budget;
  std::optional<bool> include_thoughts;
  std::string tag_name;
};

struct ReasoningBudgetTokenTable {
  int low = 1024;
  int medium = 4096;
  int high = 16384;
};

enum class CacheStrategy {
  None,       // default — rely on provider auto-cache only
  Explicit,   // inject cache_control / prompt_cache_key markers
};

enum class CacheScope {
  SystemOnly,             // mark only system prompt
  SystemAndTools,         // mark system + tool definitions (default when Explicit)
  SystemToolsAndSkills,   // mark system + tools + skills preamble
};

std::string to_string(CacheStrategy strategy);
std::string to_string(CacheScope scope);
CacheStrategy cache_strategy_from_string(const std::string& value);
CacheScope cache_scope_from_string(const std::string& value);

struct ModelSettings {
  std::string model;
  std::optional<double> temperature;
  std::optional<int> max_output_tokens;
  std::optional<ReasoningSettings> reasoning;
  CacheStrategy cache_strategy = CacheStrategy::None;
  CacheScope cache_scope = CacheScope::SystemAndTools;
  std::string cache_key;  // optional override; if empty, derive from system+tools fingerprint
  Value extra = Value::object({});
};

Value reasoning_settings_to_json_value(const ReasoningSettings& settings);
void assert_reasoning_settings(const Value& value, const std::string& path = "reasoning");
ReasoningSettings reasoning_settings_from_json_value(const Value& value);
std::optional<ReasoningSettings> merge_reasoning_settings(
    const std::optional<ReasoningSettings>& base,
    const std::optional<ReasoningSettings>& override_settings);
std::string normalize_reasoning_budget_to_effort(
    const std::variant<std::monostate, std::string, double>& budget);
int normalize_reasoning_budget_to_tokens(
    const std::variant<std::monostate, std::string, double>& budget,
    ReasoningBudgetTokenTable table = {});
std::optional<ModelReasoning> build_model_reasoning(std::string text,
                                                     std::string format = "provider-visible");
std::optional<ModelReasoning> merge_model_reasoning(std::optional<ModelReasoning> primary,
                                                    std::optional<ModelReasoning> fallback);
TaggedReasoningExtraction extract_tagged_reasoning(const std::string& input,
                                                   const std::string& tag_name = "think");
class TaggedReasoningStreamParser {
 public:
  explicit TaggedReasoningStreamParser(std::string tag_name = "think");
  std::vector<TaggedReasoningDelta> feed(const std::string& chunk);
  std::vector<TaggedReasoningDelta> finish();
  [[nodiscard]] std::optional<ModelReasoning> to_reasoning() const;
  [[nodiscard]] const std::string& text() const noexcept;
  [[nodiscard]] const std::string& reasoning_text() const noexcept;

 private:
  enum class Mode {
    Text,
    Reasoning,
  };

  std::vector<TaggedReasoningDelta> push_delta(TaggedReasoningDeltaType type, std::string delta);

  std::string open_tag_;
  std::string close_tag_;
  Mode mode_ = Mode::Text;
  std::string buffer_;
  std::string text_;
  std::string reasoning_text_;
  std::string pending_text_utf8_;
  std::string pending_reasoning_utf8_;
};
Value model_settings_to_json_value(const ModelSettings& settings);
ModelSettings model_settings_from_json_value(const Value& value);

struct GenerateParams {
  std::vector<AgentMessage> messages;
  std::vector<ChatToolDescriptor> tools;
  ModelSettings settings;
  CancellationToken* cancellation = nullptr;
};

using EmbeddingVector = std::vector<double>;

enum class ModelStreamEventType {
  ResponseStart,
  TextDelta,
  ReasoningDelta,
  ContentPart,
  Response,
  ToolCallDelta,
};

std::string to_string(ModelStreamEventType type);

enum class ModelStreamFrameBoundary {
  None,
  SseEvent,
  JsonLine,
  NativeCallback,
};

std::string to_string(ModelStreamFrameBoundary boundary);

struct ModelProviderStreamContract {
  int schema_version = kAgentStreamEventSchemaVersion;
  std::string provider;
  std::string model;
  ModelStreamFrameBoundary frame_boundary = ModelStreamFrameBoundary::None;
  bool text_delta = true;
  bool reasoning_delta = false;
  bool tool_call_delta = false;
  bool content_part = false;
};

Value model_provider_stream_contract_to_value(const ModelProviderStreamContract& contract);

struct ModelStreamEvent {
  int schema_version = kAgentStreamEventSchemaVersion;
  std::uint64_t sequence = 0;
  ModelStreamEventType type = ModelStreamEventType::ResponseStart;
  std::string provider;
  std::string model;
  std::string delta;
  std::string text;
  std::string reasoning;
  MessageContentPart part;
  AgentOutput response;
  // Populated only when type == ToolCallDelta.
  std::string tool_call_id;
  std::string tool_call_name;
  std::string tool_call_args_delta;
  std::string tool_call_args_accumulated;
};

class ChatModelAdapter {
 public:
  ChatModelAdapter(std::string provider, std::string model, double temperature = 0.2,
                   int max_output_tokens = 1024, std::set<std::string> capabilities = {"input.text"},
                   std::optional<ReasoningSettings> reasoning = std::nullopt);
  virtual ~ChatModelAdapter() = default;

  [[nodiscard]] const std::string& provider() const noexcept;
  [[nodiscard]] const std::string& model() const noexcept;
  [[nodiscard]] const std::set<std::string>& capabilities() const noexcept;
  [[nodiscard]] bool supports(const std::string& capability) const;
  [[nodiscard]] virtual ModelProviderStreamContract stream_contract() const;
  [[nodiscard]] ModelSettings resolve_settings(const ModelSettings& settings = {}) const;
  [[nodiscard]] AgentOutput build_output(AgentOutput payload = {}) const;
  void set_capabilities(std::set<std::string> capabilities);

  virtual AgentOutput generate(const GenerateParams& params) = 0;
  using StreamEventHandler = std::function<void(const ModelStreamEvent&)>;
  virtual void stream(const GenerateParams& params, StreamEventHandler on_event);

 private:
  std::string provider_;
  std::string model_;
  double temperature_;
  int max_output_tokens_;
  std::set<std::string> capabilities_;
  std::optional<ReasoningSettings> default_reasoning_;
};

class EchoChatModelAdapter : public ChatModelAdapter {
 public:
  EchoChatModelAdapter();
  AgentOutput generate(const GenerateParams& params) override;
};

class FallbackChatModelAdapter : public ChatModelAdapter {
 public:
  explicit FallbackChatModelAdapter(std::vector<std::shared_ptr<ChatModelAdapter>> adapters);

  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] const std::vector<std::shared_ptr<ChatModelAdapter>>& adapters() const noexcept;

 private:
  [[nodiscard]] GenerateParams params_for_adapter(const ChatModelAdapter& adapter,
                                                  const GenerateParams& params) const;

  std::vector<std::shared_ptr<ChatModelAdapter>> adapters_;
};

using ChatAdapterFactory = std::function<std::shared_ptr<ChatModelAdapter>(const Value& config)>;

class ChatProviderRegistry {
 public:
  ChatProviderRegistry() = default;
  ChatProviderRegistry(const ChatProviderRegistry& other);
  ChatProviderRegistry& operator=(const ChatProviderRegistry& other);
  ChatProviderRegistry(ChatProviderRegistry&& other) noexcept;
  ChatProviderRegistry& operator=(ChatProviderRegistry&& other) noexcept;
  ChatProviderRegistry& register_provider(std::string provider, ChatAdapterFactory factory);
  [[nodiscard]] bool has(const std::string& provider) const;
  std::shared_ptr<ChatModelAdapter> create(const std::string& provider, const Value& config = Value::object({})) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ChatAdapterFactory> factories_;
};

struct EmbeddingSettings {
  std::string model;
  std::optional<int> dimensions;
  std::string task_type;
  std::string space_id;
  Value extra = Value::object({});
};

struct EmbeddingSpaceDescriptor {
  std::string id;
  std::vector<std::string> modalities;
  std::optional<int> dimensions;
};

class TextEmbeddingAdapter {
 public:
  TextEmbeddingAdapter(std::string provider, std::string model, int dimensions, std::string space_id);
  virtual ~TextEmbeddingAdapter() = default;

  [[nodiscard]] const std::string& provider() const noexcept;
  [[nodiscard]] const std::string& model() const noexcept;
  [[nodiscard]] int dimensions() const noexcept;
  [[nodiscard]] const std::string& space_id() const noexcept;
  [[nodiscard]] EmbeddingSettings resolve_settings(const EmbeddingSettings& settings = {}) const;
  [[nodiscard]] EmbeddingSpaceDescriptor space() const;

  virtual std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts,
                                             const Value& settings = {},
                                             CancellationToken* cancellation = nullptr) = 0;
  EmbeddingVector embed_one(const std::string& text, const Value& settings = {},
                            CancellationToken* cancellation = nullptr);

 private:
  std::string provider_;
  std::string model_;
  int dimensions_;
  std::string space_id_;
};

class HashEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  explicit HashEmbeddingAdapter(int dimensions = 256, std::string model = "hash-embedding",
                                std::string space_id = "hash-shared-v1");
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts, const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;
};

using ClipTextEmbeddingBatch = std::function<std::vector<EmbeddingVector>(
    const std::vector<std::string>& texts,
    const EmbeddingSettings& settings)>;

class ClipTextEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  explicit ClipTextEmbeddingAdapter(ClipTextEmbeddingBatch embed_batch);
  explicit ClipTextEmbeddingAdapter(std::string model = "Xenova/clip-vit-base-patch32",
                                    int dimensions = 0,
                                    std::string space_id = "clip-shared-v1",
                                    ClipTextEmbeddingBatch embed_batch = {});
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts, const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

 private:
  ClipTextEmbeddingBatch embed_batch_;
};

struct ImageEmbeddingInput {
  MediaSource source;
  std::string alt_text;
  std::string title;
  std::string text_hint;
  Value metadata = Value::object({});
};

class ImageEmbeddingAdapter {
 public:
  ImageEmbeddingAdapter(std::string provider, std::string model, int dimensions, std::string space_id);
  virtual ~ImageEmbeddingAdapter() = default;

  [[nodiscard]] const std::string& provider() const noexcept;
  [[nodiscard]] const std::string& model() const noexcept;
  [[nodiscard]] int dimensions() const noexcept;
  [[nodiscard]] const std::string& space_id() const noexcept;
  [[nodiscard]] EmbeddingSettings resolve_settings(const EmbeddingSettings& settings = {}) const;
  [[nodiscard]] EmbeddingSpaceDescriptor space() const;

  virtual std::vector<EmbeddingVector> embed(const std::vector<ImageEmbeddingInput>& images,
                                             const Value& settings = {},
                                             CancellationToken* cancellation = nullptr) = 0;
  EmbeddingVector embed_one(const ImageEmbeddingInput& image, const Value& settings = {},
                            CancellationToken* cancellation = nullptr);

 private:
  std::string provider_;
  std::string model_;
  int dimensions_;
  std::string space_id_;
};

class HashImageEmbeddingAdapter : public ImageEmbeddingAdapter {
 public:
  explicit HashImageEmbeddingAdapter(int dimensions = 256, std::string model = "hash-image-embedding",
                                     std::string space_id = "hash-shared-v1");
  std::vector<EmbeddingVector> embed(const std::vector<ImageEmbeddingInput>& images,
                                     const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;
};

using ClipImageEmbeddingBatch = std::function<std::vector<EmbeddingVector>(
    const std::vector<ImageEmbeddingInput>& images,
    const EmbeddingSettings& settings)>;

class ClipImageEmbeddingAdapter : public ImageEmbeddingAdapter {
 public:
  explicit ClipImageEmbeddingAdapter(ClipImageEmbeddingBatch embed_batch);
  explicit ClipImageEmbeddingAdapter(std::string model = "Xenova/clip-vit-base-patch32",
                                     int dimensions = 0,
                                     std::string space_id = "clip-shared-v1",
                                     ClipImageEmbeddingBatch embed_batch = {});
  std::vector<EmbeddingVector> embed(const std::vector<ImageEmbeddingInput>& images,
                                     const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

 private:
  ClipImageEmbeddingBatch embed_batch_;
};

using EmbeddingAdapterFactory = std::function<std::shared_ptr<TextEmbeddingAdapter>(const Value& config)>;
using ImageEmbeddingAdapterFactory = std::function<std::shared_ptr<ImageEmbeddingAdapter>(const Value& config)>;

class EmbeddingProviderRegistry {
 public:
  EmbeddingProviderRegistry() = default;
  EmbeddingProviderRegistry(const EmbeddingProviderRegistry& other);
  EmbeddingProviderRegistry& operator=(const EmbeddingProviderRegistry& other);
  EmbeddingProviderRegistry(EmbeddingProviderRegistry&& other) noexcept;
  EmbeddingProviderRegistry& operator=(EmbeddingProviderRegistry&& other) noexcept;
  EmbeddingProviderRegistry& register_provider(std::string provider, EmbeddingAdapterFactory factory);
  [[nodiscard]] bool has(const std::string& provider) const;
  std::shared_ptr<TextEmbeddingAdapter> create(const std::string& provider,
                                               const Value& config = Value::object({})) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, EmbeddingAdapterFactory> factories_;
};

class ImageEmbeddingProviderRegistry {
 public:
  ImageEmbeddingProviderRegistry() = default;
  ImageEmbeddingProviderRegistry(const ImageEmbeddingProviderRegistry& other);
  ImageEmbeddingProviderRegistry& operator=(const ImageEmbeddingProviderRegistry& other);
  ImageEmbeddingProviderRegistry(ImageEmbeddingProviderRegistry&& other) noexcept;
  ImageEmbeddingProviderRegistry& operator=(ImageEmbeddingProviderRegistry&& other) noexcept;
  ImageEmbeddingProviderRegistry& register_provider(std::string provider, ImageEmbeddingAdapterFactory factory);
  [[nodiscard]] bool has(const std::string& provider) const;
  std::shared_ptr<ImageEmbeddingAdapter> create(const std::string& provider,
                                                const Value& config = Value::object({})) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ImageEmbeddingAdapterFactory> factories_;
};

}  // namespace agent
