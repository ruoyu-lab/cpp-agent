# Sandbox

The Sandbox subsystem is the framework's seam between *what a tool needs* and
*what the host can grant*. The framework owns the contract (capability axes,
policy, enforcement, events). Platform-specific isolation mechanisms
(macOS Seatbelt, Linux seccomp + Landlock, Windows AppContainer, namespaces,
chroot, gVisor, …) live in the business layer behind the `Sandbox` interface.

## Capability axes

A sandbox is described by four independent, monotonic axes. Higher numbers
mean *more capability granted* (less restricted). A sandbox satisfies a
tool's request when **every** axis it can grant is `>=` what the tool
requires.

| Axis | Values (low → high) | Meaning |
|------|--------------------|---------|
| `FsAccess` | `None`, `ReadOnly`, `ReadWrite`, `Unrestricted` | What the tool may do with the filesystem. `ReadOnly` / `ReadWrite` are typically scoped by the allowlists on `SandboxRequest`. |
| `NetAccess` | `None`, `Loopback`, `Allowlist`, `Unrestricted` | Outbound network reach. `Allowlist` is paired with `net_hosts`. |
| `ProcessAccess` | `None`, `ChildOnly`, `Unrestricted` | Whether the tool may spawn subprocesses. `ChildOnly` permits direct children only (no daemonization). |
| `SyscallAccess` | `Minimal`, `Standard`, `Unrestricted` | Breadth of the host-syscall surface (Linux seccomp / macOS sandbox_init policy / Windows job-object restrictions). `Standard` is the default for "ordinary user-space code." |

`capabilities_satisfy(provided, required)` returns `true` iff every axis of
`provided` is `>= required`.

## `SandboxCapabilities`, `SandboxRequest`, `ToolSandboxPolicy`

```cpp
struct SandboxCapabilities {
  FsAccess      fs      = FsAccess::None;
  NetAccess     net     = NetAccess::None;
  ProcessAccess process = ProcessAccess::None;
  SyscallAccess syscall = SyscallAccess::Standard;
};

struct SandboxRequest {
  SandboxCapabilities capabilities;
  std::vector<std::filesystem::path> fs_read_paths;
  std::vector<std::filesystem::path> fs_write_paths;
  std::vector<std::string> net_hosts;
  std::vector<std::string> allowed_subcommands;
  Value extensions = Value::object({});  // free-form, platform-specific
};

struct ToolSandboxPolicy {
  SandboxRequest required;
  std::optional<SandboxRequest> preferred;
};
```

`SandboxCapabilities` is the pure axis-only view (used for max-capability
reporting and contract comparisons). `SandboxRequest` adds the allowlists
and an `extensions` blob platform implementations may consume or ignore.
`ToolSandboxPolicy` is what each `ToolDefinition` declares — a hard
`required` contract plus an optional `preferred` upgrade.

`SandboxScope` is the RAII handle a `Sandbox` returns from `enter()`:

```cpp
class SandboxScope {
 public:
  virtual const SandboxRequest&  request()   const noexcept = 0;
  virtual SandboxCapabilities    effective() const noexcept = 0;
};
```

The executor registers the active `SandboxScope*` as
`kToolServiceSandboxScope` for the duration of `invoke()` so downstream
consumers (e.g. `DeveloperProcessExecutor` reached via `shell.exec`) can
pick up `request()` from the scoped service view without rethreading
arguments.

## Contract semantics

```cpp
SandboxRequest enforce_sandbox_contract(const ToolSandboxPolicy& policy,
                                        const Sandbox* sandbox,
                                        EventBus* event_bus = nullptr);
```

Resolution order — the *first* satisfiable branch wins:

1. If `policy.preferred` is set **and** the sandbox can satisfy it →
   returns `*policy.preferred` (mode `"preferred"`).
2. Else if `policy.required` is satisfiable (by the sandbox, or trivially
   because every axis is `None` and no sandbox is installed) →
   returns `policy.required` (mode `"required"`).
3. Else throws `SandboxContractError` with a human-readable diagnostic
   describing which axes fell short.

The executor then calls `sandbox->enter(effective_request)` and runs the
tool inside the resulting scope.

## Events emitted

When `event_bus != nullptr`, the contract layer publishes:

| Category | Payload |
|----------|---------|
| `sandbox.violated` | `{ required: SandboxCapabilities, provided: SandboxCapabilities \| null, reason: string }` (`provided` is `null` when no sandbox was installed) |
| `sandbox.entered`  | `{ request: SandboxRequest, mode: "preferred" \| "required" }` |
| `sandbox.exited`   | `{ durationMs: number }` (emitted when a scope was actually entered, on every exit path — success, throw, return) |

All `SandboxCapabilities` and `SandboxRequest` payloads are produced via
`sandbox_capabilities_to_value` / `sandbox_request_to_value`, which use the
same key names a parallel runtime can read with the symmetric `_from_value`
helpers.

## Presets

```cpp
namespace sandbox_presets {
  SandboxRequest fs_restricted();  // fs=ReadWrite, net=None,      process=None,      syscall=Standard
  SandboxRequest net_isolated();   // fs=None,      net=Allowlist, process=None,      syscall=Standard
  SandboxRequest shell_safe();     // fs=ReadWrite, net=None,      process=ChildOnly, syscall=Standard
  SandboxRequest fully_open();     // every axis Unrestricted (trusted internal tools only)
}
```

`shell.exec` ships with `ToolSandboxPolicy{ .required = sandbox_presets::shell_safe() }`.
Other builtins default to an empty policy (every axis `None`), which is
trivially satisfiable by `NoopSandbox` or by no sandbox at all.

## Per-platform integration notes

The framework deliberately ships **no** platform-specific code. A business
layer translates the active `SandboxRequest` into the matching mechanism at
the moment a privileged operation happens.

- **macOS Seatbelt (`sandbox_init`, `sandbox_exec`).** Once entered, the
  Seatbelt profile cannot be loosened in the calling process. The
  recommended pattern is to make `SandboxScope` a *passive carrier* — its
  destructor is a no-op — and have the process spawner apply the profile at
  fork-exec by translating `request().capabilities`, `fs_read_paths`,
  `fs_write_paths`, and `net_hosts` into an SBPL string before
  `posix_spawnattr_setbinpref` + `posix_spawn`.

- **Linux seccomp + Landlock.** Real in-process isolation is possible.
  Implement `SandboxScope::~` to restore the prior seccomp filter (when
  layered via `SECCOMP_FILTER_FLAG_TSYNC` is not feasible, a `clone(CLONE_*)`
  worker thread can hold the filter and exit on scope destruction). Map
  `FsAccess` + `fs_read_paths` / `fs_write_paths` onto Landlock rulesets
  (`landlock_create_ruleset` → `landlock_add_rule` →
  `landlock_restrict_self`).

- **Windows AppContainer / Job Objects.** Like Seatbelt, isolation is
  applied at process creation. Treat `SandboxScope` as a passive carrier and
  apply the capabilities, named-pipe ACLs, and network firewall rules
  derived from the request via `UpdateProcThreadAttribute(...,
  PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES, ...)` before
  `CreateProcessW`.

In all three cases, the `extensions` field is the escape hatch — e.g. on
macOS you can stuff an inline SBPL fragment under
`request.extensions["sbplFragment"]` if the standard axes can't express what
you need.

## Test recipe — permissive in-process sandbox

For unit / smoke tests of tools whose policies are non-empty, a tiny inline
sandbox that promises everything and carries the request through to the
scope is usually enough:

```cpp
struct PermissiveTestSandbox final : agent::Sandbox {
  agent::SandboxCapabilities max_capabilities() const noexcept override {
    return agent::SandboxCapabilities{
        .fs = agent::FsAccess::Unrestricted,
        .net = agent::NetAccess::Unrestricted,
        .process = agent::ProcessAccess::Unrestricted,
        .syscall = agent::SyscallAccess::Unrestricted,
    };
  }
  std::unique_ptr<agent::SandboxScope> enter(const agent::SandboxRequest& request) override {
    struct InlineScope final : agent::SandboxScope {
      explicit InlineScope(agent::SandboxRequest req) : req_(std::move(req)) {}
      const agent::SandboxRequest& request() const noexcept override { return req_; }
      agent::SandboxCapabilities effective() const noexcept override {
        return req_.capabilities;
      }
      agent::SandboxRequest req_;
    };
    return std::make_unique<InlineScope>(request);
  }
};
```

Use `agent::NoopSandbox` when you specifically want a sandbox whose
`max_capabilities()` is `{None, None, None, Standard}` — useful for
exercising the rejection path on tools with non-trivial requirements.

Pair the sandbox with an `EventBus` to observe `sandbox.violated`,
`sandbox.entered`, and `sandbox.exited`, exactly as the smoke tests do.
