#pragma once

#include "agent/model.hpp"
#include "agent/execution.hpp"
#include "agent/hooks.hpp"
#include "agent/sandbox.hpp"
#include "agent/scratch.hpp"
#include "agent/security_governance.hpp"

#include <cstdint>
#include <iosfwd>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <vector>

namespace agent {

class KnowledgeContextProvider;
class LongTermMemoryPort;
class SessionMemory;
class ToolRunManager;

enum class ToolRiskLevel {
  Low,
  Medium,
  High,
};

std::string to_string(ToolRiskLevel risk);
ToolRiskLevel tool_risk_level_from_string(const std::string& value, ToolRiskLevel fallback = ToolRiskLevel::Low);

class ToolRegistry;
struct HookSet;  // forward to avoid header cycle

struct ToolServiceRequirement {
  std::string name;
  bool optional = false;
};

template <typename T>
struct ToolServiceToken {
  std::string name;
};

template <typename T>
inline ToolServiceRequirement tool_service_requirement(const ToolServiceToken<T>& token,
                                                       bool optional = false) {
  return ToolServiceRequirement{.name = token.name, .optional = optional};
}

inline const ToolServiceToken<SessionMemory> kToolServiceSessionMemory{"memory.session"};
inline const ToolServiceToken<ScratchStore> kToolServiceScratchStore{"state.scratch"};
inline const ToolServiceToken<ToolRegistry> kToolServiceToolRegistry{"tool.registry"};
inline const ToolServiceToken<const HookSet> kToolServiceHooks{"tool.hooks"};
inline const ToolServiceToken<LongTermMemoryPort> kToolServiceLongTermMemory{"memory.long_term"};
inline const ToolServiceToken<KnowledgeContextProvider> kToolServiceKnowledgeProvider{"knowledge.provider"};
inline const ToolServiceToken<ToolRunManager> kToolServiceToolRunManager{"tool_runs.manager"};
inline const ToolServiceToken<Sandbox> kToolServiceSandbox{"sandbox"};
inline const ToolServiceToken<SandboxScope> kToolServiceSandboxScope{"sandbox.scope"};

struct ToolServiceBinding {
  void* pointer = nullptr;
  std::string type_name;
};

class ToolServiceView {
 public:
  [[nodiscard]] bool has(const std::string& name) const;
  [[nodiscard]] std::vector<std::string> names() const;

  template <typename T>
  [[nodiscard]] T* get(const ToolServiceToken<T>& token) const {
    const auto found = services_.find(token.name);
    if (found == services_.end() || found->second.type_name != typeid(T*).name()) {
      return nullptr;
    }
    return static_cast<T*>(found->second.pointer);
  }

  template <typename T>
  T& require(const ToolServiceToken<T>& token) const {
    auto* service = get(token);
    if (!service) {
      throw ConfigurationError("Tool service \"" + token.name + "\" is required but was not provided.");
    }
    return *service;
  }

 private:
  friend class ToolServiceContainer;
  std::map<std::string, ToolServiceBinding> services_;
};

class ToolServiceContainer {
 public:
  [[nodiscard]] bool has(const std::string& name) const;
  [[nodiscard]] ToolServiceView view(const std::vector<ToolServiceRequirement>& requirements) const;
  void merge_from(const ToolServiceContainer& other);

  template <typename T>
  void set(const ToolServiceToken<T>& token, T* service) {
    if (token.name.empty()) {
      throw ConfigurationError("Tool service token name is required.");
    }
    if (!service) {
      services_.erase(token.name);
      return;
    }
    services_[token.name] = ToolServiceBinding{
        .pointer = const_cast<void*>(static_cast<const void*>(service)),
        .type_name = typeid(T*).name(),
    };
  }

  template <typename T, typename U,
            typename = std::enable_if_t<!std::is_same_v<T, U> && std::is_convertible_v<U*, T*>>>
  void set(const ToolServiceToken<T>& token, U* service) {
    set(token, static_cast<T*>(service));
  }

  template <typename T>
  [[nodiscard]] T* get(const ToolServiceToken<T>& token) const {
    return view({ToolServiceRequirement{.name = token.name, .optional = true}}).get(token);
  }

 private:
  std::map<std::string, ToolServiceBinding> services_;
};

struct ToolExecutionServices {
  Value values = Value::object({});
  ToolServiceContainer service_container;
  ToolServiceView service_view;
};

struct ToolProgressEvent {
  std::string kind;
  std::string tool_call_id;
  std::string tool_name;
  int iteration = 0;
  std::uint64_t sequence = 0;
  Value payload = Value::object({});
  TraceContext trace;
};

using ToolProgressEmitter = std::function<void(ToolProgressEvent)>;

struct ToolExecutionContext {
  Value services = Value::object({});
  ToolExecutionServices service_refs;
  Value attributes = Value::object({});
  std::optional<ToolCall> tool_call;
  int iteration = 0;
  TraceContext trace_context;
  CancellationToken* cancellation = nullptr;
  ToolProgressEmitter emit_progress;
};

Value merge_tool_service_values(const Value& base, const Value& overlay);
ToolExecutionServices merge_tool_execution_services(const ToolExecutionServices& base,
                                                    const ToolExecutionServices& overlay);
ToolExecutionContext with_tool_execution_services(ToolExecutionContext context,
                                                  const ToolExecutionServices& configured_services,
                                                  const ToolExecutionServices& runtime_services = {});
ToolExecutionContext normalize_tool_execution_context(ToolExecutionContext context);
void emit_tool_progress(ToolExecutionContext& context,
                        std::string kind,
                        Value payload = Value::object({}));

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
  std::vector<ToolServiceRequirement> service_requirements;
  ToolRiskLevel risk_level = ToolRiskLevel::Low;
  std::vector<std::string> tags;
  std::string bundle;
  bool builtin = false;
  bool read_only = false;
  bool mutates_files = false;
  bool interactive = false;
  bool long_running = false;
  bool batchable = false;
  std::string concurrency_key;
  std::string side_effect_level = "unknown";
  // Sandbox capabilities this tool requires (and optionally prefers). The
  // executor calls `enforce_sandbox_contract(sandbox_policy,
  // ctx.service_refs.service_container, event_bus)` before dispatch; contract
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
  PermissionDecisionSource source = PermissionDecisionSource::Default;
  std::string rule_id;
  PermissionPriority priority = 0;
  std::vector<PermissionResource> resources;
  std::optional<SandboxRequest> sandbox_request;
};

struct PermissionRequest {
  PermissionToolRef tool;
  std::string tool_name;
  std::vector<std::string> capabilities;
  ToolRiskLevel risk_level = ToolRiskLevel::Low;
  std::vector<std::string> tags;
  std::string bundle;
  bool builtin = false;
  std::vector<PermissionAction> actions;
  std::vector<PermissionResource> resources;
  PermissionDecisionSource source = PermissionDecisionSource::Default;
  PermissionPriority priority = 0;
  ToolCall tool_call;
  ToolExecutionContext context;
};

using PermissionPolicy = std::function<PermissionDecision(const PermissionRequest&)>;
using PermissionApprovalHandler = std::function<PermissionDecision(const PermissionRequest&, const PermissionDecision&)>;

struct ToolPermissionMatcher {
  std::vector<std::string> tool_names;
  std::vector<std::string> capabilities;
  std::vector<PermissionAction> actions;
  std::vector<PermissionResourceMatcher> resource_matchers;
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

struct FineGrainedPermissionMatcher {
  std::vector<std::string> tool_names;
  std::vector<std::string> capabilities;
  std::vector<PermissionAction> actions;
  std::vector<PermissionResourceMatcher> resource_matchers;
  std::vector<std::string> tags;
  std::vector<ToolRiskLevel> risk_levels;
  std::optional<bool> builtin;
  std::vector<std::string> bundles;
};

struct FineGrainedPermissionRule {
  std::string id;
  std::string description;
  PermissionPriority priority = 0;
  PermissionDecisionSource source = PermissionDecisionSource::Rule;
  FineGrainedPermissionMatcher match;
  PermissionDecisionKind decision = PermissionDecisionKind::Deny;
  std::string reason;
  std::vector<PermissionResource> resources;
  std::optional<SandboxRequest> sandbox_request;
};

struct FineGrainedPermissionPolicyConfig {
  std::vector<FineGrainedPermissionRule> rules;
  PermissionDecisionKind default_decision = PermissionDecisionKind::Deny;
  std::string default_reason;
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
PermissionPolicy create_fine_grained_permission_policy(FineGrainedPermissionPolicyConfig config = {});
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
