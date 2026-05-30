#include "agent/sandbox.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace agent {

namespace {

class CarrierScope final : public SandboxScope {
 public:
  explicit CarrierScope(SandboxRequest request) : request_(std::move(request)) {}
  [[nodiscard]] const SandboxRequest& request() const noexcept override { return request_; }
  [[nodiscard]] SandboxCapabilities effective() const noexcept override {
    return request_.capabilities;
  }

 private:
  SandboxRequest request_;
};

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return out;
}

Value path_list_to_value(const std::vector<std::filesystem::path>& paths) {
  Value::Array out;
  out.reserve(paths.size());
  for (const auto& path : paths) {
    out.emplace_back(path.string());
  }
  return Value(std::move(out));
}

std::vector<std::filesystem::path> path_list_from_value(const Value& value) {
  std::vector<std::filesystem::path> out;
  if (!value.is_array()) return out;
  for (const auto& entry : value.as_array()) {
    if (entry.is_string()) out.emplace_back(entry.as_string());
  }
  return out;
}

Value string_list_to_value(const std::vector<std::string>& items) {
  Value::Array out;
  out.reserve(items.size());
  for (const auto& item : items) {
    out.emplace_back(item);
  }
  return Value(std::move(out));
}

std::vector<std::string> string_list_from_value(const Value& value) {
  std::vector<std::string> out;
  if (!value.is_array()) return out;
  for (const auto& entry : value.as_array()) {
    if (entry.is_string()) out.emplace_back(entry.as_string());
  }
  return out;
}

std::string axis_summary(const SandboxCapabilities& caps) {
  return "fs=" + to_string(caps.fs) + ", net=" + to_string(caps.net) +
         ", process=" + to_string(caps.process) + ", syscall=" + to_string(caps.syscall);
}

}  // namespace

bool capabilities_satisfy(const SandboxCapabilities& provided,
                          const SandboxCapabilities& required) {
  return static_cast<int>(provided.fs)      >= static_cast<int>(required.fs)
      && static_cast<int>(provided.net)     >= static_cast<int>(required.net)
      && static_cast<int>(provided.process) >= static_cast<int>(required.process)
      && static_cast<int>(provided.syscall) >= static_cast<int>(required.syscall);
}

std::string to_string(FsAccess access) {
  switch (access) {
    case FsAccess::None:         return "none";
    case FsAccess::ReadOnly:     return "read-only";
    case FsAccess::ReadWrite:    return "read-write";
    case FsAccess::Unrestricted: return "unrestricted";
  }
  return "none";
}

std::string to_string(NetAccess access) {
  switch (access) {
    case NetAccess::None:         return "none";
    case NetAccess::Loopback:     return "loopback";
    case NetAccess::Allowlist:    return "allowlist";
    case NetAccess::Unrestricted: return "unrestricted";
  }
  return "none";
}

std::string to_string(ProcessAccess access) {
  switch (access) {
    case ProcessAccess::None:         return "none";
    case ProcessAccess::ChildOnly:    return "child-only";
    case ProcessAccess::Unrestricted: return "unrestricted";
  }
  return "none";
}

std::string to_string(SyscallAccess access) {
  switch (access) {
    case SyscallAccess::Minimal:      return "minimal";
    case SyscallAccess::Standard:     return "standard";
    case SyscallAccess::Unrestricted: return "unrestricted";
  }
  return "standard";
}

FsAccess fs_access_from_string(std::string_view text, FsAccess fallback) {
  const auto value = lower(text);
  if (value == "none" || value.empty()) return FsAccess::None;
  if (value == "read-only" || value == "read_only" || value == "readonly") return FsAccess::ReadOnly;
  if (value == "read-write" || value == "read_write" || value == "readwrite") return FsAccess::ReadWrite;
  if (value == "unrestricted") return FsAccess::Unrestricted;
  return fallback;
}

NetAccess net_access_from_string(std::string_view text, NetAccess fallback) {
  const auto value = lower(text);
  if (value == "none" || value.empty()) return NetAccess::None;
  if (value == "loopback") return NetAccess::Loopback;
  if (value == "allowlist" || value == "allow-list" || value == "allow_list") return NetAccess::Allowlist;
  if (value == "unrestricted") return NetAccess::Unrestricted;
  return fallback;
}

ProcessAccess process_access_from_string(std::string_view text, ProcessAccess fallback) {
  const auto value = lower(text);
  if (value == "none" || value.empty()) return ProcessAccess::None;
  if (value == "child-only" || value == "child_only" || value == "childonly") return ProcessAccess::ChildOnly;
  if (value == "unrestricted") return ProcessAccess::Unrestricted;
  return fallback;
}

SyscallAccess syscall_access_from_string(std::string_view text, SyscallAccess fallback) {
  const auto value = lower(text);
  if (value == "minimal") return SyscallAccess::Minimal;
  if (value == "standard" || value.empty()) return SyscallAccess::Standard;
  if (value == "unrestricted") return SyscallAccess::Unrestricted;
  return fallback;
}

SandboxCapabilities NoopSandbox::max_capabilities() const noexcept {
  return SandboxCapabilities{
      .fs = FsAccess::None,
      .net = NetAccess::None,
      .process = ProcessAccess::None,
      .syscall = SyscallAccess::Standard,
  };
}

std::unique_ptr<SandboxScope> NoopSandbox::enter(const SandboxRequest& request) {
  return std::make_unique<CarrierScope>(request);
}

Value sandbox_capabilities_to_value(const SandboxCapabilities& caps) {
  return Value::object({
      {"fs", to_string(caps.fs)},
      {"net", to_string(caps.net)},
      {"process", to_string(caps.process)},
      {"syscall", to_string(caps.syscall)},
  });
}

SandboxCapabilities sandbox_capabilities_from_value(const Value& value) {
  SandboxCapabilities caps;
  if (!value.is_object()) return caps;
  const auto& obj = value.as_object();
  if (auto it = obj.find("fs"); it != obj.end() && it->second.is_string()) {
    caps.fs = fs_access_from_string(it->second.as_string());
  }
  if (auto it = obj.find("net"); it != obj.end() && it->second.is_string()) {
    caps.net = net_access_from_string(it->second.as_string());
  }
  if (auto it = obj.find("process"); it != obj.end() && it->second.is_string()) {
    caps.process = process_access_from_string(it->second.as_string());
  }
  if (auto it = obj.find("syscall"); it != obj.end() && it->second.is_string()) {
    caps.syscall = syscall_access_from_string(it->second.as_string());
  }
  return caps;
}

Value sandbox_request_to_value(const SandboxRequest& request) {
  return Value::object({
      {"capabilities", sandbox_capabilities_to_value(request.capabilities)},
      {"fsReadPaths", path_list_to_value(request.fs_read_paths)},
      {"fsWritePaths", path_list_to_value(request.fs_write_paths)},
      {"netHosts", string_list_to_value(request.net_hosts)},
      {"allowedSubcommands", string_list_to_value(request.allowed_subcommands)},
      {"extensions", request.extensions},
  });
}

SandboxRequest sandbox_request_from_value(const Value& value) {
  SandboxRequest request;
  if (!value.is_object()) return request;
  const auto& obj = value.as_object();
  if (auto it = obj.find("capabilities"); it != obj.end()) {
    request.capabilities = sandbox_capabilities_from_value(it->second);
  }
  if (auto it = obj.find("fsReadPaths"); it != obj.end()) {
    request.fs_read_paths = path_list_from_value(it->second);
  }
  if (auto it = obj.find("fsWritePaths"); it != obj.end()) {
    request.fs_write_paths = path_list_from_value(it->second);
  }
  if (auto it = obj.find("netHosts"); it != obj.end()) {
    request.net_hosts = string_list_from_value(it->second);
  }
  if (auto it = obj.find("allowedSubcommands"); it != obj.end()) {
    request.allowed_subcommands = string_list_from_value(it->second);
  }
  if (auto it = obj.find("extensions"); it != obj.end()) {
    request.extensions = it->second;
  }
  return request;
}

SandboxRequest enforce_sandbox_contract(const ToolSandboxPolicy& policy,
                                        const Sandbox* sandbox,
                                        EventBus* event_bus) {
  const SandboxCapabilities provided =
      sandbox ? sandbox->max_capabilities() : SandboxCapabilities{};

  auto emit_entered = [&](const SandboxRequest& effective, const char* mode) {
    if (!event_bus) return;
    event_bus->publish("sandbox.entered", ExecutionTarget::Tool,
                       Value::object({
                           {"request", sandbox_request_to_value(effective)},
                           {"mode", std::string(mode)},
                       }));
  };

  if (policy.preferred.has_value()) {
    if (sandbox && capabilities_satisfy(provided, policy.preferred->capabilities)) {
      emit_entered(*policy.preferred, "preferred");
      return *policy.preferred;
    }
  }

  if (sandbox == nullptr) {
    // Even with no sandbox, an empty required policy is satisfiable (None
    // capabilities trivially satisfy themselves).
    if (capabilities_satisfy(SandboxCapabilities{}, policy.required.capabilities)) {
      emit_entered(policy.required, "required");
      return policy.required;
    }
    const std::string reason =
        "No sandbox installed but tool requires capabilities {" +
        axis_summary(policy.required.capabilities) + "}.";
    if (event_bus) {
      event_bus->publish("sandbox.violated", ExecutionTarget::Tool,
                         Value::object({
                             {"required", sandbox_capabilities_to_value(policy.required.capabilities)},
                             {"provided", Value(nullptr)},
                             {"reason", reason},
                         }));
    }
    throw SandboxContractError(reason);
  }

  if (capabilities_satisfy(provided, policy.required.capabilities)) {
    emit_entered(policy.required, "required");
    return policy.required;
  }

  const std::string reason =
      "Sandbox capabilities {" + axis_summary(provided) +
      "} are below required {" + axis_summary(policy.required.capabilities) +
      "}; cannot satisfy tool contract.";
  if (event_bus) {
    event_bus->publish("sandbox.violated", ExecutionTarget::Tool,
                       Value::object({
                           {"required", sandbox_capabilities_to_value(policy.required.capabilities)},
                           {"provided", sandbox_capabilities_to_value(provided)},
                           {"reason", reason},
                       }));
  }
  throw SandboxContractError(reason);
}

namespace sandbox_presets {

SandboxRequest fs_restricted() {
  SandboxRequest request;
  request.capabilities = SandboxCapabilities{
      .fs = FsAccess::ReadWrite,
      .net = NetAccess::None,
      .process = ProcessAccess::None,
      .syscall = SyscallAccess::Standard,
  };
  return request;
}

SandboxRequest net_isolated() {
  SandboxRequest request;
  request.capabilities = SandboxCapabilities{
      .fs = FsAccess::None,
      .net = NetAccess::Allowlist,
      .process = ProcessAccess::None,
      .syscall = SyscallAccess::Standard,
  };
  return request;
}

SandboxRequest shell_safe() {
  SandboxRequest request;
  request.capabilities = SandboxCapabilities{
      .fs = FsAccess::ReadWrite,
      .net = NetAccess::None,
      .process = ProcessAccess::ChildOnly,
      .syscall = SyscallAccess::Standard,
  };
  return request;
}

SandboxRequest fully_open() {
  SandboxRequest request;
  request.capabilities = SandboxCapabilities{
      .fs = FsAccess::Unrestricted,
      .net = NetAccess::Unrestricted,
      .process = ProcessAccess::Unrestricted,
      .syscall = SyscallAccess::Unrestricted,
  };
  return request;
}

}  // namespace sandbox_presets

}  // namespace agent
