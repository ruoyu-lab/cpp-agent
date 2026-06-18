#include "agent/security_governance.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <string>

namespace agent {

namespace {

std::string lower(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return out;
}

bool wildcard_match(std::string pattern, std::string text) {
  pattern = lower(pattern);
  text = lower(text);
  std::size_t pattern_index = 0;
  std::size_t text_index = 0;
  std::size_t star_index = std::string::npos;
  std::size_t match_index = 0;

  while (text_index < text.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == text[text_index]) {
      ++pattern_index;
      ++text_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      match_index = text_index;
      continue;
    }
    if (star_index != std::string::npos) {
      pattern_index = star_index + 1;
      text_index = ++match_index;
      continue;
    }
    return false;
  }
  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

template <typename T>
bool contains(const std::vector<T>& values, const T& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool matches_string(const std::string& actual, const std::vector<std::string>& expected) {
  if (expected.empty()) {
    return true;
  }
  if (actual.empty()) {
    return false;
  }
  return std::any_of(expected.begin(), expected.end(), [&](const std::string& value) {
    return wildcard_match(value, actual);
  });
}

template <typename T>
bool matches_enum(const T& actual, const std::vector<T>& expected) {
  return expected.empty() || contains(expected, actual);
}

bool matches_any_action(const std::vector<PermissionAction>& actual,
                        const std::vector<PermissionAction>& expected) {
  if (expected.empty()) {
    return true;
  }
  return std::any_of(expected.begin(), expected.end(), [&](PermissionAction action) {
    return contains(actual, action);
  });
}

template <typename T>
void push_unique(std::vector<T>& target, const T& value) {
  if (!contains(target, value)) {
    target.push_back(value);
  }
}

template <typename T>
T max_axis(T left, T right) {
  return static_cast<T>(std::max(static_cast<int>(left), static_cast<int>(right)));
}

Value actions_to_value(const std::vector<PermissionAction>& actions) {
  Value::Array out;
  out.reserve(actions.size());
  for (const auto action : actions) {
    out.emplace_back(to_string(action));
  }
  return Value(std::move(out));
}

std::vector<PermissionAction> actions_from_value(const Value& value) {
  std::vector<PermissionAction> out;
  if (!value.is_array()) {
    return out;
  }
  for (const auto& entry : value.as_array()) {
    if (entry.is_string()) {
      out.push_back(permission_action_from_string(entry.as_string()));
    }
  }
  return out;
}

}  // namespace

std::string to_string(PermissionAction action) {
  switch (action) {
    case PermissionAction::ToolInvoke: return "tool.invoke";
    case PermissionAction::ProcessExecute: return "process.execute";
    case PermissionAction::FilesystemRead: return "filesystem.read";
    case PermissionAction::FilesystemWrite: return "filesystem.write";
    case PermissionAction::FilesystemDelete: return "filesystem.delete";
    case PermissionAction::NetworkConnect: return "network.connect";
    case PermissionAction::EnvRead: return "env.read";
    case PermissionAction::StateRead: return "state.read";
    case PermissionAction::StateWrite: return "state.write";
    case PermissionAction::MemoryRead: return "memory.read";
    case PermissionAction::KnowledgeRead: return "knowledge.read";
    case PermissionAction::BrowserRead: return "browser.read";
    case PermissionAction::BrowserScreenshot: return "browser.screenshot";
    case PermissionAction::RepositoryRead: return "repository.read";
    case PermissionAction::RepositoryWrite: return "repository.write";
    case PermissionAction::WorkflowRead: return "workflow.read";
    case PermissionAction::WorkflowResume: return "workflow.resume";
    case PermissionAction::WorkflowRespond: return "workflow.respond";
    case PermissionAction::WorkflowControl: return "workflow.control";
    case PermissionAction::ToolDiscover: return "tool.discover";
  }
  return "tool.invoke";
}

PermissionAction permission_action_from_string(std::string_view value,
                                               PermissionAction fallback) {
  const auto text = lower(value);
  if (text == "tool.invoke") return PermissionAction::ToolInvoke;
  if (text == "process.execute") return PermissionAction::ProcessExecute;
  if (text == "filesystem.read") return PermissionAction::FilesystemRead;
  if (text == "filesystem.write") return PermissionAction::FilesystemWrite;
  if (text == "filesystem.delete") return PermissionAction::FilesystemDelete;
  if (text == "network.connect") return PermissionAction::NetworkConnect;
  if (text == "env.read") return PermissionAction::EnvRead;
  if (text == "state.read") return PermissionAction::StateRead;
  if (text == "state.write") return PermissionAction::StateWrite;
  if (text == "memory.read") return PermissionAction::MemoryRead;
  if (text == "knowledge.read") return PermissionAction::KnowledgeRead;
  if (text == "browser.read") return PermissionAction::BrowserRead;
  if (text == "browser.screenshot") return PermissionAction::BrowserScreenshot;
  if (text == "repository.read") return PermissionAction::RepositoryRead;
  if (text == "repository.write") return PermissionAction::RepositoryWrite;
  if (text == "workflow.read") return PermissionAction::WorkflowRead;
  if (text == "workflow.resume") return PermissionAction::WorkflowResume;
  if (text == "workflow.respond") return PermissionAction::WorkflowRespond;
  if (text == "workflow.control") return PermissionAction::WorkflowControl;
  if (text == "tool.discover") return PermissionAction::ToolDiscover;
  return fallback;
}

std::string to_string(PermissionResourceKind kind) {
  switch (kind) {
    case PermissionResourceKind::Tool: return "tool";
    case PermissionResourceKind::Process: return "process";
    case PermissionResourceKind::Filesystem: return "filesystem";
    case PermissionResourceKind::Network: return "network";
    case PermissionResourceKind::Env: return "env";
    case PermissionResourceKind::State: return "state";
    case PermissionResourceKind::Memory: return "memory";
    case PermissionResourceKind::Knowledge: return "knowledge";
    case PermissionResourceKind::Browser: return "browser";
    case PermissionResourceKind::Repository: return "repository";
    case PermissionResourceKind::Workflow: return "workflow";
    case PermissionResourceKind::Sandbox: return "sandbox";
    case PermissionResourceKind::Unknown: return "unknown";
  }
  return "unknown";
}

PermissionResourceKind permission_resource_kind_from_string(
    std::string_view value,
    PermissionResourceKind fallback) {
  const auto text = lower(value);
  if (text == "tool") return PermissionResourceKind::Tool;
  if (text == "process") return PermissionResourceKind::Process;
  if (text == "filesystem") return PermissionResourceKind::Filesystem;
  if (text == "network") return PermissionResourceKind::Network;
  if (text == "env") return PermissionResourceKind::Env;
  if (text == "state") return PermissionResourceKind::State;
  if (text == "memory") return PermissionResourceKind::Memory;
  if (text == "knowledge") return PermissionResourceKind::Knowledge;
  if (text == "browser") return PermissionResourceKind::Browser;
  if (text == "repository") return PermissionResourceKind::Repository;
  if (text == "workflow") return PermissionResourceKind::Workflow;
  if (text == "sandbox") return PermissionResourceKind::Sandbox;
  if (text == "unknown") return PermissionResourceKind::Unknown;
  return fallback;
}

std::string to_string(PermissionDecisionSource source) {
  switch (source) {
    case PermissionDecisionSource::Default: return "default";
    case PermissionDecisionSource::Rule: return "rule";
    case PermissionDecisionSource::Capability: return "capability";
    case PermissionDecisionSource::Classifier: return "classifier";
    case PermissionDecisionSource::Approval: return "approval";
    case PermissionDecisionSource::Skill: return "skill";
    case PermissionDecisionSource::Host: return "host";
  }
  return "default";
}

PermissionDecisionSource permission_decision_source_from_string(
    std::string_view value,
    PermissionDecisionSource fallback) {
  const auto text = lower(value);
  if (text == "default") return PermissionDecisionSource::Default;
  if (text == "rule") return PermissionDecisionSource::Rule;
  if (text == "capability") return PermissionDecisionSource::Capability;
  if (text == "classifier") return PermissionDecisionSource::Classifier;
  if (text == "approval") return PermissionDecisionSource::Approval;
  if (text == "skill") return PermissionDecisionSource::Skill;
  if (text == "host") return PermissionDecisionSource::Host;
  return fallback;
}

bool permission_resource_matches(const PermissionResource& resource,
                                 const PermissionResourceMatcher& matcher) {
  if (!matches_enum(resource.kind, matcher.kinds)) {
    return false;
  }
  if (!matches_string(resource.id, matcher.ids)) {
    return false;
  }
  if (!matches_any_action(resource.actions, matcher.actions)) {
    return false;
  }
  if (!matches_string(resource.boundary.command, matcher.commands)) {
    return false;
  }
  if (!matches_string(resource.boundary.host, matcher.hosts)) {
    return false;
  }
  if (!matcher.path_prefixes.empty()) {
    const auto path = !resource.boundary.path.empty() ? resource.boundary.path
                                                      : resource.boundary.path_prefix;
    if (path.empty()) {
      return false;
    }
    const bool matched = std::any_of(matcher.path_prefixes.begin(), matcher.path_prefixes.end(),
                                     [&](const std::string& prefix) {
                                       return path == prefix || path.rfind(prefix, 0) == 0;
                                     });
    if (!matched) {
      return false;
    }
  }
  return true;
}

SandboxRequest sandbox_request_from_permission_resources(
    const std::vector<PermissionResource>& resources,
    Value extensions) {
  SandboxRequest request;
  request.extensions = std::move(extensions);

  for (const auto& resource : resources) {
    if (resource.kind == PermissionResourceKind::Filesystem) {
      const auto path = !resource.boundary.path.empty() ? resource.boundary.path
                                                        : !resource.boundary.path_prefix.empty()
                                                              ? resource.boundary.path_prefix
                                                              : resource.id;
      if (contains(resource.actions, PermissionAction::FilesystemWrite) ||
          contains(resource.actions, PermissionAction::FilesystemDelete)) {
        request.capabilities.fs = max_axis(request.capabilities.fs, FsAccess::ReadWrite);
        if (!path.empty()) {
          push_unique(request.fs_write_paths, std::filesystem::path(path));
        }
      } else if (contains(resource.actions, PermissionAction::FilesystemRead)) {
        request.capabilities.fs = max_axis(request.capabilities.fs, FsAccess::ReadOnly);
        if (!path.empty()) {
          push_unique(request.fs_read_paths, std::filesystem::path(path));
        }
      }
    }

    if (resource.kind == PermissionResourceKind::Network &&
        contains(resource.actions, PermissionAction::NetworkConnect)) {
      request.capabilities.net = max_axis(request.capabilities.net, NetAccess::Allowlist);
      const auto host = !resource.boundary.host.empty() ? resource.boundary.host : resource.id;
      if (!host.empty()) {
        push_unique(request.net_hosts, host);
      }
    }

    if (resource.kind == PermissionResourceKind::Process &&
        contains(resource.actions, PermissionAction::ProcessExecute)) {
      request.capabilities.process = max_axis(request.capabilities.process, ProcessAccess::ChildOnly);
      const auto command = !resource.boundary.command.empty() ? resource.boundary.command : resource.id;
      if (!command.empty()) {
        push_unique(request.allowed_subcommands, command);
      }
    }
  }

  return request;
}

SandboxRequest merge_sandbox_requests(const SandboxRequest& base,
                                      const SandboxRequest& overlay) {
  SandboxRequest merged = base;
  merged.capabilities.fs = max_axis(base.capabilities.fs, overlay.capabilities.fs);
  merged.capabilities.net = max_axis(base.capabilities.net, overlay.capabilities.net);
  merged.capabilities.process = max_axis(base.capabilities.process, overlay.capabilities.process);
  merged.capabilities.syscall = max_axis(base.capabilities.syscall, overlay.capabilities.syscall);
  for (const auto& path : overlay.fs_read_paths) {
    push_unique(merged.fs_read_paths, path);
  }
  for (const auto& path : overlay.fs_write_paths) {
    push_unique(merged.fs_write_paths, path);
  }
  for (const auto& host : overlay.net_hosts) {
    push_unique(merged.net_hosts, host);
  }
  for (const auto& command : overlay.allowed_subcommands) {
    push_unique(merged.allowed_subcommands, command);
  }
  if (merged.extensions.is_object() && overlay.extensions.is_object()) {
    for (const auto& [key, value] : overlay.extensions.as_object()) {
      merged.extensions[key] = value;
    }
  } else if (overlay.extensions.is_object()) {
    merged.extensions = overlay.extensions;
  }
  return merged;
}

Value permission_resource_to_value(const PermissionResource& resource) {
  return Value::object({
      {"kind", to_string(resource.kind)},
      {"id", resource.id},
      {"actions", actions_to_value(resource.actions)},
      {"boundary", Value::object({
                       {"path", resource.boundary.path},
                       {"pathPrefix", resource.boundary.path_prefix},
                       {"host", resource.boundary.host},
                       {"command", resource.boundary.command},
                   })},
      {"source", to_string(resource.source)},
      {"priority", resource.priority},
      {"metadata", resource.metadata},
  });
}

PermissionResource permission_resource_from_value(const Value& value) {
  PermissionResource resource;
  if (!value.is_object()) {
    return resource;
  }
  const auto& obj = value.as_object();
  if (auto it = obj.find("kind"); it != obj.end() && it->second.is_string()) {
    resource.kind = permission_resource_kind_from_string(it->second.as_string());
  }
  if (auto it = obj.find("id"); it != obj.end() && it->second.is_string()) {
    resource.id = it->second.as_string();
  }
  if (auto it = obj.find("actions"); it != obj.end()) {
    resource.actions = actions_from_value(it->second);
  }
  if (auto it = obj.find("boundary"); it != obj.end() && it->second.is_object()) {
    const auto& boundary = it->second.as_object();
    if (auto path = boundary.find("path"); path != boundary.end() && path->second.is_string()) {
      resource.boundary.path = path->second.as_string();
    }
    if (auto prefix = boundary.find("pathPrefix"); prefix != boundary.end() &&
                                                prefix->second.is_string()) {
      resource.boundary.path_prefix = prefix->second.as_string();
    }
    if (auto host = boundary.find("host"); host != boundary.end() && host->second.is_string()) {
      resource.boundary.host = host->second.as_string();
    }
    if (auto command = boundary.find("command"); command != boundary.end() &&
                                                    command->second.is_string()) {
      resource.boundary.command = command->second.as_string();
    }
  }
  if (auto it = obj.find("source"); it != obj.end() && it->second.is_string()) {
    resource.source = permission_decision_source_from_string(it->second.as_string());
  }
  if (auto it = obj.find("priority"); it != obj.end() && it->second.is_number()) {
    resource.priority = static_cast<PermissionPriority>(it->second.as_integer());
  }
  if (auto it = obj.find("metadata"); it != obj.end()) {
    resource.metadata = it->second;
  }
  return resource;
}

}  // namespace agent
