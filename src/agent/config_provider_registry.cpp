#include "agent/config.hpp"
#include "agent/http_native.hpp"
#include "agent/model_providers.hpp"
#include "agent/providers/reasoning.hpp"
#include "config_helpers.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>

namespace agent {

namespace {

using config_detail::embedding_dimensions_from_config;
using config_detail::reasoning_mode_from_model_config;
using config_detail::resolve_defaulted_env_config_value;
using config_detail::resolve_env_config_value;

struct OpenAICompatibleProviderProfile {
  const char* provider;
  const char* service_name;
  const char* base_url_env;
  const char* default_base_url;
  const char* api_key_env;
  const char* organization_env;
  bool require_api_key = false;
  bool supports_responses_api = false;
  bool supports_prompt_cache_key = false;
  bool derive_chat_endpoint_from_base_url = false;
};

std::optional<int> optional_int_config_value(const Value& value, const std::string& key) {
  if (value.is_object() && value.at(key).is_number()) {
    return static_cast<int>(value.at(key).as_integer());
  }
  return std::nullopt;
}

std::optional<double> optional_number_config_value(const Value& value, const std::string& key) {
  if (value.is_object() && value.at(key).is_number()) {
    return value.at(key).as_number();
  }
  return std::nullopt;
}

std::optional<bool> optional_bool_config_value(const Value& value, const std::string& key) {
  if (value.is_object() && value.at(key).is_bool()) {
    return value.at(key).as_bool();
  }
  return std::nullopt;
}

LlamaCppNativeRuntimeConfig llama_runtime_config_from_value(const Value& value) {
  LlamaCppNativeRuntimeConfig config;
  config.model_path = resolve_env_config_value(value.at("modelPath"), value.at("modelPathEnv"));
  config.library_path = resolve_env_config_value(value.at("libraryPath"), value.at("libraryPathEnv"));
  config.library_dir = resolve_env_config_value(value.at("libraryDir"), value.at("libraryDirEnv"));
  config.context_size = optional_int_config_value(value, "contextSize");
  config.batch_size = optional_int_config_value(value, "batchSize");
  config.threads = optional_int_config_value(value, "threads");
  config.batch_threads = optional_int_config_value(value, "batchThreads");
  config.gpu_layers = optional_int_config_value(value, "gpuLayers");
  config.mmap = optional_bool_config_value(value, "mmap");
  config.mlock = optional_bool_config_value(value, "mlock");
  config.mmproj_path = resolve_env_config_value(value.at("mmprojPath"), value.at("mmprojPathEnv"));
  config.mtmd_library_path = resolve_env_config_value(value.at("mtmdLibraryPath"), value.at("mtmdLibraryPathEnv"));
  config.mtmd_library_dir = resolve_env_config_value(value.at("mtmdLibraryDir"), value.at("mtmdLibraryDirEnv"));
  config.mmproj_use_gpu = optional_bool_config_value(value, "mmprojUseGpu");
  config.mmproj_threads = optional_int_config_value(value, "mmprojThreads");
  config.mmproj_image_min_tokens = optional_int_config_value(value, "mmprojImageMinTokens");
  config.mmproj_image_max_tokens = optional_int_config_value(value, "mmprojImageMaxTokens");
  config.media_marker = value.at("mediaMarker").as_string("<__media__>");
  const auto& lora_adapters = value.at("loraAdapters").is_array()
                                  ? value.at("loraAdapters")
                                  : value.at("lora_adapters");
  for (const auto& item : lora_adapters.as_array()) {
    if (!item.is_object()) {
      continue;
    }
    const auto path = item.at("path").as_string();
    if (path.empty()) {
      continue;
    }
    config.lora_adapters.push_back(LlamaCppNativeLoraAdapter{
        .path = path,
        .scale = item.at("scale").is_number() ? item.at("scale").as_number() : 1.0,
    });
  }
  return config;
}

LlamaCppNativeSamplingConfig llama_sampling_config_from_value(const Value& value) {
  LlamaCppNativeSamplingConfig config;
  config.temperature = optional_number_config_value(value, "temperature");
  config.max_output_tokens = optional_int_config_value(value, "maxOutputTokens");
  config.top_k = optional_int_config_value(value, "topK");
  config.top_p = optional_number_config_value(value, "topP");
  config.min_p = optional_number_config_value(value, "minP");
  config.repeat_penalty = optional_number_config_value(value, "repeatPenalty");
  config.seed = optional_int_config_value(value, "seed");
  return config;
}

LlamaCppNativeChatModelAdapterConfig llama_chat_config_from_value(
    const Value& value,
    NativeLlamaCppAdapters llama_adapters) {
  LlamaCppNativeChatModelAdapterConfig config;
  static_cast<LlamaCppNativeRuntimeConfig&>(config) = llama_runtime_config_from_value(value);
  static_cast<LlamaCppNativeSamplingConfig&>(config) = llama_sampling_config_from_value(value);
  config.model = value.at("model").as_string();
  config.chat_template = value.at("chatTemplate").as_string(value.at("chat_template").as_string());
  config.strict_template = value.at("strictTemplate").as_bool(value.at("strict_template").as_bool(false));
  config.tool_mode = value.at("toolMode").as_string(value.at("tool_mode").as_string("auto"));
  config.grammar = value.at("grammar").as_string();
  config.grammar_root = value.at("grammarRoot").as_string(value.at("grammar_root").as_string());
  config.session_id = value.at("sessionId").as_string(value.at("session_id").as_string());
  if (value.at("reasoning").is_object()) {
    config.reasoning = reasoning_settings_from_json_value(value.at("reasoning"));
  }
  config.binding = std::move(llama_adapters.binding);
  return config;
}

LlamaCppNativeEmbeddingAdapterConfig llama_embedding_config_from_value(
    const Value& value,
    NativeLlamaCppAdapters llama_adapters) {
  LlamaCppNativeEmbeddingAdapterConfig config;
  static_cast<LlamaCppNativeRuntimeConfig&>(config) = llama_runtime_config_from_value(value);
  config.model = value.at("model").as_string();
  config.pooling = value.at("pooling").as_string("mean");
  config.normalize = value.contains("normalize") ? value.at("normalize").as_bool() : true;
  const auto dimensions = embedding_dimensions_from_config(value, 0);
  if (dimensions > 0) {
    config.dimensions = dimensions;
  }
  config.space_id = value.at("spaceId").as_string(value.at("space_id").as_string());
  config.binding = std::move(llama_adapters.binding);
  return config;
}

std::map<std::string, NativeTextEmbeddingProviderDescriptor>& native_text_embedding_provider_descriptors() {
  static std::map<std::string, NativeTextEmbeddingProviderDescriptor> descriptors;
  return descriptors;
}

std::mutex& native_text_embedding_provider_descriptors_mutex() {
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, NativeImageEmbeddingProviderDescriptor>& native_image_embedding_provider_descriptors() {
  static std::map<std::string, NativeImageEmbeddingProviderDescriptor> descriptors;
  return descriptors;
}

std::mutex& native_image_embedding_provider_descriptors_mutex() {
  static std::mutex mutex;
  return mutex;
}

void put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor descriptor) {
  if (descriptor.provider.empty()) {
    throw ConfigurationError("Native text embedding provider descriptor requires provider.");
  }
  if (!descriptor.create_adapter) {
    throw ConfigurationError("Native text embedding provider descriptor \"" + descriptor.provider +
                             "\" requires create_adapter.");
  }
  native_text_embedding_provider_descriptors()[descriptor.provider] = std::move(descriptor);
}

void put_native_image_embedding_provider_descriptor(NativeImageEmbeddingProviderDescriptor descriptor) {
  if (descriptor.provider.empty()) {
    throw ConfigurationError("Native image embedding provider descriptor requires provider.");
  }
  if (!descriptor.create_adapter) {
    throw ConfigurationError("Native image embedding provider descriptor \"" + descriptor.provider +
                             "\" requires create_adapter.");
  }
  native_image_embedding_provider_descriptors()[descriptor.provider] = std::move(descriptor);
}

void ensure_builtin_native_embedding_provider_descriptors() {
  static std::once_flag once;
  std::call_once(once, [] {
    {
      std::lock_guard<std::mutex> lock(native_text_embedding_provider_descriptors_mutex());
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "hash",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext) {
            return std::make_shared<HashEmbeddingAdapter>(
                embedding_dimensions_from_config(embedding, 256),
                embedding.at("model").as_string("hash-embedding"),
                embedding.at("spaceId").as_string("hash-shared-v1"));
          },
          .resolve_known_dimensions = [](const Value& embedding) -> std::optional<int> {
            return embedding_dimensions_from_config(embedding, 256);
          },
      });
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "clip",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext) {
            return std::make_shared<ClipTextEmbeddingAdapter>(
                embedding.at("model").as_string("Xenova/clip-vit-base-patch32"),
                embedding_dimensions_from_config(embedding, 0),
                embedding.at("spaceId").as_string("clip-shared-v1"));
          },
      });
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "llamacpp-native",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext context) {
            return std::make_shared<LlamaCppNativeTextEmbeddingAdapter>(
                llama_embedding_config_from_value(embedding, std::move(context.llama_adapters)));
          },
          .collect_env_refs = [](const Value& embedding) {
            return std::vector<std::string>{
                embedding.at("modelPathEnv").as_string(),
                embedding.at("libraryPathEnv").as_string(),
                embedding.at("libraryDirEnv").as_string(),
                embedding.at("mmprojPathEnv").as_string(),
                embedding.at("mtmdLibraryPathEnv").as_string(),
                embedding.at("mtmdLibraryDirEnv").as_string(),
            };
          },
          .resolve_known_dimensions = [](const Value& embedding) -> std::optional<int> {
            const auto dimensions = embedding_dimensions_from_config(embedding, 0);
            return dimensions > 0 ? std::optional<int>(dimensions) : std::nullopt;
          },
      });
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "openai",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext context) {
            auto transport = context.transport;
            if (!transport) {
              transport = create_native_provider_http_transport(create_native_http_transport());
            }
            const auto base_url = resolve_defaulted_env_config_value(
                embedding.at("baseUrl"), embedding.at("baseUrlEnv"),
                "OPENAI_BASE_URL", "https://api.openai.com/v1");
            return std::make_shared<OpenAIEmbeddingAdapter>(
                resolve_defaulted_env_config_value(embedding.at("model"), Value(),
                                                   "OPENAI_EMBEDDING_MODEL",
                                                   "text-embedding-3-small"),
                transport,
                resolve_defaulted_env_config_value(embedding.at("apiKey"), embedding.at("apiKeyEnv"),
                                                   "OPENAI_API_KEY"),
                base_url,
                resolve_defaulted_env_config_value(embedding.at("organization"),
                                                   embedding.at("organizationEnv"),
                                                   "OPENAI_ORG_ID"),
                embedding_dimensions_from_config(embedding, 0),
                embedding.at("spaceId").as_string());
          },
          .collect_env_refs = [](const Value& embedding) {
            return std::vector<std::string>{
                embedding.at("apiKeyEnv").as_string(),
                embedding.at("baseUrlEnv").as_string(),
                embedding.at("organizationEnv").as_string(),
            };
          },
      });
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "qwen",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext context) {
            auto transport = context.transport;
            if (!transport) {
              transport = create_native_provider_http_transport(create_native_http_transport());
            }
            return std::make_shared<QwenEmbeddingAdapter>(
                resolve_defaulted_env_config_value(embedding.at("model"), Value(),
                                                   "QWEN_EMBEDDING_MODEL",
                                                   "text-embedding-v4"),
                transport,
                resolve_defaulted_env_config_value(embedding.at("apiKey"), embedding.at("apiKeyEnv"),
                                                   "QWEN_API_KEY"),
                resolve_defaulted_env_config_value(embedding.at("baseUrl"), embedding.at("baseUrlEnv"),
                                                   "QWEN_BASE_URL",
                                                   "https://dashscope.aliyuncs.com/compatible-mode/v1"),
                embedding_dimensions_from_config(embedding, 0),
                embedding.at("spaceId").as_string());
          },
          .collect_env_refs = [](const Value& embedding) {
            return std::vector<std::string>{
                embedding.at("apiKeyEnv").as_string(),
                embedding.at("baseUrlEnv").as_string(),
            };
          },
      });
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "ollama",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext context) {
            auto transport = context.transport;
            if (!transport) {
              transport = create_native_provider_http_transport(create_native_http_transport());
            }
            return std::make_shared<OllamaEmbeddingAdapter>(
                resolve_defaulted_env_config_value(embedding.at("model"), Value(),
                                                   "OLLAMA_EMBEDDING_MODEL",
                                                   "embeddinggemma"),
                transport,
                resolve_defaulted_env_config_value(embedding.at("baseUrl"), embedding.at("baseUrlEnv"),
                                                   "OLLAMA_BASE_URL",
                                                   "http://127.0.0.1:11434"),
                embedding_dimensions_from_config(embedding, 0),
                embedding.at("spaceId").as_string());
          },
          .collect_env_refs = [](const Value& embedding) {
            return std::vector<std::string>{embedding.at("baseUrlEnv").as_string()};
          },
      });
      put_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor{
          .provider = "gemini",
          .create_adapter = [](const Value& embedding, NativeEmbeddingProviderBuildContext context) {
            auto transport = context.transport;
            if (!transport) {
              transport = create_native_provider_http_transport(create_native_http_transport());
            }
            return std::make_shared<GeminiEmbeddingAdapter>(
                resolve_defaulted_env_config_value(embedding.at("model"), Value(),
                                                   "GEMINI_EMBEDDING_MODEL",
                                                   "gemini-embedding-001"),
                transport,
                resolve_defaulted_env_config_value(embedding.at("apiKey"), embedding.at("apiKeyEnv"),
                                                   "GEMINI_API_KEY"),
                resolve_defaulted_env_config_value(embedding.at("baseUrl"), embedding.at("baseUrlEnv"),
                                                   "GEMINI_BASE_URL",
                                                   "https://generativelanguage.googleapis.com/v1beta"),
                embedding_dimensions_from_config(embedding, 0),
                embedding.at("taskType").as_string(),
                embedding.at("spaceId").as_string());
          },
          .collect_env_refs = [](const Value& embedding) {
            return std::vector<std::string>{
                embedding.at("apiKeyEnv").as_string(),
                embedding.at("baseUrlEnv").as_string(),
            };
          },
      });
    }
    {
      std::lock_guard<std::mutex> lock(native_image_embedding_provider_descriptors_mutex());
      put_native_image_embedding_provider_descriptor(NativeImageEmbeddingProviderDescriptor{
          .provider = "hash",
          .create_adapter = [](const Value& embedding) {
            return std::make_shared<HashImageEmbeddingAdapter>(
                embedding_dimensions_from_config(embedding, 256),
                embedding.at("model").as_string("hash-image-embedding"),
                embedding.at("spaceId").as_string("hash-shared-v1"));
          },
          .resolve_known_dimensions = [](const Value& embedding) -> std::optional<int> {
            return embedding_dimensions_from_config(embedding, 256);
          },
      });
      put_native_image_embedding_provider_descriptor(NativeImageEmbeddingProviderDescriptor{
          .provider = "clip",
          .create_adapter = [](const Value& embedding) {
            return std::make_shared<ClipImageEmbeddingAdapter>(
                embedding.at("model").as_string("Xenova/clip-vit-base-patch32"),
                embedding_dimensions_from_config(embedding, 0),
                embedding.at("spaceId").as_string("clip-shared-v1"));
          },
      });
    }
  });
}

std::map<std::string, NativeChatProviderDescriptor>& native_chat_provider_descriptors() {
  static std::map<std::string, NativeChatProviderDescriptor> descriptors;
  return descriptors;
}

std::mutex& native_chat_provider_descriptors_mutex() {
  static std::mutex mutex;
  return mutex;
}

void put_native_chat_provider_descriptor(NativeChatProviderDescriptor descriptor) {
  if (descriptor.provider.empty()) {
    throw ConfigurationError("Native chat provider descriptor requires provider.");
  }
  if (!descriptor.create_adapter) {
    throw ConfigurationError("Native chat provider descriptor \"" + descriptor.provider +
                             "\" requires create_adapter.");
  }
  native_chat_provider_descriptors()[descriptor.provider] = std::move(descriptor);
}

std::string required_chat_model_name(const Value& model, const std::string& provider) {
  const std::string name = model.at("model").as_string();
  if (name.empty()) {
    throw ConfigurationError("Provider \"" + provider + "\" requires model.");
  }
  return name;
}

std::vector<std::string> env_refs_from_model(
    const Value& model,
    std::initializer_list<const char*> fields) {
  std::vector<std::string> refs;
  for (const auto* field : fields) {
    const auto value = model.at(field).as_string();
    if (!value.empty()) {
      refs.push_back(value);
    }
  }
  return refs;
}

std::set<std::string> openai_chat_capabilities() {
  return {"input.text", "input.image", "input.file", "input.audio", "input.video",
          "output.structuredContent", "transport.inline", "transport.reference",
          "reasoning", "reasoning.budget", "reasoning.includeThoughts"};
}

std::set<std::string> openai_compatible_chat_capabilities() {
  return {"input.text", "output.structuredContent", "transport.inline", "reasoning"};
}

std::set<std::string> full_multimodal_reasoning_capabilities() {
  return {"input.text", "input.image", "input.file", "input.audio", "input.video",
          "output.structuredContent", "transport.inline", "transport.reference",
          "reasoning", "reasoning.budget", "reasoning.includeThoughts"};
}

std::set<std::string> llama_cpp_native_chat_capabilities(const Value& model) {
  auto capabilities = std::set<std::string>{
      "input.text", "output.structuredContent", "transport.inline", "reasoning"};
  if (model.at("mmprojPath").is_string() || model.at("mmprojPathEnv").is_string()) {
    capabilities.insert("input.image");
  }
  return capabilities;
}

ProviderRequestProfile llama_cpp_native_request_profile(const Value& model) {
  (void)model;
  return ProviderRequestProfile{
      .provider = "llamacpp-native",
      .runtime = "llamacpp-native",
      .protocol = ProviderRequestProtocol::Unknown,
      .reasoning_mode = ProviderReasoningMode::None,
      .supports_reasoning = true,
      .supports_reasoning_budget = false,
      .supports_reasoning_visibility = false,
      .strict_reasoning = false,
      .reasoning_disable_strategy = ProviderReasoningDisableStrategy::Unsupported,
  };
}

ModelUsage common_native_usage_extractor(const NativeProviderUsageExtractionContext& context) {
  if (context.response) {
    return extract_model_usage(*context.response);
  }
  if (context.raw) {
    return extract_model_usage(*context.raw, context.provider);
  }
  ModelUsage usage;
  usage.provider = context.provider;
  return usage;
}

NativeChatProviderStreamParserMetadata stream_parser_metadata(
    std::string parser,
    ModelStreamFrameBoundary boundary,
    std::set<ModelStreamEventType> events,
    bool includes_terminal_usage = true,
    NativeProviderReasoningParser reasoning_parser = NativeProviderReasoningParser::Native) {
  return NativeChatProviderStreamParserMetadata{
      .parser = std::move(parser),
      .frame_boundary = boundary,
      .events = std::move(events),
      .includes_terminal_usage = includes_terminal_usage,
      .reasoning_parser = reasoning_parser,
  };
}

NativeChatProviderMediaInputMarshaller media_input_marshaller(
    std::string name,
    std::set<ContentPartType> supported_parts,
    std::set<MediaSourceKind> accepted_sources,
    bool requires_resolver_for_non_inline = true) {
  return NativeChatProviderMediaInputMarshaller{
      .name = std::move(name),
      .supported_parts = std::move(supported_parts),
      .accepted_sources = std::move(accepted_sources),
      .requires_resolver_for_non_inline = requires_resolver_for_non_inline,
  };
}

const Value& scoped_provider_extra_or_null(const Value& extra, const std::string& provider) {
  static const Value kNull;
  if (!extra.is_object()) {
    return kNull;
  }
  const auto& scoped = extra.at(provider);
  return scoped.is_object() ? scoped : kNull;
}

void merge_object_fields(Value& target, const Value& source) {
  if (!source.is_object()) {
    return;
  }
  for (const auto& [key, value] : source.as_object()) {
    target[key] = value;
  }
}

std::optional<double> provider_extra_number(const Value& scoped,
                                            const std::string& provider,
                                            const std::string& camel_key,
                                            const std::string& snake_key) {
  const auto& value = scoped.at(snake_key).is_null() ? scoped.at(camel_key) : scoped.at(snake_key);
  const auto key = scoped.at(snake_key).is_null() ? camel_key : snake_key;
  if (value.is_null()) {
    return std::nullopt;
  }
  if (!value.is_number()) {
    throw ConfigurationError("ModelSettings.extra." + provider + "." + key + " must be a number.");
  }
  return value.as_number();
}

void apply_ollama_descriptor_extra(NativeProviderRequest& request,
                                   const Value& model,
                                   const ModelSettings& settings) {
  if (!request.body.at("options").is_object()) {
    request.body["options"] = Value::object({});
  }
  auto& options = request.body["options"];
  for (const auto* extra : {&model.at("extra"), &settings.extra}) {
    const auto& scoped = scoped_provider_extra_or_null(*extra, "ollama");
    if (!scoped.is_object()) {
      continue;
    }
    merge_object_fields(options, scoped.at("options"));
    if (const auto num_ctx = provider_extra_number(scoped, "ollama", "numCtx", "num_ctx")) {
      options["num_ctx"] = *num_ctx;
    }
    const auto& keep_alive = scoped.at("keep_alive").is_null()
                                 ? scoped.at("keepAlive")
                                 : scoped.at("keep_alive");
    if (!keep_alive.is_null()) {
      if (!keep_alive.is_string() && !keep_alive.is_number()) {
        throw ConfigurationError("ModelSettings.extra.ollama.keepAlive must be a string or number.");
      }
      request.body["keep_alive"] = keep_alive;
    }
  }
}

std::shared_ptr<ChatModelAdapter> create_openai_compatible_chat_adapter(
    const std::string& provider,
    const OpenAICompatibleProviderProfile& profile,
    const Value& model,
    NativeChatProviderBuildContext context) {
  const std::string name = required_chat_model_name(model, provider);
  std::string endpoint = model.at("endpoint").as_string("/v1/chat/completions");
  std::string base_url = resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                            profile.base_url_env, profile.default_base_url);
  std::string api_key = profile.api_key_env[0] == '\0'
                            ? resolve_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"))
                            : resolve_defaulted_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"),
                                                                 profile.api_key_env, {});
  std::string organization = profile.organization_env[0] == '\0'
                                 ? std::string()
                                 : resolve_defaulted_env_config_value(model.at("organization"),
                                                                      model.at("organizationEnv"),
                                                                      profile.organization_env,
                                                                      {});
  if (!model.contains("endpoint")) {
    if (profile.derive_chat_endpoint_from_base_url) {
      while (!base_url.empty() && base_url.back() == '/') {
        base_url.pop_back();
      }
      endpoint = base_url.size() >= 3 &&
                         base_url.compare(base_url.size() - 3, 3, "/v1") == 0
                     ? "/chat/completions"
                     : "/v1/chat/completions";
    } else {
      endpoint = "/chat/completions";
    }
  }
  OpenAICompatibleRuntimeOptions runtime_options{
      .service_name = profile.service_name,
      .api_key_env_name = profile.api_key_env,
      .require_api_key = profile.require_api_key,
      .supports_responses_api = profile.supports_responses_api,
      .supports_prompt_cache_key = profile.supports_prompt_cache_key,
      .capabilities = profile.supports_responses_api
                          ? openai_chat_capabilities()
                          : openai_compatible_chat_capabilities(),
  };

  return std::make_shared<OpenAICompatibleChatModelAdapter>(
      provider, name, std::move(context.transport), endpoint, base_url, api_key,
      std::move(context.stream_transport), organization, std::move(context.streaming_transport),
      reasoning_mode_from_model_config(model, "model"), std::move(runtime_options));
}

void register_openai_compatible_native_chat_descriptor(
    const OpenAICompatibleProviderProfile& profile) {
  const std::string provider = profile.provider;
  put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
      .provider = provider,
      .create_adapter = [provider, profile](const Value& model,
                                            NativeChatProviderBuildContext context) {
        return create_openai_compatible_chat_adapter(provider, profile, model, std::move(context));
      },
      .resolve_protocol = [](const Value&) {
        return ProviderRequestProtocol::OpenAIChatCompletions;
      },
      .default_capabilities = profile.supports_responses_api
                                  ? openai_chat_capabilities()
                                  : openai_compatible_chat_capabilities(),
      .collect_env_refs = [profile](const Value& model) {
        auto refs = env_refs_from_model(model, {"apiKeyEnv", "baseUrlEnv"});
        if (profile.organization_env[0] != '\0') {
          auto organization_refs = env_refs_from_model(model, {"organizationEnv"});
          refs.insert(refs.end(), organization_refs.begin(), organization_refs.end());
        }
        return refs;
      },
      .extract_usage = common_native_usage_extractor,
      .media_input_marshaller = media_input_marshaller(
          "openai-compatible",
          {ContentPartType::Image, ContentPartType::File, ContentPartType::Audio, ContentPartType::Video},
          {MediaSourceKind::Inline, MediaSourceKind::Url, MediaSourceKind::Path, MediaSourceKind::Artifact}),
      .resolve_stream_parser_metadata = [](const Value&) {
        return stream_parser_metadata(
            "openai-compatible-chat-completions",
            ModelStreamFrameBoundary::SseEvent,
            {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
             ModelStreamEventType::ReasoningDelta, ModelStreamEventType::ToolCallDelta,
             ModelStreamEventType::Response});
      },
  });
}

void ensure_builtin_native_chat_provider_descriptors() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::lock_guard<std::mutex> lock(native_chat_provider_descriptors_mutex());
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "echo",
        .create_adapter = [](const Value&, NativeChatProviderBuildContext) {
          return std::make_shared<EchoChatModelAdapter>();
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::Unknown;
        },
        .default_capabilities = {"input.text", "transport.inline"},
        .extract_usage = common_native_usage_extractor,
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "echo",
              ModelStreamFrameBoundary::None,
              {ModelStreamEventType::Response},
              false,
              NativeProviderReasoningParser::None);
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "llamacpp-native",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          return std::make_shared<LlamaCppNativeChatModelAdapter>(
              llama_chat_config_from_value(model, std::move(context.llama_adapters)));
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::Unknown;
        },
        .resolve_profile = llama_cpp_native_request_profile,
        .default_capabilities = {"input.text", "output.structuredContent", "transport.inline", "reasoning"},
        .resolve_capabilities = llama_cpp_native_chat_capabilities,
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"modelPathEnv", "libraryPathEnv", "libraryDirEnv",
                                            "mmprojPathEnv", "mtmdLibraryPathEnv", "mtmdLibraryDirEnv"});
        },
        .extract_usage = common_native_usage_extractor,
        .media_input_marshaller = media_input_marshaller(
            "llamacpp-native",
            {ContentPartType::Image},
            {MediaSourceKind::Inline, MediaSourceKind::Path, MediaSourceKind::Artifact}),
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "llamacpp-native",
              ModelStreamFrameBoundary::NativeCallback,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ReasoningDelta, ModelStreamEventType::Response});
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "ollama",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          const std::string name = required_chat_model_name(model, "ollama");
          const std::string base_url =
              resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                 "OLLAMA_BASE_URL", "http://127.0.0.1:11434");
          return std::make_shared<OllamaChatModelAdapter>(
              name, std::move(context.transport), base_url,
              std::move(context.stream_transport), std::move(context.streaming_transport),
              reasoning_mode_from_model_config(model, "model"));
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::OllamaChat;
        },
        .default_capabilities = {"input.text", "input.image", "transport.inline", "reasoning"},
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"baseUrlEnv"});
        },
        .extract_usage = common_native_usage_extractor,
        .apply_extra = apply_ollama_descriptor_extra,
        .media_input_marshaller = media_input_marshaller(
            "ollama-chat",
            {ContentPartType::Image},
            {MediaSourceKind::Inline, MediaSourceKind::Path, MediaSourceKind::Artifact}),
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "ollama-chat",
              ModelStreamFrameBoundary::JsonLine,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ReasoningDelta, ModelStreamEventType::Response});
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "qwen",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          const std::string name =
              resolve_defaulted_env_config_value(model.at("model"), Value(), "QWEN_MODEL", "qwen-plus");
          const std::string api_key =
              resolve_defaulted_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"), "QWEN_API_KEY", {});
          const std::string base_url =
              resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                 "QWEN_BASE_URL",
                                                 "https://dashscope.aliyuncs.com/compatible-mode/v1");
          const std::string reasoning_api = model.at("reasoningApi").as_string("chat-completions");
          return std::make_shared<QwenChatModelAdapter>(
              name, std::move(context.transport), api_key, base_url,
              std::move(context.stream_transport), reasoning_api,
              std::move(context.streaming_transport));
        },
        .resolve_protocol = [](const Value& model) {
          return model.at("reasoningApi").as_string("chat-completions") == "responses"
                     ? ProviderRequestProtocol::QwenResponses
                     : ProviderRequestProtocol::QwenChatCompletions;
        },
        .default_capabilities = full_multimodal_reasoning_capabilities(),
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"apiKeyEnv", "baseUrlEnv"});
        },
        .validate_model = [](const Value& model, const std::string& path) {
          if (!model.contains("reasoningApi")) {
            return;
          }
          const auto reasoning_api = model.at("reasoningApi").as_string();
          if (reasoning_api != "chat-completions" && reasoning_api != "responses") {
            throw ConfigurationError(path + ".reasoningApi must be \"chat-completions\" or \"responses\".");
          }
        },
        .extract_usage = common_native_usage_extractor,
        .media_input_marshaller = media_input_marshaller(
            "openai-compatible",
            {ContentPartType::Image, ContentPartType::File, ContentPartType::Audio, ContentPartType::Video},
            {MediaSourceKind::Inline, MediaSourceKind::Url, MediaSourceKind::Path, MediaSourceKind::Artifact}),
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "qwen",
              ModelStreamFrameBoundary::SseEvent,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ReasoningDelta, ModelStreamEventType::ToolCallDelta,
               ModelStreamEventType::Response});
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "mimo",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          const std::string name =
              resolve_defaulted_env_config_value(model.at("model"), Value(), "MIMO_MODEL", "mimo-v2-flash");
          const std::string api_key =
              resolve_defaulted_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"), "MIMO_API_KEY", {});
          const std::string base_url =
              resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                 "MIMO_BASE_URL", "https://api.xiaomimimo.com/v1");
          return std::make_shared<MiMoChatModelAdapter>(
              name, std::move(context.transport), api_key, base_url,
              std::move(context.stream_transport), std::move(context.streaming_transport));
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::OpenAIChatCompletions;
        },
        .default_capabilities = {"input.text", "input.image", "input.file", "output.structuredContent",
                                 "transport.inline", "transport.reference"},
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"apiKeyEnv", "baseUrlEnv"});
        },
        .extract_usage = common_native_usage_extractor,
        .media_input_marshaller = media_input_marshaller(
            "openai-compatible",
            {ContentPartType::Image, ContentPartType::File},
            {MediaSourceKind::Inline, MediaSourceKind::Url, MediaSourceKind::Path, MediaSourceKind::Artifact}),
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "openai-compatible-chat-completions",
              ModelStreamFrameBoundary::SseEvent,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ToolCallDelta, ModelStreamEventType::Response},
              true,
              NativeProviderReasoningParser::None);
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "anthropic",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          const std::string name = required_chat_model_name(model, "anthropic");
          const std::string api_key =
              resolve_defaulted_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"),
                                                 "ANTHROPIC_API_KEY", {});
          const std::string base_url =
              resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                 "ANTHROPIC_BASE_URL", "https://api.anthropic.com/v1");
          return std::make_shared<AnthropicChatModelAdapter>(
              name, std::move(context.transport), api_key, base_url,
              std::move(context.stream_transport), std::move(context.streaming_transport));
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::AnthropicMessages;
        },
        .default_capabilities = {"input.text", "input.image", "input.file", "transport.inline",
                                 "transport.reference", "reasoning", "reasoning.budget"},
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"apiKeyEnv", "baseUrlEnv"});
        },
        .extract_usage = common_native_usage_extractor,
        .media_input_marshaller = media_input_marshaller(
            "anthropic-messages",
            {ContentPartType::Image, ContentPartType::File},
            {MediaSourceKind::Inline, MediaSourceKind::Url, MediaSourceKind::Path, MediaSourceKind::Artifact}),
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "anthropic-messages",
              ModelStreamFrameBoundary::SseEvent,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ReasoningDelta, ModelStreamEventType::ToolCallDelta,
               ModelStreamEventType::Response});
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "deepseek",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          const std::string name =
              resolve_defaulted_env_config_value(model.at("model"), Value(), "DEEPSEEK_MODEL",
                                                 "deepseek-v4-flash");
          const std::string api_key =
              resolve_defaulted_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"),
                                                 "DEEPSEEK_API_KEY", {});
          const std::string base_url =
              resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                 "DEEPSEEK_BASE_URL",
                                                 "https://api.deepseek.com/anthropic");
          return std::make_shared<DeepSeekChatModelAdapter>(
              name, std::move(context.transport), api_key, base_url,
              std::move(context.stream_transport), std::move(context.streaming_transport));
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::DeepSeekMessages;
        },
        .default_capabilities = {"input.text", "output.structuredContent", "transport.inline", "reasoning"},
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"apiKeyEnv", "baseUrlEnv"});
        },
        .extract_usage = common_native_usage_extractor,
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "deepseek-messages",
              ModelStreamFrameBoundary::SseEvent,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ReasoningDelta, ModelStreamEventType::Response});
        },
    });
    put_native_chat_provider_descriptor(NativeChatProviderDescriptor{
        .provider = "gemini",
        .create_adapter = [](const Value& model, NativeChatProviderBuildContext context) {
          const std::string name = required_chat_model_name(model, "gemini");
          const std::string api_key =
              resolve_defaulted_env_config_value(model.at("apiKey"), model.at("apiKeyEnv"),
                                                 "GEMINI_API_KEY", {});
          const std::string base_url =
              resolve_defaulted_env_config_value(model.at("baseUrl"), model.at("baseUrlEnv"),
                                                 "GEMINI_BASE_URL",
                                                 "https://generativelanguage.googleapis.com/v1beta");
          return std::make_shared<GeminiChatModelAdapter>(
              name, std::move(context.transport), api_key, base_url,
              std::move(context.stream_transport), std::move(context.streaming_transport));
        },
        .resolve_protocol = [](const Value&) {
          return ProviderRequestProtocol::GeminiGenerateContent;
        },
        .default_capabilities = full_multimodal_reasoning_capabilities(),
        .collect_env_refs = [](const Value& model) {
          return env_refs_from_model(model, {"apiKeyEnv", "baseUrlEnv"});
        },
        .extract_usage = common_native_usage_extractor,
        .media_input_marshaller = media_input_marshaller(
            "gemini-generate-content",
            {ContentPartType::Image, ContentPartType::File, ContentPartType::Audio, ContentPartType::Video},
            {MediaSourceKind::Inline, MediaSourceKind::Url, MediaSourceKind::Path, MediaSourceKind::Artifact}),
        .resolve_stream_parser_metadata = [](const Value&) {
          return stream_parser_metadata(
              "gemini-generate-content",
              ModelStreamFrameBoundary::SseEvent,
              {ModelStreamEventType::ResponseStart, ModelStreamEventType::TextDelta,
               ModelStreamEventType::ReasoningDelta, ModelStreamEventType::ToolCallDelta,
               ModelStreamEventType::Response});
        },
    });
    static const OpenAICompatibleProviderProfile kProfiles[] = {
        {"openai", "OpenAI", "OPENAI_BASE_URL", "https://api.openai.com/v1",
         "OPENAI_API_KEY", "OPENAI_ORG_ID", true, true, true, true},
        {"lmstudio", "LM Studio", "LMSTUDIO_BASE_URL", "http://127.0.0.1:1234/v1",
         "", "", false, false, false, false},
        {"llamacpp", "llama.cpp server", "LLAMACPP_BASE_URL", "http://127.0.0.1:8080/v1",
         "LLAMACPP_API_KEY", "", false, false, false, false},
        {"vllm", "vLLM", "VLLM_BASE_URL", "http://127.0.0.1:8000/v1",
         "VLLM_API_KEY", "", false, false, false, false},
        {"openrouter", "OpenRouter", "OPENROUTER_BASE_URL", "https://openrouter.ai/api/v1",
         "OPENROUTER_API_KEY", "", true, false, false, false},
        {"groq", "Groq", "GROQ_BASE_URL", "https://api.groq.com/openai/v1",
         "GROQ_API_KEY", "", true, false, false, false},
        {"siliconflow", "SiliconFlow", "SILICONFLOW_BASE_URL", "https://api.siliconflow.cn/v1",
         "SILICONFLOW_API_KEY", "", true, false, false, false},
        {"omlx", "oMLX", "OMLX_BASE_URL", "http://127.0.0.1:8000/v1",
         "OMLX_API_KEY", "", false, false, false, false},
        {"kimi", "Kimi", "KIMI_BASE_URL", "https://api.moonshot.ai/v1",
         "MOONSHOT_API_KEY", "", true, false, false, false},
        {"zhipu", "Zhipu GLM", "ZHIPU_BASE_URL", "https://open.bigmodel.cn/api/paas/v4",
         "ZHIPU_API_KEY", "", true, false, false, false},
        {"doubao", "Doubao / Volcengine Ark", "DOUBAO_BASE_URL", "https://ark.cn-beijing.volces.com/api/v3",
         "DOUBAO_API_KEY", "", true, false, false, false},
        {"hunyuan", "Tencent Hunyuan", "HUNYUAN_BASE_URL", "https://api.hunyuan.cloud.tencent.com/v1",
         "HUNYUAN_API_KEY", "", true, false, false, false},
        {"minimax", "MiniMax", "MINIMAX_BASE_URL", "https://api.minimax.io/v1",
         "MINIMAX_API_KEY", "", true, false, false, false},
        {"qianfan", "Baidu Qianfan", "QIANFAN_BASE_URL", "https://qianfan.baidubce.com/v2",
         "QIANFAN_API_KEY", "", true, false, false, false},
    };
    for (const auto& profile : kProfiles) {
      register_openai_compatible_native_chat_descriptor(profile);
    }
  });
}

bool native_model_declares_capability(const Value& model,
                                      const NativeChatProviderDescriptor* descriptor,
                                      const std::string& capability) {
  if (descriptor) {
    const auto capabilities = descriptor->resolve_capabilities
                                  ? descriptor->resolve_capabilities(model)
                                  : descriptor->default_capabilities;
    if (capabilities.find(capability) != capabilities.end()) {
      return true;
    }
  }
  for (const auto& item : model.at("capabilities").as_array()) {
    if (item.as_string() == capability) {
      return true;
    }
  }
  return false;
}

}  // namespace

void register_native_chat_provider_descriptor(NativeChatProviderDescriptor descriptor) {
  ensure_builtin_native_chat_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_chat_provider_descriptors_mutex());
  put_native_chat_provider_descriptor(std::move(descriptor));
}

std::optional<NativeChatProviderDescriptor> find_native_chat_provider_descriptor(
    const std::string& provider) {
  ensure_builtin_native_chat_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_chat_provider_descriptors_mutex());
  const auto found = native_chat_provider_descriptors().find(provider);
  if (found == native_chat_provider_descriptors().end()) {
    return std::nullopt;
  }
  return found->second;
}

void register_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor descriptor) {
  ensure_builtin_native_embedding_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_text_embedding_provider_descriptors_mutex());
  put_native_text_embedding_provider_descriptor(std::move(descriptor));
}

std::optional<NativeTextEmbeddingProviderDescriptor> find_native_text_embedding_provider_descriptor(
    const std::string& provider) {
  ensure_builtin_native_embedding_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_text_embedding_provider_descriptors_mutex());
  const auto found = native_text_embedding_provider_descriptors().find(provider);
  if (found == native_text_embedding_provider_descriptors().end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<std::string> native_text_embedding_provider_names() {
  ensure_builtin_native_embedding_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_text_embedding_provider_descriptors_mutex());
  std::vector<std::string> names;
  names.reserve(native_text_embedding_provider_descriptors().size());
  for (const auto& [provider, _] : native_text_embedding_provider_descriptors()) {
    names.push_back(provider);
  }
  return names;
}

void register_native_image_embedding_provider_descriptor(NativeImageEmbeddingProviderDescriptor descriptor) {
  ensure_builtin_native_embedding_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_image_embedding_provider_descriptors_mutex());
  put_native_image_embedding_provider_descriptor(std::move(descriptor));
}

std::optional<NativeImageEmbeddingProviderDescriptor> find_native_image_embedding_provider_descriptor(
    const std::string& provider) {
  ensure_builtin_native_embedding_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_image_embedding_provider_descriptors_mutex());
  const auto found = native_image_embedding_provider_descriptors().find(provider);
  if (found == native_image_embedding_provider_descriptors().end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<std::string> native_image_embedding_provider_names() {
  ensure_builtin_native_embedding_provider_descriptors();
  std::lock_guard<std::mutex> lock(native_image_embedding_provider_descriptors_mutex());
  std::vector<std::string> names;
  names.reserve(native_image_embedding_provider_descriptors().size());
  for (const auto& [provider, _] : native_image_embedding_provider_descriptors()) {
    names.push_back(provider);
  }
  return names;
}

ProviderRequestProtocol resolve_native_chat_provider_protocol(const Value& model) {
  if (!model.is_object()) {
    return ProviderRequestProtocol::Unknown;
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return ProviderRequestProtocol::Unknown;
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (!descriptor || !descriptor->resolve_protocol) {
    return ProviderRequestProtocol::Unknown;
  }
  return descriptor->resolve_protocol(model);
}

ProviderRequestProfile resolve_native_chat_provider_profile(const Value& model) {
  if (!model.is_object()) {
    return ProviderRequestProfile{};
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return ProviderRequestProfile{};
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (descriptor && descriptor->resolve_profile) {
    return descriptor->resolve_profile(model);
  }
  const auto requested_reasoning_mode = reasoning_mode_from_model_config(model, "model");
  auto profile = make_provider_request_profile(
      provider,
      model.at("model").as_string(),
      descriptor && descriptor->resolve_protocol
          ? descriptor->resolve_protocol(model)
          : ProviderRequestProtocol::Unknown,
      {},
      requested_reasoning_mode);
  if (!descriptor ||
      requested_reasoning_mode != ProviderReasoningMode::None ||
      native_model_declares_capability(model, descriptor ? &*descriptor : nullptr, "reasoning")) {
    return profile;
  }
  profile.reasoning_mode = ProviderReasoningMode::None;
  profile.reasoning_disable_strategy = ProviderReasoningDisableStrategy::Unknown;
  profile.supports_reasoning = false;
  profile.supports_reasoning_budget = false;
  profile.supports_reasoning_visibility = false;
  return profile;
}

std::shared_ptr<const ProviderReasoningMapper>
resolve_native_chat_provider_reasoning_mapper(const Value& model) {
  if (!model.is_object()) {
    return nullptr;
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return nullptr;
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (!descriptor || !descriptor->resolve_reasoning_mapper) {
    return nullptr;
  }
  return descriptor->resolve_reasoning_mapper(model);
}

NativeChatProviderUsageExtractor resolve_native_chat_provider_usage_extractor(const Value& model) {
  if (!model.is_object()) {
    return {};
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return {};
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  return descriptor ? descriptor->extract_usage : NativeChatProviderUsageExtractor{};
}

NativeChatProviderExtraMapper resolve_native_chat_provider_extra_mapper(const Value& model) {
  if (!model.is_object()) {
    return {};
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return {};
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  return descriptor ? descriptor->apply_extra : NativeChatProviderExtraMapper{};
}

std::optional<NativeChatProviderMediaInputMarshaller>
resolve_native_chat_provider_media_input_marshaller(const Value& model) {
  if (!model.is_object()) {
    return std::nullopt;
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return std::nullopt;
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  return descriptor ? descriptor->media_input_marshaller : std::nullopt;
}

std::optional<NativeChatProviderStreamParserMetadata>
resolve_native_chat_provider_stream_parser_metadata(const Value& model) {
  if (!model.is_object()) {
    return std::nullopt;
  }
  const auto provider = model.at("provider").as_string();
  if (provider.empty()) {
    return std::nullopt;
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (!descriptor || !descriptor->resolve_stream_parser_metadata) {
    return std::nullopt;
  }
  return descriptor->resolve_stream_parser_metadata(model);
}

}  // namespace agent
