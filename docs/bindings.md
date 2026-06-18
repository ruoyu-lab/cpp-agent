# Host-Language Bindings

`agent_native` is designed to be embedded from any language that can call C
(Python, Go, Rust, Java/Kotlin, C#, Swift, Node.js native add-ons, PHP, Ruby,
Lua, …). The framework itself is C++20, but the integration boundary is a
thin **C ABI shim** that callers in other languages bind through their normal
FFI.

The shim is shipped in two in-tree CMake targets:

- `agent_capi`: embeddable runner ABI. Header:
  [`include/agent_capi.h`](../include/agent_capi.h) (pure C99). Links
  `agent_runtime`, not app/server/I/O.
- `agent_capi_full`: batteries-included C ABI. Header:
  [`include/agent_capi_full.h`](../include/agent_capi_full.h) (pure C99,
  extends `agent_capi.h`). Links `agent_app` for config-backed constructors
  and async-agent-run modules.
- Source: [`src/agent_capi.cpp`](../src/agent_capi.cpp), compiled once for the
  minimal target and once with full extensions enabled.
- Round-trip test: [`tests/capi_smoke.c`](../tests/capi_smoke.c) (compiled as
  C99 to prove the header has no C++ leaks; registered with CTest as
  `agent_capi_smoke`).
- Host-language fixture: [`tests/capi_ctypes_smoke.py`](../tests/capi_ctypes_smoke.py)
  (loads the built shared shim through Python `ctypes`; registered with CTest
  as `agent_python_ctypes_smoke` when Python is available).

The shim ships a representative ABI v4 surface: runner lifecycle,
synchronous run, callback streaming, pull-based async-iterator streaming,
explicit cancellation handles, opaque async run handles, structured error
objects, host model vtable injection, usage metadata, version negotiation, memory
ownership, and a machine-readable contract document. Extend it the same way for
evals, workflows, sessions, config loading, or any other capability your host
needs to expose - every new function is one more stability commitment, so only
export the surface you actually use.

This page is about how to wire and extend that shim. The framework
intentionally does not ship a generated SWIG/pybind/Napi surface, because the
right shape depends on which slice of capability the host wants to expose.

## Why a C Shim

- Every mainstream language has a stable C FFI. None of them share a stable
  C++ ABI.
- A C surface gives one boundary to version, document, audit, and test.
- C++ types like `std::string`, `std::variant`, `std::function`, and
  `std::optional` do not cross language boundaries cleanly. JSON or opaque
  handle pointers do.

## Recommended Shape

1. **Opaque handles.** Every framework object that the host needs to keep
   alive (runner, registry, store, listener, …) becomes a `void*` handle
   returned by `agent_*_create` and freed by `agent_*_release`.
2. **JSON at the boundary.** Pass payloads as UTF-8 JSON `const char*`. The
   framework already has `agent::parse_json` / `agent::stringify` for both
   directions, so the shim adds no parsing dependency.
3. **Error codes plus error objects.** Return an `int32_t` status and expose
   `agent_last_error()` for quick diagnostics. Bindings that need stable
   machine handling should copy `agent_last_error_object()` immediately and
   release it with `agent_error_release()`.
4. **Callbacks via function pointer + `void* user`.** The shipped C ABI uses
   this pattern for host-owned chat models:
   `int (*model)(const char* req_json, char** resp_json, void* user)`.
   Additional host-owned adapters such as HTTP transport, MCP transport, or
   browser renderer should be added as separate ABI surfaces when they are
   actually wired and tested.
5. **Memory ownership is explicit.** All `char*` returned by the shim must be
   freed by `agent_string_free`. Hosts must never `free()` framework memory
   directly.

## Reference: Shipped Surface

The current `agent_capi.h` exposes the groups below; the header remains the
authoritative signature contract:

- ABI/version: `agent_capi_abi_version`,
  `agent_capi_negotiate_abi_version`, `agent_capi_version_info_json`,
  `agent_capi_contract_json`.
- Memory and errors: `agent_string_clone`, `agent_string_free`,
  `agent_last_error`, `agent_last_error_object`, `agent_error_*`.
- Host injection: `agent_host_runtime_create`,
  `agent_host_runtime_describe_json`, `agent_runner_create_with_host_model`.
- Runner lifecycle: echo/host constructors and `agent_runner_release`.
- Run/stream: text and JSON run, cancellable variants, callback streaming, and
  pull-based event streams.
- Async run handles: `agent_runner_run_async`,
  `agent_runner_run_json_async`, `agent_run_wait_json`,
  `agent_run_try_get_json`, `agent_run_cancel`, `agent_run_release`.
- Generic ToolRun runtime through JSON.

`agent_capi_full.h` adds the app/config and async-agent-run module surface:
`agent_runner_create_from_config_json`, `agent_runner_create_from_config_path`,
`agent_async_runtime_create`, and `agent_async_run_*`.

The common core still looks like:

```c
const char* agent_last_error(void);
char*       agent_string_clone(const char* str);
void        agent_string_free(char* str);
const char* agent_version(void);
int32_t     agent_capi_abi_version(void);
int32_t     agent_capi_negotiate_abi_version(int32_t min_version,
                                             int32_t max_version,
                                             int32_t* out_version);
const char* agent_capi_contract_json(void);

typedef struct agent_runner_t agent_runner_t;
typedef struct agent_runner_event_stream_t agent_runner_event_stream_t;
typedef struct agent_tool_run_runtime_t agent_tool_run_runtime_t;

int32_t agent_runner_create_with_echo_model(agent_runner_t** out);
void    agent_runner_release(agent_runner_t* runner);

int32_t agent_runner_run(agent_runner_t* runner,
                         const char* input,
                         const char* session_id,
                         char** out_result_json);

int32_t agent_runner_run_json(agent_runner_t* runner,
                              const char* input_json,
                              const char* session_id,
                              char** out_result_json);

typedef int32_t (*agent_stream_callback_t)(const char* event_json, void* user_data);

int32_t agent_runner_stream(agent_runner_t* runner,
                            const char* input,
                            const char* session_id,
                            agent_stream_callback_t on_event,
                            void* user_data);

int32_t agent_runner_stream_json(agent_runner_t* runner,
                                 const char* input_json,
                                 const char* session_id,
                                 agent_stream_callback_t on_event,
                                 void* user_data);

int32_t agent_runner_stream_events(agent_runner_t* runner,
                                   const char* input,
                                   const char* session_id,
                                   size_t capacity,
                                   agent_runner_event_stream_t** out_stream);

int32_t agent_runner_stream_events_json(agent_runner_t* runner,
                                        const char* input_json,
                                        const char* session_id,
                                        size_t capacity,
                                        agent_runner_event_stream_t** out_stream);

int32_t agent_runner_event_stream_next_json(agent_runner_event_stream_t* stream,
                                            char** out_event_json,
                                            int32_t* out_has_event);

void agent_runner_event_stream_cancel(agent_runner_event_stream_t* stream,
                                      const char* reason);
void agent_runner_event_stream_close(agent_runner_event_stream_t* stream);
void agent_runner_event_stream_release(agent_runner_event_stream_t* stream);

int32_t agent_tool_run_runtime_create(agent_tool_run_runtime_t** out);
void    agent_tool_run_runtime_release(agent_tool_run_runtime_t* runtime);
int32_t agent_tool_run_start_json(agent_tool_run_runtime_t* runtime,
                                  const char* start_json,
                                  char** out_snapshot_json);
int32_t agent_tool_run_status_json(agent_tool_run_runtime_t* runtime,
                                   const char* run_id,
                                   char** out_snapshot_json);
int32_t agent_tool_run_list_json(agent_tool_run_runtime_t* runtime,
                                 const char* filter_json,
                                 char** out_runs_json);
int32_t agent_tool_run_update_json(agent_tool_run_runtime_t* runtime,
                                   const char* run_id,
                                   const char* update_json,
                                   char** out_snapshot_json);
int32_t agent_tool_run_append_event_json(agent_tool_run_runtime_t* runtime,
                                         const char* run_id,
                                         const char* event_json,
                                         char** out_event_json);
int32_t agent_tool_run_read_json(agent_tool_run_runtime_t* runtime,
                                 const char* run_id,
                                 const char* read_json,
                                 char** out_read_json);
int32_t agent_tool_run_cancel_json(agent_tool_run_runtime_t* runtime,
                                   const char* run_id,
                                   const char* cancel_json,
                                   char** out_snapshot_json);
int32_t agent_tool_run_wait_json(agent_tool_run_runtime_t* runtime,
                                 const char* run_id,
                                 const char* wait_json,
                                 char** out_snapshot_json);
```

All non-void entry points return:

| status | meaning |
|---:|---|
| `0` | success |
| `1` | `agent::AgentFrameworkError` (validation, configuration, adapter) |
| `2` | `std::exception` |
| `3` | unknown error |

On non-zero return, call `agent_last_error()` for the thread-local message.

The full extension `agent_runner_create_from_config_json` resolves relative paths from the host
process current working directory because the shim has no config file path at
that boundary. Hosts that need file-relative semantics should either chdir
before construction or use `agent_runner_create_from_config_path`.

The full extension `agent_runner_create_from_config_path` accepts a JSON config file path or a
directory to search for the default native config file. JS/TS module configs
remain unsupported at this boundary because the C shim does not accept a
`NativeConfigModuleLoader` callback.

`agent_capi_contract_json()` returns a borrowed JSON document that binding
generators can inspect at runtime. It carries:

- Native version string.
- ABI version.
- Status-code meanings.
- Constructor names.
- The stable run-result envelope and stream event categories.
- Async-iterator entry points for pull-based runner streaming.
- ToolRun entry points for generic custom/background tool work.

This is the release-governed metadata surface for foreign-language tooling.

ToolRun is independent of `AgentRunner`: a host can create
`agent_tool_run_runtime_t` and manage custom background jobs entirely through
UTF-8 JSON. The stable snapshot fields are `runId`, `toolCallId`, `toolName`,
`kind`, `label`, `status`, `startedAt`, `updatedAt`, `finishedAt`, `ready`,
`error`, and `metadata`; event reads return `{ run, cursor, nextCursor,
events }`. Process execution can be layered on this surface, but the ABI does
not assume the run is a terminal command.

Long-running background agent work is governed by the language-neutral
[Async Agent Run](async-agent-run.md) JSON contract. C, C#, Rust, Python, and
other bindings should expose async start/read/cancel/resume operations as JSON
documents instead of leaking C++ futures, host threads, or platform timer
handles across the ABI.

## Pull Stream Contract

ABI v4 bindings should expose `agent_runner_stream_events_json` as the primary
realtime surface. The C handle maps directly to a host-language async iterator:

```text
open(inputJson, sessionId, capacity) -> handle
next(handle) -> { done: false, value: AgentRunnerStreamEventJson }
next(handle) -> { done: true }
cancel(handle, reason)
close(handle)
release(handle)
```

`capacity` controls backpressure. When the bounded queue is full, the native
runner worker waits until the host pulls another event. `cancel` and `close`
are cooperative: they close the queue, cancel the internally owned runner token
when the stream was opened without an external token, and make later `next`
calls return `hasEvent = 0`. `release` destroys the handle; bindings should
call `cancel` first when a user cancels iteration early.

Every pulled JSON event has the stable envelope:

```json
{
  "schemaVersion": 1,
  "sequence": 1,
  "type": "status"
}
```

`sequence` is monotonically increasing per runner stream. The formal JSON
Schema contract is in
`contracts/observable/stream-events.schema.json`; provider streaming capability
metadata is in `contracts/observable/provider-stream-contract.schema.json`.

## Result And Event Contracts

The synchronous run surface intentionally stays narrow:

```json
{
  "sessionId": "...",
  "text": "...",
  "iterationCount": 1,
  "terminationReason": "completed"
}
```

That compact envelope is the compatibility contract used by existing bindings
and observable contract tests. Richer state such as retrieval payloads,
planning payloads, tool-call deltas, loop events, and error snapshots is
available through streaming events instead of being added to the synchronous
result object.

## Skeleton: Extending the Shim

When you need to expose another surface (eval, workflow, session, config, …),
follow the same template. The header sketch below is illustrative — it does
**not** ship today; copy it into a host-side fork or upstream a PR.

```c
// extending agent_capi.h — distribute alongside the static library.
#ifdef __cplusplus
extern "C" {
#endif

typedef struct agent_runner_t agent_runner_t;

int32_t agent_runner_create_from_config_json(const char* config_json,
                                             agent_runner_t** out);

int32_t agent_runner_run_json(agent_runner_t* runner,
                              const char* input_json,
                              const char* session_id,
                              char** out_result_json);

int32_t agent_runner_stream_json(agent_runner_t* runner,
                                 const char* input_json,
                                 const char* session_id,
                                 int (*on_event)(const char* event_json,
                                                 void* user),
                                 void* user);

void agent_runner_release(agent_runner_t* runner);

const char* agent_last_error(void);
void        agent_string_free(char* str);

#ifdef __cplusplus
}
#endif
```

## Skeleton: Shim Implementation

```cpp
// agent_capi.cpp — links agent_app for config-backed runner constructors.
#include "agent_capi.h"
#include "agent/app_api.hpp"

#include <atomic>
#include <cstring>
#include <new>
#include <string>

namespace {
thread_local std::string g_last_error;

char* dup_cstr(const std::string& s) {
  auto* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, s.data(), s.size() + 1);
  return p;
}

template <typename Fn>
int32_t guarded(Fn&& fn) {
  try {
    fn();
    return 0;
  } catch (const agent::AgentFrameworkError& e) {
    g_last_error = e.what();
    return 1;
  } catch (const std::exception& e) {
    g_last_error = e.what();
    return 2;
  } catch (...) {
    g_last_error = "unknown error";
    return 3;
  }
}
}  // namespace

struct agent_runner_t {
  std::unique_ptr<agent::AgentRunner> runner;
};

extern "C" int32_t agent_runner_create_from_config_json(const char* config_json,
                                                        agent_runner_t** out) {
  return guarded([&] {
    auto value = agent::parse_json(config_json);
    auto loaded = agent::load_native_loaded_agent_config(value);
    auto runner = loaded.resolve_runner(/* default agent */ {});
    auto* handle = new agent_runner_t{std::move(runner)};
    *out = handle;
  });
}

extern "C" int32_t agent_runner_run_json(agent_runner_t* runner,
                                         const char* input_json,
                                         const char* session_id,
                                         char** out_result_json) {
  return guarded([&] {
    auto input = agent::parse_json(input_json);
    auto result = runner->runner->run(input, session_id ? session_id : "");
    *out_result_json = dup_cstr(agent::stringify(result.to_value()));
  });
}

extern "C" int32_t agent_runner_stream_json(agent_runner_t* runner,
                                            const char* input_json,
                                            const char* session_id,
                                            int (*on_event)(const char*, void*),
                                            void* user) {
  return guarded([&] {
    auto input = agent::parse_json(input_json);
    auto callback = [on_event, user, forwarding = true](
                        const agent::AgentRunnerStreamEvent& ev) mutable {
      if (!forwarding) return;
      std::string payload = agent::stringify(ev.to_value());
      if (on_event(payload.c_str(), user) != 0) forwarding = false;
    };
    (void)runner->runner->stream(input, callback, session_id ? session_id : "");
  });
}

extern "C" void agent_runner_release(agent_runner_t* runner) { delete runner; }

extern "C" const char* agent_last_error(void) { return g_last_error.c_str(); }
extern "C" void agent_string_free(char* str) { std::free(str); }
```

Extend the same pattern for: `agent_eval_*`, `agent_workflow_*`,
`agent_session_*`, `agent_server_*`, `agent_tool_*`, and so on. Only expose
the surfaces the host actually needs — every additional symbol is a stability
commitment.

## Wiring Injected Adapters

When the host wants to provide its own TLS transport, model, browser, or
MCP transport, expose a registration entry point that accepts function
pointers:

```c
int32_t agent_register_http_transport(int (*send)(const char* req_json,
                                                  char** resp_json,
                                                  void* user),
                                      void* user);
```

The shim wraps the callback with `agent::make_callback_http_transport(...)`
or the equivalent factory and installs it on the appropriate registry.
Streaming responses can use a second `on_chunk` callback rather than a single
`resp_json`.

## Language Recipes

The patterns below all assume the C shim above.

### Python (ctypes)

```python
import ctypes, json

lib = ctypes.CDLL("./libagent_capi_full_shared.so")
lib.agent_runner_create_from_config_json.restype = ctypes.c_int32
lib.agent_runner_run_json.restype = ctypes.c_int32
lib.agent_string_free.argtypes = [ctypes.c_char_p]

handle = ctypes.c_void_p()
config = json.dumps({...}).encode()
assert lib.agent_runner_create_from_config_json(config, ctypes.byref(handle)) == 0

out = ctypes.c_char_p()
assert lib.agent_runner_run_json(handle, b'"hello"', b"session", ctypes.byref(out)) == 0
result = json.loads(out.value)
lib.agent_string_free(out)
lib.agent_runner_release(handle)
```

`pybind11` works too; ctypes is the dependency-free baseline.

### Go (cgo)

```go
/*
#cgo LDFLAGS: -L. -lagent_capi_full -lagent_app -lagent_mcp_native -lagent_mcp -lagent_model_providers -lagent_runtime_modules -lagent_runtime_io_native -lagent_runtime_io -lagent_platform -lagent_runtime -lagent_tools -lagent_model -lagent_core -lstdc++
#include "agent_capi_full.h"
#include <stdlib.h>
*/
import "C"
import (
    "encoding/json"
    "errors"
    "unsafe"
)

type Runner struct{ h *C.agent_runner_t }

func NewRunner(cfg any) (*Runner, error) {
    b, _ := json.Marshal(cfg)
    cstr := C.CString(string(b))
    defer C.free(unsafe.Pointer(cstr))

    var h *C.agent_runner_t
    if C.agent_runner_create_from_config_json(cstr, &h) != 0 {
        return nil, errors.New(C.GoString(C.agent_last_error()))
    }
    return &Runner{h: h}, nil
}
```

### Rust

```rust
// build.rs links libagent_capi_full and the native layer libraries; bindgen generates the FFI.
use std::ffi::{CStr, CString};

pub struct Runner { handle: *mut agent_runner_t }

impl Runner {
    pub fn from_config(cfg: &serde_json::Value) -> anyhow::Result<Self> {
        let s = CString::new(cfg.to_string())?;
        let mut h: *mut agent_runner_t = std::ptr::null_mut();
        let rc = unsafe { agent_runner_create_from_config_json(s.as_ptr(), &mut h) };
        if rc != 0 { anyhow::bail!(last_error()); }
        Ok(Self { handle: h })
    }
}

impl Drop for Runner { fn drop(&mut self) { unsafe { agent_runner_release(self.handle) } } }
```

### Java / Kotlin (JNI or Project Panama)

JNI: write a small `.cpp` per native method that converts `jstring` to
`const char*`, calls the shim, and converts the result back. Project Panama
(`java.lang.foreign`) consumes `agent_capi.h` directly via `jextract`.

### Node.js Native Add-On

Use `node-addon-api` (Napi). Each Napi method validates arguments, calls the
shim, and wraps the resulting JSON string with `JSON.parse` on return.

## Bridging the NodeJS Workspace

If the host is itself Node.js and you want to expose the C++ implementation
to NodeJS-style consumers, the same shim works through `node-addon-api`. The
trade-off vs. the TypeScript implementation:

- Pro: single dependency, deterministic synchronous calls, lower runtime
  overhead.
- Con: any provider/transport that the TypeScript packages would handle out of
  the box (TLS HTTPS, real Playwright, real Postgres/Redis drivers, JS/TS
  config modules) must be injected here.

See [Architecture](architecture.md#zero-dependency-boundary) for the full
list of injection points.

## Versioning the Shim

- Bump a `AGENT_CAPI_ABI_VERSION` macro for every breaking change.
- Add new functions instead of changing existing signatures.
- Avoid exposing C++ container layouts (don't return `std::vector` data
  pointers from extern "C" functions). Marshal through JSON or pre-allocated
  output buffers.

## Testing the Shim

The framework's smoke binary covers the C++ side. For each language binding,
write a small "round-trip" test that:

1. Creates a runner from a static JSON config (using the echo model).
2. Runs a fixed input.
3. Asserts the JSON result matches a checked-in fixture.

Run this against the same `libagent_native` build the host uses in production.
This catches ABI drift and adapter wiring regressions long before a real
provider call is made.
