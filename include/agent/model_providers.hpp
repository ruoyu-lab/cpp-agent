#pragma once

#include "agent/model.hpp"
#include "agent/providers/native.hpp"
#include "agent/providers/reasoning_types.hpp"

namespace agent {

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

Value serialize_chat_tool_descriptor(const ChatToolDescriptor& tool);
Value serialize_openai_chat_messages(const std::vector<AgentMessage>& messages);
NativeProviderRequest build_openai_chat_request(const GenerateParams& params, std::string model,
                                                std::string endpoint = "/v1/chat/completions",
                                                std::string base_url = {});
NativeProviderRequest build_openai_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string endpoint = "/v1/chat/completions",
                                                       std::string base_url = {});
NativeProviderRequest build_openai_responses_request(const GenerateParams& params, std::string model,
                                                      std::string endpoint = "/v1/responses",
                                                      std::string base_url = {});
NativeProviderRequest build_openai_responses_stream_request(const GenerateParams& params, std::string model,
                                                            std::string endpoint = "/v1/responses",
                                                            std::string base_url = {});
AgentOutput parse_openai_chat_response(const Value& raw, std::string provider, std::string model);
std::vector<ModelStreamEvent> parse_openai_chat_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model);
AgentOutput parse_openai_responses_response(const Value& raw, std::string provider, std::string model);
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
AgentOutput parse_qwen_chat_response(const Value& raw, std::string model);
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
AgentOutput parse_qwen_responses_response(const Value& raw, std::string model);
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
AgentOutput parse_mimo_chat_response(const Value& raw, std::string model);
NativeProviderRequest build_anthropic_messages_request(const GenerateParams& params, std::string model,
                                                       std::string api_key = {},
                                                       std::string base_url = "https://api.anthropic.com/v1");
NativeProviderRequest build_anthropic_messages_stream_request(
    const GenerateParams& params,
    std::string model,
    std::string api_key = {},
    std::string base_url = "https://api.anthropic.com/v1");
AgentOutput parse_anthropic_messages_response(const Value& raw, std::string provider, std::string model);
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
AgentOutput parse_deepseek_messages_response(const Value& raw, std::string model);
NativeProviderRequest build_ollama_chat_request(const GenerateParams& params, std::string model,
                                                std::string base_url = "http://127.0.0.1:11434/api");
NativeProviderRequest build_ollama_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string base_url = "http://127.0.0.1:11434/api");
AgentOutput parse_ollama_chat_response(const Value& raw, std::string model);
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
AgentOutput parse_gemini_generate_content_response(const Value& raw, std::string model);
std::vector<ModelStreamEvent> parse_gemini_generate_content_stream_events(
    const std::vector<std::string>& chunks,
    std::string model);
std::string json_schema_to_gbnf(const JsonSchema& schema);
std::string llama_tool_envelope_gbnf(const std::vector<ChatToolDescriptor>& tools,
                                     bool required_only = false);

struct OpenAICompatibleRuntimeOptions {
  std::string service_name;
  std::string api_key_env_name;
  bool require_api_key = false;
  bool supports_responses_api = false;
  bool supports_prompt_cache_key = false;
  std::set<std::string> capabilities;
};

class OpenAICompatibleChatModelAdapter : public ChatModelAdapter {
 public:
  OpenAICompatibleChatModelAdapter(std::string provider, std::string model,
                                   NativeProviderTransport transport,
                                   std::string endpoint = "/v1/chat/completions",
                                   std::string base_url = {},
                                   std::string api_key = {},
                                   NativeProviderStreamTransport stream_transport = {},
                                   std::string organization = {},
                                   NativeProviderStreamingTransport streaming_transport = {},
                                   ProviderReasoningMode reasoning_mode = ProviderReasoningMode::None,
                                   OpenAICompatibleRuntimeOptions runtime_options = {});
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string endpoint_;
  std::string base_url_;
  std::string api_key_;
  std::string organization_;
  ProviderReasoningMode reasoning_mode_ = ProviderReasoningMode::None;
  OpenAICompatibleRuntimeOptions runtime_options_;
};

class QwenChatModelAdapter : public ChatModelAdapter {
 public:
  QwenChatModelAdapter(std::string model, NativeProviderTransport transport,
                       std::string api_key = {},
                       std::string base_url = "https://dashscope.aliyuncs.com/compatible-mode/v1",
                       NativeProviderStreamTransport stream_transport = {},
                       std::string reasoning_api = "chat-completions",
                       NativeProviderStreamingTransport streaming_transport = {});
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
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
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
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
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
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
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
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
                         NativeProviderStreamingTransport streaming_transport = {},
                         ProviderReasoningMode reasoning_mode = ProviderReasoningMode::None);
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string base_url_;
  ProviderReasoningMode reasoning_mode_ = ProviderReasoningMode::None;
};

class GeminiChatModelAdapter : public ChatModelAdapter {
 public:
  GeminiChatModelAdapter(std::string model, NativeProviderTransport transport,
                         std::string api_key = {},
                         std::string base_url = "https://generativelanguage.googleapis.com/v1beta",
                         NativeProviderStreamTransport stream_transport = {},
                         NativeProviderStreamingTransport streaming_transport = {});
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
  NativeProviderTransport transport_;
  NativeProviderStreamTransport stream_transport_;
  NativeProviderStreamingTransport streaming_transport_;
  std::string api_key_;
  std::string base_url_;
};

class LlamaCppNativeChatModelAdapter : public ChatModelAdapter {
 public:
  explicit LlamaCppNativeChatModelAdapter(LlamaCppNativeChatModelAdapterConfig config);
  AgentOutput generate(const GenerateParams& params) override;
  void stream(const GenerateParams& params, StreamEventHandler on_event) override;
  [[nodiscard]] ModelProviderStreamContract stream_contract() const override;

  [[nodiscard]] const LlamaCppNativeRuntimeConfig& native_config() const noexcept;

 private:
  std::vector<ModelStreamEvent> collect_stream_events(const GenerateParams& params);
  LlamaCppNativeChatRequest build_request(const GenerateParams& params,
                                          const ModelSettings& settings) const;
  AgentOutput build_model_output(const LlamaCppNativeChatResult& raw,
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

}  // namespace agent
