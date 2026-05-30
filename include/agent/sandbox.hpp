#pragma once

#include "agent/core.hpp"
#include "agent/execution.hpp"  // EventBus

#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

// Four independent capability axes. Each is monotonic — higher number means
// "more capability granted" (less restricted). A sandbox satisfies a tool
// requirement when each axis it can provide is >= what the tool requires.
enum class FsAccess      { None = 0, ReadOnly = 1, ReadWrite = 2, Unrestricted = 3 };
enum class NetAccess     { None = 0, Loopback = 1, Allowlist = 2, Unrestricted = 3 };
enum class ProcessAccess { None = 0, ChildOnly = 1, Unrestricted = 2 };
enum class SyscallAccess { Minimal = 0, Standard = 1, Unrestricted = 2 };

struct SandboxCapabilities {
  FsAccess fs = FsAccess::None;
  NetAccess net = NetAccess::None;
  ProcessAccess process = ProcessAccess::None;
  SyscallAccess syscall = SyscallAccess::Standard;
};

// True iff every axis of `provided` is >= the corresponding axis of `required`.
bool capabilities_satisfy(const SandboxCapabilities& provided,
                          const SandboxCapabilities& required);

std::string to_string(FsAccess access);
std::string to_string(NetAccess access);
std::string to_string(ProcessAccess access);
std::string to_string(SyscallAccess access);

FsAccess      fs_access_from_string(std::string_view text, FsAccess fallback = FsAccess::None);
NetAccess     net_access_from_string(std::string_view text, NetAccess fallback = NetAccess::None);
ProcessAccess process_access_from_string(std::string_view text,
                                         ProcessAccess fallback = ProcessAccess::None);
SyscallAccess syscall_access_from_string(std::string_view text,
                                         SyscallAccess fallback = SyscallAccess::Standard);

// What a tool needs at dispatch time. Beyond the capability axes, carries
// allowlists and a free-form extensions blob that platform implementations
// can consume (or ignore).
struct SandboxRequest {
  SandboxCapabilities capabilities;
  std::vector<std::filesystem::path> fs_read_paths;
  std::vector<std::filesystem::path> fs_write_paths;
  std::vector<std::string> net_hosts;
  std::vector<std::string> allowed_subcommands;
  Value extensions = Value::object({});
};

// A tool declares required (hard contract) and optionally preferred (soft).
// The executor uses `preferred` when the active sandbox can satisfy it;
// otherwise falls back to `required`. Both default to "no capabilities at
// all", which makes empty policies satisfiable by any sandbox or by no sandbox.
struct ToolSandboxPolicy {
  SandboxRequest required;
  std::optional<SandboxRequest> preferred;
};

// RAII handle carrying the *active* request. Downstream consumers like
// DeveloperProcessExecutor read `request()` to apply the right isolation at
// the moment they spawn a subprocess. On platforms where sandboxing is one-way
// (e.g. macOS Seatbelt), the scope is a passive carrier — its destructor is
// a no-op and the host's process spawner applies the request at fork-exec.
class SandboxScope {
 public:
  virtual ~SandboxScope() = default;
  [[nodiscard]] virtual const SandboxRequest& request() const noexcept = 0;
  [[nodiscard]] virtual SandboxCapabilities effective() const noexcept = 0;
};

class Sandbox {
 public:
  virtual ~Sandbox() = default;
  // Highest capability set this sandbox can grant on each axis.
  [[nodiscard]] virtual SandboxCapabilities max_capabilities() const noexcept = 0;
  [[nodiscard]] virtual std::unique_ptr<SandboxScope> enter(const SandboxRequest& request) = 0;
};

class NoopSandbox final : public Sandbox {
 public:
  [[nodiscard]] SandboxCapabilities max_capabilities() const noexcept override;
  std::unique_ptr<SandboxScope> enter(const SandboxRequest& request) override;
};

class SandboxContractError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Returns the effective request the executor should pass to `enter()`.
//   - If sandbox satisfies policy.preferred → returns *policy.preferred
//   - Else if sandbox satisfies policy.required → returns policy.required
//   - Else throws SandboxContractError
//
// If `event_bus` is non-null:
//   - on rejection: publishes "sandbox.violated" with payload
//     { required: <caps>, provided: <caps|null>, reason: "..." }
//   - on acceptance: publishes "sandbox.entered" with
//     { request: <request>, mode: "preferred"|"required" }
SandboxRequest enforce_sandbox_contract(const ToolSandboxPolicy& policy,
                                        const Sandbox* sandbox,
                                        EventBus* event_bus = nullptr);

// Serialization round-trips.
Value sandbox_capabilities_to_value(const SandboxCapabilities& caps);
SandboxCapabilities sandbox_capabilities_from_value(const Value& value);
Value sandbox_request_to_value(const SandboxRequest& request);
SandboxRequest sandbox_request_from_value(const Value& value);

// Convenience presets for common tool shapes.
namespace sandbox_presets {

// fs=ReadWrite (allowlist driven), net=None, process=None, syscall=Standard
SandboxRequest fs_restricted();
// fs=None, net=Allowlist, process=None, syscall=Standard
SandboxRequest net_isolated();
// fs=ReadWrite, net=None, process=ChildOnly, syscall=Standard
SandboxRequest shell_safe();
// All axes Unrestricted. Use only for trusted internal tools.
SandboxRequest fully_open();

}  // namespace sandbox_presets

}  // namespace agent
