#pragma once

#include "agent/core.hpp"
#include "agent/sandbox.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

enum class PermissionAction {
  ToolInvoke,
  ProcessExecute,
  FilesystemRead,
  FilesystemWrite,
  FilesystemDelete,
  NetworkConnect,
  EnvRead,
  StateRead,
  StateWrite,
  MemoryRead,
  KnowledgeRead,
  BrowserRead,
  BrowserScreenshot,
  RepositoryRead,
  RepositoryWrite,
  WorkflowRead,
  WorkflowResume,
  WorkflowRespond,
  WorkflowControl,
  ToolDiscover,
};

enum class PermissionResourceKind {
  Tool,
  Process,
  Filesystem,
  Network,
  Env,
  State,
  Memory,
  Knowledge,
  Browser,
  Repository,
  Workflow,
  Sandbox,
  Unknown,
};

enum class PermissionDecisionSource {
  Default,
  Rule,
  Capability,
  Classifier,
  Approval,
  Skill,
  Host,
};

using PermissionPriority = int;

struct PermissionToolRef {
  std::string name;
  std::vector<std::string> capabilities;
  std::string risk_level = "low";
  std::vector<std::string> tags;
  std::string bundle;
  bool builtin = false;
};

struct PermissionResourceBoundary {
  std::string path;
  std::string path_prefix;
  std::string host;
  std::string command;
};

struct PermissionResource {
  PermissionResourceKind kind = PermissionResourceKind::Unknown;
  std::string id;
  std::vector<PermissionAction> actions;
  PermissionResourceBoundary boundary;
  PermissionDecisionSource source = PermissionDecisionSource::Default;
  PermissionPriority priority = 0;
  Value metadata = Value::object({});
};

struct PermissionResourceMatcher {
  std::vector<PermissionResourceKind> kinds;
  std::vector<std::string> ids;
  std::vector<PermissionAction> actions;
  std::vector<std::string> path_prefixes;
  std::vector<std::string> hosts;
  std::vector<std::string> commands;
};

std::string to_string(PermissionAction action);
PermissionAction permission_action_from_string(std::string_view value,
                                               PermissionAction fallback = PermissionAction::ToolInvoke);

std::string to_string(PermissionResourceKind kind);
PermissionResourceKind permission_resource_kind_from_string(
    std::string_view value,
    PermissionResourceKind fallback = PermissionResourceKind::Unknown);

std::string to_string(PermissionDecisionSource source);
PermissionDecisionSource permission_decision_source_from_string(
    std::string_view value,
    PermissionDecisionSource fallback = PermissionDecisionSource::Default);

bool permission_resource_matches(const PermissionResource& resource,
                                 const PermissionResourceMatcher& matcher);

SandboxRequest sandbox_request_from_permission_resources(
    const std::vector<PermissionResource>& resources,
    Value extensions = Value::object({}));

SandboxRequest merge_sandbox_requests(const SandboxRequest& base,
                                      const SandboxRequest& overlay);

Value permission_resource_to_value(const PermissionResource& resource);
PermissionResource permission_resource_from_value(const Value& value);

}  // namespace agent
