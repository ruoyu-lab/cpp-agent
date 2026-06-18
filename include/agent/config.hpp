#pragma once

#include "agent/browser.hpp"
#include "agent/builtins.hpp"
#include "agent/knowledge_core.hpp"
#include "agent/mcp.hpp"
#include "agent/model_providers.hpp"
#include "agent/providers/reasoning.hpp"
#include "agent/runtime.hpp"
#include "agent/tools.hpp"
#include "agent/web.hpp"

#include <set>

namespace agent {

using NativeMCPTransportFactory =
    std::function<std::shared_ptr<MCPTransport>(const AnthropicMcpServerConfig&)>;
using NativeConfigModuleLoader = std::function<Value(const std::filesystem::path&)>;
using NativeSessionMemoryConfigurator =
    std::function<void(SessionMemoryOptions&, const Value& session_store_config)>;

struct NativeLoadedAgentConfig {
  Value config = Value::object({});
  std::filesystem::path cwd;
  std::filesystem::path path;
};

struct NativeWorkflowRuntime {
  std::shared_ptr<WorkflowStore> store;
  std::shared_ptr<ToolRegistry> tools;
  std::shared_ptr<ToolExecutor> tool_executor;
  std::shared_ptr<WorkflowEngine> engine;
};

struct NativeWebAdapters {
  NativeWebSearchTransport search_transport;
  NativeWebFetchTransport fetch_transport;
};

struct NativeDeveloperAdapters {
  DeveloperProcessExecutor process_executor;
};

struct NativeBrowserAdapters {
  std::shared_ptr<BrowserRenderer> renderer;
};

struct NativeLlamaCppAdapters {
  std::shared_ptr<LlamaCppNativeBinding> binding;
};

struct NativeChatProviderBuildContext {
  NativeProviderTransport transport;
  NativeProviderStreamTransport stream_transport;
  NativeProviderStreamingTransport streaming_transport;
  NativeLlamaCppAdapters llama_adapters;
};

struct NativeEmbeddingProviderBuildContext {
  NativeProviderTransport transport;
  NativeLlamaCppAdapters llama_adapters;
};

using NativeChatProviderAdapterFactory =
    std::function<std::shared_ptr<ChatModelAdapter>(
        const Value& model,
        NativeChatProviderBuildContext context)>;
using NativeChatProviderProtocolResolver =
    std::function<ProviderRequestProtocol(const Value& model)>;
using NativeChatProviderProfileResolver =
    std::function<ProviderRequestProfile(const Value& model)>;
using NativeChatProviderCapabilitiesResolver =
    std::function<std::set<std::string>(const Value& model)>;
using NativeChatProviderEnvRefCollector =
    std::function<std::vector<std::string>(const Value& model)>;
using NativeChatProviderModelValidator =
    std::function<void(const Value& model, const std::string& path)>;
using NativeChatProviderReasoningMapperResolver =
    std::function<std::shared_ptr<const ProviderReasoningMapper>(const Value& model)>;

struct NativeProviderUsageExtractionContext {
  std::string provider;
  std::string model;
  const AgentOutput* response = nullptr;
  const Value* raw = nullptr;
};

using NativeChatProviderUsageExtractor =
    std::function<ModelUsage(const NativeProviderUsageExtractionContext& context)>;
using NativeChatProviderExtraMapper =
    std::function<void(NativeProviderRequest& request,
                       const Value& model,
                       const ModelSettings& settings)>;

struct NativeChatProviderMediaInputMarshaller {
  std::string name;
  std::set<ContentPartType> supported_parts;
  std::set<MediaSourceKind> accepted_sources;
  bool requires_resolver_for_non_inline = false;
};

enum class NativeProviderReasoningParser {
  None,
  Native,
  Tagged,
};

struct NativeChatProviderStreamParserMetadata {
  std::string parser;
  ModelStreamFrameBoundary frame_boundary = ModelStreamFrameBoundary::None;
  std::set<ModelStreamEventType> events;
  bool includes_terminal_usage = false;
  NativeProviderReasoningParser reasoning_parser = NativeProviderReasoningParser::None;
};

using NativeChatProviderStreamParserMetadataResolver =
    std::function<NativeChatProviderStreamParserMetadata(const Value& model)>;

using NativeTextEmbeddingProviderAdapterFactory =
    std::function<std::shared_ptr<TextEmbeddingAdapter>(
        const Value& embedding,
        NativeEmbeddingProviderBuildContext context)>;
using NativeImageEmbeddingProviderAdapterFactory =
    std::function<std::shared_ptr<ImageEmbeddingAdapter>(const Value& embedding)>;
using NativeEmbeddingProviderEnvRefCollector =
    std::function<std::vector<std::string>(const Value& embedding)>;
using NativeEmbeddingProviderDimensionsResolver =
    std::function<std::optional<int>(const Value& embedding)>;

struct NativeChatProviderDescriptor {
  std::string provider;
  NativeChatProviderAdapterFactory create_adapter;
  NativeChatProviderProtocolResolver resolve_protocol;
  NativeChatProviderProfileResolver resolve_profile;
  std::set<std::string> default_capabilities;
  NativeChatProviderCapabilitiesResolver resolve_capabilities;
  NativeChatProviderEnvRefCollector collect_env_refs;
  NativeChatProviderModelValidator validate_model;
  NativeChatProviderReasoningMapperResolver resolve_reasoning_mapper;
  NativeChatProviderUsageExtractor extract_usage;
  NativeChatProviderExtraMapper apply_extra;
  std::optional<NativeChatProviderMediaInputMarshaller> media_input_marshaller;
  NativeChatProviderStreamParserMetadataResolver resolve_stream_parser_metadata;
};

struct NativeTextEmbeddingProviderDescriptor {
  std::string provider;
  NativeTextEmbeddingProviderAdapterFactory create_adapter;
  NativeEmbeddingProviderEnvRefCollector collect_env_refs;
  NativeEmbeddingProviderDimensionsResolver resolve_known_dimensions;
};

struct NativeImageEmbeddingProviderDescriptor {
  std::string provider;
  NativeImageEmbeddingProviderAdapterFactory create_adapter;
  NativeEmbeddingProviderEnvRefCollector collect_env_refs;
  NativeEmbeddingProviderDimensionsResolver resolve_known_dimensions;
};

void register_native_chat_provider_descriptor(NativeChatProviderDescriptor descriptor);
[[nodiscard]] std::optional<NativeChatProviderDescriptor> find_native_chat_provider_descriptor(
    const std::string& provider);
void register_native_text_embedding_provider_descriptor(NativeTextEmbeddingProviderDescriptor descriptor);
[[nodiscard]] std::optional<NativeTextEmbeddingProviderDescriptor>
find_native_text_embedding_provider_descriptor(const std::string& provider);
[[nodiscard]] std::vector<std::string> native_text_embedding_provider_names();
void register_native_image_embedding_provider_descriptor(NativeImageEmbeddingProviderDescriptor descriptor);
[[nodiscard]] std::optional<NativeImageEmbeddingProviderDescriptor>
find_native_image_embedding_provider_descriptor(const std::string& provider);
[[nodiscard]] std::vector<std::string> native_image_embedding_provider_names();
[[nodiscard]] ProviderRequestProtocol resolve_native_chat_provider_protocol(
    const Value& model);
[[nodiscard]] ProviderRequestProfile resolve_native_chat_provider_profile(
    const Value& model);
[[nodiscard]] std::shared_ptr<const ProviderReasoningMapper>
resolve_native_chat_provider_reasoning_mapper(
    const Value& model);
[[nodiscard]] NativeChatProviderUsageExtractor resolve_native_chat_provider_usage_extractor(
    const Value& model);
[[nodiscard]] NativeChatProviderExtraMapper resolve_native_chat_provider_extra_mapper(
    const Value& model);
[[nodiscard]] std::optional<NativeChatProviderMediaInputMarshaller>
resolve_native_chat_provider_media_input_marshaller(const Value& model);
[[nodiscard]] std::optional<NativeChatProviderStreamParserMetadata>
resolve_native_chat_provider_stream_parser_metadata(const Value& model);

struct NativeAgentAppResolveOptions {
  std::string requested_agent_id;
  NativeProviderTransport provider_transport;
  NativeProviderStreamTransport provider_stream_transport;
  NativeProviderStreamingTransport provider_streaming_transport;
  NativeMCPTransportFactory mcp_transport_factory;
  NativeWebAdapters web_adapters;
  NativeDeveloperAdapters developer_adapters;
  NativeBrowserAdapters browser_adapters;
  NativeLlamaCppAdapters llama_adapters;
  NativeSessionMemoryConfigurator configure_session_memory;
};

struct NativeWebRuntime {
  std::shared_ptr<WebSearchProviderRegistry> search_registry;
  std::shared_ptr<NativeWebPageFetcher> fetcher;
  std::shared_ptr<NativeWebCrawler> crawler;
  std::string default_search_provider;
  Value default_crawler_profile = Value::object({});
};

struct NativeBrowserRuntime {
  std::shared_ptr<BrowserRenderer> renderer;
};

struct NativeKnowledgeRuntime {
  std::shared_ptr<KnowledgeSourceLoader> loader;
  std::shared_ptr<KnowledgeBase> base;
  std::shared_ptr<KnowledgeBaseManager> manager;
};

struct NativeResolvedAgentApp {
  std::string agent_id;
  std::shared_ptr<AgentRunner> runner;
  std::vector<ToolDefinition> tools;
  std::shared_ptr<SkillRegistry> skills;
  std::vector<std::shared_ptr<MCPClient>> mcp_clients;
  std::shared_ptr<NativeWorkflowRuntime> workflow;
  std::shared_ptr<NativeWebRuntime> web;
  std::shared_ptr<NativeBrowserRuntime> browser;
  std::shared_ptr<NativeKnowledgeRuntime> knowledge;
};

Value define_native_agent_config(Value config);
NativeLoadedAgentConfig define_native_loaded_agent_config(Value config,
                                                          std::filesystem::path cwd = {},
                                                          std::filesystem::path path = {});
std::string resolve_agent_id(const Value& config, std::string requested_agent_id = {});
std::string resolve_agent_id(const NativeLoadedAgentConfig& loaded_config,
                             std::string requested_agent_id = {});
Value resolve_agent_definition(const Value& config, std::string requested_agent_id = {});
Value resolve_agent_definition(const NativeLoadedAgentConfig& loaded_config,
                               std::string requested_agent_id = {});
void validate_native_agent_config(const Value& config);
void validate_native_agent_config(const NativeLoadedAgentConfig& loaded_config);
std::vector<std::string> collect_referenced_env_keys(const Value& config);
std::vector<std::string> collect_referenced_env_keys(const NativeLoadedAgentConfig& loaded_config);
std::vector<std::string> assert_referenced_env_keys(
    const Value& config,
    std::optional<std::map<std::string, std::string>> env = std::nullopt);
std::vector<std::string> assert_referenced_env_keys(
    const NativeLoadedAgentConfig& loaded_config,
    std::optional<std::map<std::string, std::string>> env = std::nullopt);
std::optional<std::filesystem::path> find_native_agent_config_file(std::filesystem::path cwd = {});
Value load_native_agent_config(const std::filesystem::path& config_path,
                               NativeConfigModuleLoader config_module_loader = {});
NativeLoadedAgentConfig load_native_loaded_agent_config(const std::filesystem::path& config_path,
                                                        NativeConfigModuleLoader config_module_loader = {});
NativeResolvedAgentApp resolve_native_agent_app(const Value& config, std::string requested_agent_id);
NativeResolvedAgentApp resolve_native_agent_app(const Value& config,
                                                NativeAgentAppResolveOptions options = {});
NativeResolvedAgentApp resolve_native_agent_app(NativeLoadedAgentConfig loaded_config,
                                                std::string requested_agent_id);
NativeResolvedAgentApp resolve_native_agent_app(NativeLoadedAgentConfig loaded_config,
                                                NativeAgentAppResolveOptions options = {});
NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             NativeConfigModuleLoader config_module_loader);
NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             std::string requested_agent_id,
                                             NativeConfigModuleLoader config_module_loader = {});
NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             NativeAgentAppResolveOptions options = {},
                                             NativeConfigModuleLoader config_module_loader = {});

}  // namespace agent
