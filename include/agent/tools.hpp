#pragma once

#include "agent/model.hpp"
#include "agent/execution.hpp"
#include "agent/hooks.hpp"
#include "agent/sandbox.hpp"
#include "agent/scratch.hpp"

#include <iosfwd>
#include <mutex>
#include <set>

namespace agent {

class BrowserRenderer;
class DocumentPreprocessorRegistry;
class DocumentRasterizerRegistry;
class KnowledgeBase;
class KnowledgeBaseManager;
class LongTermMemory;
class NativeWebCrawler;
class NativeWebPageFetcher;
class OCRProviderRegistry;
class SessionMemory;
class WebSearchProviderRegistry;
class WorkflowEngine;
struct WorkflowDefinition;

enum class ToolRiskLevel {
  Low,
  Medium,
  High,
};

std::string to_string(ToolRiskLevel risk);
ToolRiskLevel tool_risk_level_from_string(const std::string& value, ToolRiskLevel fallback = ToolRiskLevel::Low);

class ToolRegistry;
struct HookSet;  // forward to avoid header cycle
struct ToolExecutionServices {
  Value values = Value::object({});
  SessionMemory* session = nullptr;
  // Injected by the runner; required by `scratch.*` / `todo.*` builtin tools
  // (and any user tool that wants shared short-term memory). Non-owning.
  ScratchStore* scratch_store = nullptr;
  // Registry the tool was dispatched from. Set by the executor before
  // each invocation; consumed by lazy-discovery tools like `tool.search`
  // and `tool.describe`.
  ToolRegistry* registry = nullptr;
  // Active hook set (set by the executor). Lets tools that want to fire
  // domain-specific hooks (e.g. fs.writeText's `before_fs_write`) reach them
  // without threading another argument through every signature.
  const HookSet* hooks = nullptr;
  LongTermMemory* long_term_memory = nullptr;
  KnowledgeBase* knowledge_base = nullptr;
  KnowledgeBaseManager* knowledge_base_manager = nullptr;
  WorkflowEngine* workflow_engine = nullptr;
  const WorkflowDefinition* workflow_definition = nullptr;
  WebSearchProviderRegistry* web_search_registry = nullptr;
  std::string default_search_provider;
  NativeWebPageFetcher* web_fetcher = nullptr;
  NativeWebCrawler* web_crawler = nullptr;
  Value default_crawler_profile = Value::object({});
  BrowserRenderer* browser_renderer = nullptr;
  DefaultMediaResolver::ArtifactLookup artifact_lookup;
  OCRProviderRegistry* ocr_registry = nullptr;
  std::string default_ocr_provider;
  DocumentRasterizerRegistry* document_rasterizers = nullptr;
  DocumentPreprocessorRegistry* document_preprocessors = nullptr;
  // Active sandbox the executor will enforce before each tool invocation.
  // Non-owning pointer; lifetime managed by the host. nullptr means "no
  // sandbox installed" — tools whose policy demands non-empty capabilities
  // will refuse to run in that case.
  Sandbox* sandbox = nullptr;
  // Set by the executor for the duration of `invoke()` after a successful
  // `enter()`. Lets tools (notably shell.exec) read the active request to
  // pass allowlists / capability axes through to a downstream process
  // executor without rethreading arguments. Non-owning; cleared by the
  // executor before the underlying scope is destroyed.
  SandboxScope* sandbox_scope = nullptr;
};

struct ToolExecutionContext {
  Value services = Value::object({});
  ToolExecutionServices service_refs;
  Value attributes = Value::object({});
  std::optional<ToolCall> tool_call;
  int iteration = 0;
  TraceContext trace_context;
  CancellationToken* cancellation = nullptr;
};

Value merge_tool_service_values(const Value& base, const Value& overlay);
ToolExecutionServices merge_tool_execution_services(const ToolExecutionServices& base,
                                                    const ToolExecutionServices& overlay);
ToolExecutionContext with_tool_execution_services(ToolExecutionContext context,
                                                  const ToolExecutionServices& configured_services,
                                                  const ToolExecutionServices& runtime_services = {});
ToolExecutionContext normalize_tool_execution_context(ToolExecutionContext context);

struct ToolResultEnvelope {
  std::optional<std::vector<MessageContentPart>> content;
  std::optional<Value> value;
  Value metadata = Value::object({});
};

using ToolInvokeResult = std::variant<Value, ToolResultEnvelope>;
using ToolHandler = std::function<ToolInvokeResult(const Value&, ToolExecutionContext&)>;

struct ToolDefinition {
  std::string name;
  std::string description;
  JsonSchema input_schema;
  std::vector<std::string> capabilities;
  ToolRiskLevel risk_level = ToolRiskLevel::Low;
  std::vector<std::string> tags;
  std::string bundle;
  bool builtin = false;
  // Sandbox capabilities this tool requires (and optionally prefers). The
  // executor calls `enforce_sandbox_contract(sandbox_policy,
  // ctx.service_refs.sandbox, event_bus)` before dispatch; contract
  // violations throw `SandboxContractError`.
  ToolSandboxPolicy sandbox_policy;
  ToolHandler execute;

  [[nodiscard]] Value validate_input(const Value& input) const;
  ToolInvokeResult invoke(const Value& input, ToolExecutionContext& context) const;
  [[nodiscard]] ChatToolDescriptor descriptor() const;
};

ToolDefinition define_tool(ToolDefinition tool);

class ToolRegistry {
 public:
  explicit ToolRegistry(std::vector<ToolDefinition> tools = {});
  ToolRegistry(const ToolRegistry& other);
  ToolRegistry& operator=(const ToolRegistry& other);
  ToolRegistry(ToolRegistry&& other) noexcept;
  ToolRegistry& operator=(ToolRegistry&& other) noexcept;
  ToolDefinition& register_tool(ToolDefinition tool);
  [[nodiscard]] const ToolDefinition* get(const std::string& name) const;
  [[nodiscard]] std::optional<ToolDefinition> find(const std::string& name) const;
  // Returns visible tools (respects lazy_mode). Use `list_all()` to ignore
  // visibility filtering — primarily useful for `tool.search` / `tool.describe`.
  [[nodiscard]] std::vector<ToolDefinition> list() const;
  [[nodiscard]] std::vector<ToolDefinition> list_all() const;
  [[nodiscard]] std::vector<ChatToolDescriptor> descriptors() const;

  // Lazy tool discovery. When `lazy_mode == true`, `list()` / `descriptors()`
  // advertise only `tool.*` tools plus any names added via `force_visible()`.
  // Hidden tools remain fully executable when invoked by name.
  void set_lazy_mode(bool lazy) noexcept;
  void force_visible(const std::string& name);
  [[nodiscard]] bool is_visible(const std::string& name) const;

  // Public field as called out by the API brief; prefer the setter when
  // toggling live registries to keep the mutex involvement explicit.
  bool lazy_mode = false;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ToolDefinition> tools_;
  std::set<std::string> forced_visible_;
};

enum class PermissionDecisionKind {
  Allow,
  Deny,
  Ask,
};

std::string to_string(PermissionDecisionKind decision);
PermissionDecisionKind permission_decision_kind_from_string(
    const std::string& value,
    PermissionDecisionKind fallback = PermissionDecisionKind::Deny);

struct PermissionDecision {
  PermissionDecisionKind decision = PermissionDecisionKind::Allow;
  std::string reason;
};

struct PermissionRequest {
  std::string tool_name;
  std::vector<std::string> capabilities;
  ToolRiskLevel risk_level = ToolRiskLevel::Low;
  std::vector<std::string> tags;
  std::string bundle;
  bool builtin = false;
  ToolCall tool_call;
  ToolExecutionContext context;
};

using PermissionPolicy = std::function<PermissionDecision(const PermissionRequest&)>;
using PermissionApprovalHandler = std::function<PermissionDecision(const PermissionRequest&, const PermissionDecision&)>;

struct ToolPermissionMatcher {
  std::vector<std::string> tool_names;
  std::vector<std::string> capabilities;
  std::vector<std::string> tags;
  std::vector<ToolRiskLevel> risk_levels;
  std::optional<bool> builtin;
  std::vector<std::string> bundles;
};

struct ToolPermissionRule {
  ToolPermissionMatcher match;
  PermissionDecisionKind decision = PermissionDecisionKind::Deny;
  std::string reason;
};

struct RuleBasedPermissionPolicyConfig {
  std::vector<ToolPermissionRule> rules;
  PermissionDecisionKind default_decision = PermissionDecisionKind::Deny;
  std::string default_reason;
};

struct CapabilityPermissionPolicyConfig {
  std::vector<std::string> allow;
  std::vector<std::string> deny;
  std::vector<std::string> ask;
  PermissionDecisionKind high_risk_mode = PermissionDecisionKind::Ask;
};

struct CliApprovalHandlerConfig {
  std::istream* input = nullptr;
  std::ostream* output = nullptr;
  PermissionDecisionKind default_decision = PermissionDecisionKind::Deny;
  std::function<std::string(const PermissionRequest&, const PermissionDecision&)> prompt_formatter;
};

struct MemoryApprovalRecord {
  PermissionRequest request;
  PermissionDecision decision;
};

class MemoryApprovalRecorder {
 public:
  explicit MemoryApprovalRecorder(PermissionDecision response = {});
  PermissionDecision approve(const PermissionRequest& request, const PermissionDecision& decision);
  PermissionApprovalHandler handler();
  [[nodiscard]] const std::vector<MemoryApprovalRecord>& records() const noexcept;

 private:
  PermissionDecision response_;
  std::vector<MemoryApprovalRecord> records_;
};

PermissionPolicy create_rule_based_permission_policy(RuleBasedPermissionPolicyConfig config = {});
PermissionPolicy create_capability_policy(CapabilityPermissionPolicyConfig config = {});
PermissionApprovalHandler create_static_approval_handler(PermissionDecision decision = {});
PermissionApprovalHandler create_cli_approval_handler(CliApprovalHandlerConfig config = {});

struct ToolExecutionResult {
  ToolCall tool_call;
  bool ok = false;
  std::optional<ToolInvokeResult> result;
  std::string error;
  std::string output;
  AgentMessage message;
};

class ToolExecutor {
 public:
  ToolExecutor(ToolRegistry& registry, PermissionPolicy permission_policy = {},
               PermissionApprovalHandler approval_handler = {}, EventBus* event_bus = nullptr,
               ExecutionPolicies execution_policies = {}, HookSet hooks = {});

  ToolExecutionResult execute_tool_call(const ToolCall& tool_call, ToolExecutionContext context = {});
  std::vector<ToolExecutionResult> execute_all(const std::vector<ToolCall>& tool_calls,
                                               ToolExecutionContext context = {});

 private:
  ToolRegistry& registry_;
  PermissionPolicy permission_policy_;
  PermissionApprovalHandler approval_handler_;
  EventBus* event_bus_;
  ExecutionPolicies execution_policies_;
  HookSet hooks_;
};

}  // namespace agent
