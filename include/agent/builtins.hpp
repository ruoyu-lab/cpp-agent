#pragma once

#include "agent/browser.hpp"
#include "agent/tools.hpp"
#include "agent/tool_services_io.hpp"
#include "agent/tool_services_modules.hpp"
#include "agent/http.hpp"
#include "agent/media.hpp"
#include "agent/media_generation.hpp"
#include "agent/web.hpp"
#include "agent/knowledge_runtime.hpp"
#include "agent/memory_session.hpp"
#include "agent/workflow.hpp"

#include <mutex>
#include <optional>

namespace agent {

struct DeveloperProcessRequest {
  std::string command;
  std::vector<std::string> args;
  std::string cwd;
  CancellationToken* cancellation = nullptr;
  ToolProgressEmitter emit_progress;
  // Effective sandbox request the executor MUST honor when running this
  // process. Set by the framework before dispatch by reading the active
  // `SandboxScope::request()`. Business-layer executors translate the
  // capability axes and allowlists into the matching platform mechanism
  // (Seatbelt / seccomp + Landlock / AppContainer / namespaces / …).
  SandboxRequest sandbox_request;
};

struct DeveloperProcessResult {
  std::string stdout_text;
  std::string stderr_text;
  int exit_code = 0;
};

using DeveloperProcessExecutor = std::function<DeveloperProcessResult(const DeveloperProcessRequest&)>;

struct ToolBundleMetadata {
  std::string name;
  std::string tier = "core-safe";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
  std::vector<std::string> capabilities;
  std::string default_risk_profile = "mixed";
};

struct ToolBundleProvider {
  ToolBundleMetadata metadata;
  std::function<std::vector<ToolDefinition>()> create_tools;
};

class ToolBundleRegistry {
 public:
  explicit ToolBundleRegistry(std::vector<ToolBundleProvider> providers = {});
  ToolBundleRegistry(const ToolBundleRegistry& other);
  ToolBundleRegistry& operator=(const ToolBundleRegistry& other);
  ToolBundleRegistry(ToolBundleRegistry&& other) noexcept;
  ToolBundleRegistry& operator=(ToolBundleRegistry&& other) noexcept;
  ToolBundleProvider& register_provider(ToolBundleProvider provider);
  [[nodiscard]] const ToolBundleProvider* get(const std::string& name) const;
  [[nodiscard]] std::optional<ToolBundleProvider> find(const std::string& name) const;
  [[nodiscard]] std::vector<ToolBundleProvider> list() const;
  [[nodiscard]] std::vector<ToolDefinition> create_tools(std::vector<std::string> bundles = {}) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ToolBundleProvider> providers_;
  std::vector<std::string> provider_order_;
};

std::vector<ToolDefinition> create_core_builtin_tools();
// The `fs.*` tools are confined by default: each resolves its `path` argument
// and refuses to operate outside the allowed roots (defense against
// path-traversal / absolute-path escapes from model input). With the defaults
// below, confinement is the process working directory. Pass explicit
// `allowed_roots` to widen, or `allow_unconfined = true` to disable confinement
// entirely (an explicit escape hatch).
std::vector<ToolDefinition> create_local_builtin_tools(std::vector<std::string> allowed_roots = {},
                                                       bool allow_unconfined = false);
std::vector<ToolDefinition> create_http_builtin_tools(HttpTransport transport = {});
std::vector<ToolDefinition> create_developer_builtin_tools(DeveloperProcessExecutor executor = {});
std::vector<ToolDefinition> create_browser_builtin_tools(BrowserRenderer* renderer = nullptr);
std::vector<ToolDefinition> create_web_builtin_tools(WebSearchProviderRegistry* search_registry = nullptr,
                                                     std::string default_search_provider = {},
                                                     NativeWebPageFetcher* fetcher = nullptr);
std::vector<ToolDefinition> create_agent_builtin_tools(SessionMemory* session = nullptr,
                                                       KnowledgeContextProvider* knowledge = nullptr);
std::vector<ToolDefinition> create_workflow_builtin_tools(WorkflowEngine* engine = nullptr);
std::vector<ToolDefinition> create_state_builtin_tools();
std::vector<ToolDefinition> create_media_generation_builtin_tools(
    MediaGenerationProviderRegistry* registry = nullptr,
    MediaGenerationToolOptions options = {});
std::vector<ToolBundleProvider> create_builtin_tool_bundle_providers();
ToolBundleRegistry create_builtin_tool_bundle_registry();
std::vector<ToolBundleMetadata> list_builtin_tool_bundle_metadata();
std::vector<ToolDefinition> create_builtin_tools(std::vector<std::string> bundles = {"core"});

}  // namespace agent
