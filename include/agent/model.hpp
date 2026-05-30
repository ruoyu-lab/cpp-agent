#pragma once

#include "agent/http.hpp"
#include "agent/messages.hpp"

#include <mutex>

namespace agent {

class CancellationToken;

struct ModelReasoning {
  std::string text;
  std::string format = "summary";
};

struct ModelResponse {
  std::string id;
  std::string provider;
  std::string model;
  std::vector<MessageContentPart> content;
  std::string text;
  std::optional<ModelReasoning> reasoning;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason = "stop";
  Value raw;
};

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

AgentMessage assistant_message_from_response(const ModelResponse& response);
std::string normalize_finish_reason(std::string raw);
bool is_incomplete_finish_reason(const std::string& value);

enum class ReasoningSource {
  Provider = 0,   // provider directly reported the count
  Estimated = 1,  // we estimated from thinking-content chars / 4
  Unknown = 2,    // no data; model may not have reasoned, or provider doesn't expose it
};

std::string to_string(ReasoningSource source);
ReasoningSource reasoning_source_from_string(std::string_view text,
                                             ReasoningSource fallback = ReasoningSource::Unknown);
ReasoningSource merge_reasoning_source(ReasoningSource a, ReasoningSource b);

struct ModelUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  int total_tokens = 0;
  int cached_input_tokens = 0;  // tokens billed at cache-hit rate (provider-reported)
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
ModelUsage extract_model_usage(const ModelResponse& response);
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

struct ModelStreamEvent {
  ModelStreamEventType type = ModelStreamEventType::ResponseStart;
  std::string provider;
  std::string model;
  std::string delta;
  std::string text;
  std::string reasoning;
  MessageContentPart part;
  ModelResponse response;
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
  [[nodiscard]] ModelSettings resolve_settings(const ModelSettings& settings = {}) const;
  [[nodiscard]] ModelResponse build_response(ModelResponse payload = {}) const;
  void set_capabilities(std::set<std::string> capabilities);

  virtual ModelResponse generate(const GenerateParams& params) = 0;
  virtual std::vector<ModelStreamEvent> stream(const GenerateParams& params);
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
  ModelResponse generate(const GenerateParams& params) override;
};

class FallbackChatModelAdapter : public ChatModelAdapter {
 public:
  explicit FallbackChatModelAdapter(std::vector<std::shared_ptr<ChatModelAdapter>> adapters);

  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] const std::vector<std::shared_ptr<ChatModelAdapter>>& adapters() const noexcept;

 private:
  [[nodiscard]] GenerateParams params_for_adapter(const ChatModelAdapter& adapter,
                                                  const GenerateParams& params) const;

  std::vector<std::shared_ptr<ChatModelAdapter>> adapters_;
};

struct NativeProviderRequest {
  std::string provider;
  std::string endpoint;
  Value body = Value::object({});
  std::map<std::string, std::string> headers;
  std::string base_url;
  CancellationToken* cancellation = nullptr;
};

using NativeProviderTransport = std::function<Value(const NativeProviderRequest&)>;
using NativeProviderStreamTransport = std::function<std::vector<std::string>(const NativeProviderRequest&)>;
using NativeProviderStreamingChunkHandler = std::function<void(std::string_view chunk)>;
using NativeProviderStreamingTransport =
    std::function<void(const NativeProviderRequest&,
                       NativeProviderStreamingChunkHandler on_chunk)>;

using LlamaCppNativePooling = std::string;
using LlamaCppNativeToolMode = std::string;

struct LlamaCppNativeLoraAdapter {
  std::string path;
  double scale = 1.0;
};

struct LlamaCppNativeRuntimeConfig {
  std::string model_path;
  std::string library_path;
  std::string library_dir;
  std::optional<int> context_size;
  std::optional<int> batch_size;
  std::optional<int> threads;
  std::optional<int> batch_threads;
  std::optional<int> gpu_layers;
  std::optional<bool> mmap;
  std::optional<bool> mlock;
  std::vector<LlamaCppNativeLoraAdapter> lora_adapters;
  std::string mmproj_path;
  std::string mtmd_library_path;
  std::string mtmd_library_dir;
  std::optional<bool> mmproj_use_gpu;
  std::optional<int> mmproj_threads;
  std::optional<int> mmproj_image_min_tokens;
  std::optional<int> mmproj_image_max_tokens;
  std::string media_marker = "<__media__>";
};

struct LlamaCppNativeSamplingConfig {
  std::optional<double> temperature;
  std::optional<int> max_output_tokens;
  std::optional<int> top_k;
  std::optional<double> top_p;
  std::optional<double> min_p;
  std::optional<double> repeat_penalty;
  std::optional<int> seed;
};

struct LlamaCppNativeChatMessage {
  std::string role;
  std::string content;
};

struct LlamaCppNativeChatMedia {
  std::vector<std::uint8_t> bytes;
  std::string mime_type;
  std::string id;
  std::string filename;
};

struct LlamaCppNativeChatRequest : LlamaCppNativeSamplingConfig {
  std::string request_id;
  std::string model;
  std::vector<LlamaCppNativeChatMessage> messages;
  std::vector<LlamaCppNativeChatMedia> media;
  std::vector<ChatToolDescriptor> tools;
  std::string chat_template;
  bool strict_template = false;
  std::string grammar;
  std::string grammar_root;
  std::string session_id;
  std::optional<JsonSchema> output_schema;
  LlamaCppNativeToolMode tool_mode = "auto";
  std::string reasoning_tag_name = "think";
};

struct LlamaCppNativeChatDelta {
  std::string type = "text";
  std::string delta;
};

struct LlamaCppNativeChatResult {
  std::string id;
  std::string model;
  std::string text;
  std::optional<ModelReasoning> reasoning;
  std::string finish_reason = "stop";
  Value raw;
};

struct LlamaCppNativeEmbeddingRequest {
  std::string request_id;
  std::string model;
  std::vector<std::string> texts;
  LlamaCppNativePooling pooling = "mean";
  bool normalize = true;
  std::optional<int> dimensions;
};

struct LlamaCppNativeEmbeddingResult {
  std::string model;
  std::vector<EmbeddingVector> embeddings;
  std::optional<int> dimensions;
};

struct LlamaCppNativeBinding {
  using ChatDeltaHandler = std::function<void(const LlamaCppNativeChatDelta&)>;
  std::function<LlamaCppNativeChatResult(const LlamaCppNativeRuntimeConfig&,
                                         const LlamaCppNativeChatRequest&,
                                         ChatDeltaHandler)> generate_chat;
  std::function<LlamaCppNativeEmbeddingResult(const LlamaCppNativeRuntimeConfig&,
                                              const LlamaCppNativeEmbeddingRequest&)> embed_texts;
  std::function<void(const std::string& request_id)> cancel;
};

std::shared_ptr<LlamaCppNativeBinding> create_llama_cpp_native_binding();

struct LlamaCppNativeChatModelAdapterConfig : LlamaCppNativeRuntimeConfig, LlamaCppNativeSamplingConfig {
  std::string model;
  std::string chat_template;
  bool strict_template = false;
  LlamaCppNativeToolMode tool_mode = "auto";
  std::string grammar;
  std::string grammar_root;
  std::string session_id;
  std::optional<ReasoningSettings> reasoning;
  std::shared_ptr<LlamaCppNativeBinding> binding;
};

struct LlamaCppNativeEmbeddingAdapterConfig : LlamaCppNativeRuntimeConfig {
  std::string model;
  LlamaCppNativePooling pooling = "mean";
  bool normalize = true;
  std::optional<int> dimensions;
  std::string space_id;
  std::shared_ptr<LlamaCppNativeBinding> binding;
};

NativeProviderTransport create_native_provider_http_transport(
    HttpTransport transport = create_native_http_transport());
NativeProviderStreamTransport create_native_provider_http_stream_transport(
    HttpTransport transport = create_native_http_transport());
NativeProviderStreamingTransport create_native_provider_http_streaming_transport(
    HttpStreamingTransport transport = create_native_http_streaming_transport());
Value serialize_chat_tool_descriptor(const ChatToolDescriptor& tool);
Value serialize_openai_chat_messages(const std::vector<AgentMessage>& messages);
NativeProviderRequest build_openai_chat_request(const GenerateParams& params, std::string model,
                                                std::string endpoint = "/v1/chat/completions",
                                                std::string base_url = {});
// Applies `reasoning_effort` (low / medium / high) to a Chat Completions
// request body when reasoning is enabled. Intended for OpenAI's o-series /
// gpt-5 chat-completions calls; do NOT call this on the shared
// `build_openai_chat_request` output for non-OpenAI providers (Qwen / Ollama /
// vLLM / MiMo / etc) — they use their own reasoning conventions.
void apply_openai_reasoning_effort(NativeProviderRequest& request,
                                   const std::optional<ReasoningSettings>& reasoning);
NativeProviderRequest build_openai_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string endpoint = "/v1/chat/completions",
                                                       std::string base_url = {});
NativeProviderRequest build_openai_responses_request(const GenerateParams& params, std::string model,
                                                      std::string endpoint = "/v1/responses",
                                                      std::string base_url = {});
NativeProviderRequest build_openai_responses_stream_request(const GenerateParams& params, std::string model,
                                                            std::string endpoint = "/v1/responses",
                                                            std::string base_url = {});
ModelResponse parse_openai_chat_response(const Value& raw, std::string provider, std::string model);
std::vector<ModelStreamEvent> parse_openai_chat_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model);
ModelResponse parse_openai_responses_response(const Value& raw, std::string provider, std::string model);
std::vector<ModelStreamEvent> parse_openai_responses_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model);
NativeProviderRequest build_qwen_chat_request(const GenerateParams& params, std::string model,
                                              std::string api_key = {},
                                              std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1");
NativeProviderRequest build_qwen_chat_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1");
ModelResponse parse_qwen_chat_response(const Value& raw, std::string model);
NativeProviderRequest build_qwen_responses_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1");
NativeProviderRequest build_qwen_responses_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1");
ModelResponse parse_qwen_responses_response(const Value& raw, std::string model);
std::vector<ModelStreamEvent> parse_qwen_responses_stream_events(
    const std::vector<std::string>& chunks,
    std::string model);
NativeProviderRequest build_mimo_chat_request(const GenerateParams& params, std::string model,
                                              std::string api_key = {},
                                              std::string base_url = "https://api.xiaomimimo.com/v1");
NativeProviderRequest build_mimo_chat_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://api.xiaomimimo.com/v1");
ModelResponse parse_mimo_chat_response(const Value& raw, std::string model);
NativeProviderRequest build_anthropic_messages_request(const GenerateParams& params, std::string model,
                                                       std::string api_key = {},
                                                       std::string base_url = "https://api.anthropic.com/v1");
NativeProviderRequest build_anthropic_messages_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://api.anthropic.com/v1");
ModelResponse parse_anthropic_messages_response(const Value& raw, std::string provider, std::string model);
std::vector<ModelStreamEvent> parse_anthropic_messages_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model);
NativeProviderRequest build_deepseek_messages_request(const GenerateParams& params, std::string model,
                                                      std::string api_key = {},
                                                      std::string base_url = "https://api.deepseek.com/anthropic");
NativeProviderRequest build_deepseek_messages_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://api.deepseek.com/anthropic");
ModelResponse parse_deepseek_messages_response(const Value& raw, std::string model);
NativeProviderRequest build_ollama_chat_request(const GenerateParams& params, std::string model,
                                                std::string base_url = "http://127.0.0.1:11434/api");
NativeProviderRequest build_ollama_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string base_url = "http://127.0.0.1:11434/api");
ModelResponse parse_ollama_chat_response(const Value& raw, std::string model);
std::vector<ModelStreamEvent> parse_ollama_chat_stream_events(
    const std::vector<std::string>& chunks,
    std::string model);
NativeProviderRequest build_gemini_generate_content_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://generativelanguage.googleapis.com/v1beta");
NativeProviderRequest build_gemini_generate_content_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://generativelanguage.googleapis.com/v1beta");
ModelResponse parse_gemini_generate_content_response(const Value& raw, std::string model);
std::vector<ModelStreamEvent> parse_gemini_generate_content_stream_events(
    const std::vector<std::string>& chunks,
    std::string model);
std::string json_schema_to_gbnf(const JsonSchema& schema);
std::string llama_tool_envelope_gbnf(const std::vector<ChatToolDescriptor>& tools,
                                     bool required_only = false);

class OpenAICompatibleChatModelAdapter : public ChatModelAdapter {
 public:
  OpenAICompatibleChatModelAdapter(std::string provider, std::string model,
                                   NativeProviderTransport transport,
                                   std::string endpoint = "/v1/chat/completions",
                                   std::string base_url = {},
                                   std::string api_key = {},
                                   NativeProviderStreamTransport stream_transport = {},
                                   std::string organization = {},
                                   NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string endpoint_;
  std::string base_url_;
  std::string api_key_;
  std::string organization_;
};

class QwenChatModelAdapter : public ChatModelAdapter {
 public:
  QwenChatModelAdapter(std::string model, NativeProviderTransport transport,
                       std::string api_key = {},
                       std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
                       NativeProviderStreamTransport stream_transport = {},
                       std::string reasoning_api = "chat-completions",
                       NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string api_key_;
  std::string base_url_;
  std::string reasoning_api_;
};

class MiMoChatModelAdapter : public ChatModelAdapter {
 public:
  MiMoChatModelAdapter(std::string model, NativeProviderTransport transport,
                       std::string api_key = {},
                       std::string base_url = "https://api.xiaomimimo.com/v1",
                       NativeProviderStreamTransport stream_transport = {},
                       NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string api_key_;
  std::string base_url_;
};

class AnthropicChatModelAdapter : public ChatModelAdapter {
 public:
  AnthropicChatModelAdapter(std::string model, NativeProviderTransport transport,
                            std::string api_key = {},
                            std::string base_url = "https://api.anthropic.com/v1",
                            NativeProviderStreamTransport stream_transport = {},
                            NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string api_key_;
  std::string base_url_;
};

class DeepSeekChatModelAdapter : public ChatModelAdapter {
 public:
  DeepSeekChatModelAdapter(std::string model, NativeProviderTransport transport,
                           std::string api_key = {},
                           std::string base_url = "https://api.deepseek.com/anthropic",
                           NativeProviderStreamTransport stream_transport = {},
                           NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string api_key_;
  std::string base_url_;
};

class OllamaChatModelAdapter : public ChatModelAdapter {
 public:
  OllamaChatModelAdapter(std::string model, NativeProviderTransport transport,
                         std::string base_url = "http://127.0.0.1:11434/api",
                         NativeProviderStreamTransport stream_transport = {},
                         NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string base_url_;
};

class GeminiChatModelAdapter : public ChatModelAdapter {
 public:
  GeminiChatModelAdapter(std::string model, NativeProviderTransport transport,
                         std::string api_key = {},
                         std::string base_url = "https://generativelanguage.googleapis.com/v1beta",
                         NativeProviderStreamTransport stream_transport = {},
                         NativeProviderStreamingTransport streaming_transport = {});
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

 private:
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string api_key_;
  std::string base_url_;
};

class LlamaCppNativeChatModelAdapter : public ChatModelAdapter {
 public:
  explicit LlamaCppNativeChatModelAdapter(LlamaCppNativeChatModelAdapterConfig config);
  ModelResponse generate(const GenerateParams& params) override;
  std::vector<ModelStreamEvent> stream(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;

  [[nodiscard]] const LlamaCppNativeRuntimeConfig& native_config() const noexcept;

 private:
  LlamaCppNativeChatRequest build_request(const GenerateParams& params,
                                          const ModelSettings& settings) const;
  ModelResponse build_model_response(const LlamaCppNativeChatResult& raw,
                                     const LlamaCppNativeChatRequest& request,
                                     bool reasoning_enabled,
                                     const std::string& fallback_text = {}) const;

  LlamaCppNativeRuntimeConfig native_config_;
  LlamaCppNativeSamplingConfig sampling_;
  std::string chat_template_;
  bool strict_template_ = false;
  LlamaCppNativeToolMode tool_mode_ = "auto";
  std::string grammar_;
  std::string grammar_root_;
  std::string session_id_;
  std::shared_ptr<LlamaCppNativeBinding> binding_;
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

class OpenAIEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  OpenAIEmbeddingAdapter(std::string model, NativeProviderTransport transport, std::string api_key = {},
                         std::string base_url = "https://api.openai.com/v1", std::string organization = {},
                         int dimensions = 0, std::string space_id = {});
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts, const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

 private:
  NativeProviderTransport transport_;
  std::string api_key_;
  std::string base_url_;
  std::string organization_;
};

class QwenEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  QwenEmbeddingAdapter(std::string model, NativeProviderTransport transport, std::string api_key = {},
                       std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
                       int dimensions = 0, std::string space_id = {});
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts, const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

 private:
  NativeProviderTransport transport_;
  std::string api_key_;
  std::string base_url_;
};

class OllamaEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  OllamaEmbeddingAdapter(std::string model, NativeProviderTransport transport,
                         std::string base_url = "http://127.0.0.1:11434", int dimensions = 0,
                         std::string space_id = {});
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts, const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

 private:
  NativeProviderTransport transport_;
  std::string base_url_;
};

class GeminiEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  GeminiEmbeddingAdapter(std::string model, NativeProviderTransport transport, std::string api_key = {},
                         std::string base_url = "https://generativelanguage.googleapis.com/v1beta",
                         int dimensions = 0, std::string task_type = {}, std::string space_id = {});
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts, const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

 private:
  NativeProviderTransport transport_;
  std::string api_key_;
  std::string base_url_;
  std::string task_type_;
};

class LlamaCppNativeTextEmbeddingAdapter : public TextEmbeddingAdapter {
 public:
  explicit LlamaCppNativeTextEmbeddingAdapter(LlamaCppNativeEmbeddingAdapterConfig config);
  std::vector<EmbeddingVector> embed(const std::vector<std::string>& texts,
                                     const Value& settings = {},
                                     CancellationToken* cancellation = nullptr) override;

  [[nodiscard]] const LlamaCppNativeRuntimeConfig& native_config() const noexcept;

 private:
  LlamaCppNativeRuntimeConfig native_config_;
  LlamaCppNativePooling pooling_ = "mean";
  bool normalize_ = true;
  std::shared_ptr<LlamaCppNativeBinding> binding_;
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
