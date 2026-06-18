# Scratch Store

The `ScratchStore` interface is the injection point for short-term, per-session
key/value memory shared by the `scratch.*` and `todo.*` builtin tools (and any
host-defined tool that wants a quick scratchpad without touching the model
context window). The framework owns the interface and ships two reference
backends; hosts plug in their own to add persistence, replication, or
multi-process coordination — no compat shims, no singleton fallback.

Header: [`include/agent/scratch.hpp`](../include/agent/scratch.hpp)
Impl: [`src/agent/scratch.cpp`](../src/agent/scratch.cpp)

## Interface

```cpp
class ScratchStore {
 public:
  virtual ~ScratchStore() = default;
  virtual std::optional<Value> get(const std::string& session, const std::string& key) = 0;
  virtual void set(const std::string& session, const std::string& key, Value value) = 0;
  virtual bool remove(const std::string& session, const std::string& key) = 0;
  virtual std::map<std::string, Value> entries(const std::string& session,
                                                const std::string& prefix = "") = 0;
  virtual void clear(const std::string& session) = 0;
};
```

Semantics:

- The `session` argument is opaque to the backend — typically the session id
  exposed by `SessionMemory::session_id()`. Backends MUST keep data isolated
  per session.
- `entries(session, prefix)` returns every key in the session that starts
  with `prefix` (empty `prefix` → every key). Backends return entries
  verbatim; the **internal `__todo:*` filter is policy in the tool layer**,
  not the backend, so a custom backend does not need to know about the todo
  key convention.
- `Value` is the framework's polymorphic JSON value, so backends must be
  able to serialize and rehydrate arbitrary JSON (string, number, bool,
  null, array, object).

## Built-in backends

### `InMemoryScratchStore`

- Default when no backend is supplied to `AgentRunnerConfig`.
- Thread-safe via an internal `std::mutex`.
- Process-local; everything is lost when the process exits.

### `FileScratchStore`

- Zero-dependency persistence. Each session is written to
  `<base_dir>/<url-encoded session_id>.json` using the same JSON helpers
  (`agent::read_json_file` / `agent::write_json_file`) and `encode_uri_component`
  used by `FileSessionStore`.
- Each `set` / `remove` reads the file, mutates the map under the mutex, and
  rewrites the JSON atomically. Suitable for single-process, restart-safe
  agents; for high write throughput or multi-process coordination, plug in a
  database-backed implementation.

```cpp
auto scratch = std::make_shared<agent::FileScratchStore>("./var/scratch");
auto runner = agent::AgentRuntimeBuilder()
                  .model(model)
                  .scratch_store(scratch)
                  .build();
```

## Injecting a custom backend (SQLite, Redis, …)

Subclass `ScratchStore`, implement the five methods, and pass a
`std::shared_ptr<ScratchStore>` through `AgentRuntimeBuilder::scratch_store`:

```cpp
class SqliteScratchStore final : public agent::ScratchStore { /* ... */ };

auto runner = agent::AgentRuntimeBuilder()
                  .model(model)
                  .scratch_store(std::make_shared<SqliteScratchStore>(db))
                  .build();
```

When you bypass the runner and drive tools through a bare `ToolExecutor` (for
tests or embedded integration), set the pointer directly on the execution
context:

```cpp
agent::InMemoryScratchStore store;
agent::ToolExecutionContext ctx;
ctx.service_refs.service_container.set(agent::kToolServiceSessionMemory, &session);
ctx.service_refs.service_container.set(agent::kToolServiceScratchStore, &store);
executor.execute_tool_call(call, ctx);
```

If `scratch_store` is unset when a `scratch.*` or `todo.*` tool fires, the
tool raises a `ConfigurationError` whose message contains both `Scratch` and
`scratch_store` so it is easy to grep in logs:

> Scratch / todo tools require an injected ScratchStore. Set
> AgentRunnerConfig::memory_runtime.scratch_store or kToolServiceScratchStore
> before invoking these tools.

## Lifecycle and threading

- Hosts retain ownership of the backend instance (`shared_ptr` for the
  runner; raw pointer when registered in `ToolServiceContainer`). The framework
  never frees backends it did not allocate.
- Both built-in backends are safe to share across threads. Custom backends
  MUST be thread-safe — tools may be invoked concurrently from parallel
  workflows or multi-agent coordinators.
- The runner accessor `AgentRunner::scratch_store()` returns the underlying
  pointer, primarily for tests that want to inspect state set by tool
  invocations.

## Relationship to other state surfaces

- **SessionMemory** holds chat history and summaries — model-facing state.
- **LongTermMemory / vector stores** hold retrievable embeddings for RAG.
- **ScratchStore** holds short-term key/value notes that the agent
  explicitly writes via `scratch.*` / `todo.*`. It does not get
  automatically appended to the prompt; the agent must read it back via a
  tool call.
