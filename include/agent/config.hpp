#pragma once

#include "agent/browser.hpp"
#include "agent/builtins.hpp"
#include "agent/mcp.hpp"
#include "agent/runtime.hpp"
#include "agent/tools.hpp"
#include "agent/web.hpp"

namespace agent {

using NativeMCPTransportFactory =
    std::function<std::shared_ptr<MCPTransport>(const AnthropicMcpServerConfig&)>;
using NativeConfigModuleLoader = std::function<Value(const std::filesystem::path&)>;

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
NativeResolvedAgentApp resolve_native_agent_app(const Value& config, std::string requested_agent_id = {},
                                                NativeProviderTransport provider_transport = {},
                                                NativeMCPTransportFactory mcp_transport_factory = {},
                                                NativeWebAdapters web_adapters = {},
                                                NativeDeveloperAdapters developer_adapters = {},
                                                NativeBrowserAdapters browser_adapters = {},
                                                NativeProviderStreamTransport provider_stream_transport = {},
                                                NativeLlamaCppAdapters llama_adapters = {});
NativeResolvedAgentApp resolve_native_agent_app(NativeLoadedAgentConfig loaded_config,
                                                std::string requested_agent_id = {},
                                                NativeProviderTransport provider_transport = {},
                                                NativeMCPTransportFactory mcp_transport_factory = {},
                                                NativeWebAdapters web_adapters = {},
                                                NativeDeveloperAdapters developer_adapters = {},
                                                NativeBrowserAdapters browser_adapters = {},
                                                NativeProviderStreamTransport provider_stream_transport = {},
                                                NativeLlamaCppAdapters llama_adapters = {});
NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             NativeConfigModuleLoader config_module_loader);
NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             std::string requested_agent_id,
                                             NativeConfigModuleLoader config_module_loader);
NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             std::string requested_agent_id = {},
                                             NativeProviderTransport provider_transport = {},
                                             NativeMCPTransportFactory mcp_transport_factory = {},
                                             NativeWebAdapters web_adapters = {},
                                             NativeDeveloperAdapters developer_adapters = {},
                                             NativeBrowserAdapters browser_adapters = {},
                                             NativeProviderStreamTransport provider_stream_transport = {},
                                             NativeLlamaCppAdapters llama_adapters = {},
                                             NativeConfigModuleLoader config_module_loader = {});

}  // namespace agent
