# Architecture

## What this is

`agent_native` is an **agent harness** — the deterministic scaffolding around
an LLM that turns it into a usable system. Tool registry, context manager,
planner, permission gate, sandbox, memory store, event bus, retry/timeout
policies, trace recorder. The model itself is one injectable adapter behind
the `ChatModelAdapter` interface; the rest of this library (and ~99% of the
code) is harness.

Research into other coding-agent harnesses puts the split at ~1.6% AI
decision logic and ~98.4% harness code. This project is structured the same
way on purpose: when something goes wrong in a deployed agent, the cause is
overwhelmingly in the harness, and the harness is what you can actually fix.

The native port intentionally diverges from the NodeJS workspace's package
split. Instead of dozens of npm packages, `agent_native` ships as **one static
library with one public include root**. This page describes the design rules
that the rest of the documentation assumes.

## Design Rules

1. **Single library.** All capabilities live in `agent_native`. There is no
   plugin loader, dynamic module system, or runtime package discovery. Calling
   code links the static library and includes `agent/agent.hpp` (or any of the
   focused headers).
2. **Zero external dependency.** The library compiles against only the C++20
   standard library. It does not pull in a JSON library, schema library, HTTP
   client, TLS stack, database driver, scripting engine, OpenTelemetry SDK,
   browser engine, or model runtime.
3. **Interfaces over implementations.** Anything that would require an external
   library is exposed as an abstract interface plus a default zero-dependency
   fallback when one is possible. Production bindings are supplied by the
   embedding host through dependency injection.
4. **Synchronous public API.** Streaming, retries, cancellation, hooks, and
   tool execution are all synchronous from the caller's perspective. No
   coroutine runtime, no `std::async`, no thread pool is required to call any
   public function.
5. **Stable behavior parity with NodeJS where it is observable.** Event names,
   message normalization, default policies, JSON shapes, and error wrapping
   follow the NodeJS framework so that traces, replays, and eval reports stay
   comparable across the two implementations.

## Module Map

| Layer | Headers | Purpose |
|---|---|---|
| Foundations | `core`, `messages`, `common` | Errors, `Value`, JSON, schema, identifiers, message protocol. |
| Plumbing | `http`, `execution`, `hooks`, `observability` | Transport contracts, cancellation/retry/timeout, event bus, hook merge, logs/metrics/traces. |
| Domain | `model`, `tools`, `tool_client`, `mcp`, `media`, `browser` | Provider abstractions, tool registries, MCP client, document/media handling, browser renderer interface. |
| Knowledge | `memory`, `skills` | Session/long-term memory, knowledge bases/managers, Anthropic-style skills. |
| Runtime | `context`, `runtime`, `workflow`, `orchestration`, `autonomous`, `tasks`, `evals`, `replay` | Agent loop/runner, workflow engine, multi-agent coordinators, autonomous loop, background tasks, eval suites, replay artifacts. |
| Surface | `server`, `cli`, `config`, `builtins` | Transport-independent server app (routes/governance/audit/replay), CLI binary, config loader, packaged tool bundles. |

The umbrella header `agent/agent.hpp` pulls every public header. Headers are
self-contained — including any single one transitively brings in `core` and the
common standard-library set in `common.hpp`.

## Zero-Dependency Boundary

Anything that **must** touch the outside world is split in two:

- A **pure interface** owned by the framework (e.g. `HttpTransport`,
  `BrowserRenderer`, `OcrProvider`, `DocumentRasterizer`, `MCPTransport`,
  `WorkflowWebhookTransport`, `RealtimeClient`, `TaskQueueClient`,
  `AutonomousStoreClient`, `ApprovalStoreClient`, `NativeConfigModuleLoader`,
  llama.cpp-native binding adapter, embedding adapter, search provider, page
  fetcher, …).
- A **fallback implementation** when a useful one exists with only the C++
  standard library and POSIX (e.g. native HTTP/1.1 plain-text client
  transport, in-memory pub/sub, in-memory/file-backed stores, native
  plain-HTTP web search/page-fetch transports, POSIX stdio subprocess MCP
  transport, PDF text extraction).

What is **not** included out of the box:

- TLS / HTTPS (any direction).
- A real browser engine (Playwright/Chromium/etc.).
- Real OCR (Tesseract) or PDF rasterizer (PDFium/pdf.js).
- CLIP / Transformers model execution.
- Production network clients for Redis, BullMQ, Postgres, pgvector, sqlite.
- JS/TS config module execution.
- SSE/WebSocket MCP transports.

These are reached by **injection only**. This is intentional: every embedding
host already brings its preferred TLS stack, ORM, browser binding, model
runtime, or scripting engine. Forcing one would break the "easy to host from
any language" goal.

## Injection Model

Every injection point is a plain abstract base class (or `std::function`
callback) on the public API. There is no DI container, no service locator, and
no global state required to wire one in.

The common shapes are:

```cpp
// 1. Pure callback adapter — preferred for one-off integrations.
agent::ModelAdapter model = agent::make_callback_chat_model(
    [](const agent::GenerateParams& params) -> agent::GenerateResult {
      // Call your provider here.
      return agent::GenerateResult{...};
    });

// 2. Interface implementation — preferred for stateful adapters
//    (subprocess lifetimes, connection pools, drivers).
class MyTlsTransport : public agent::HttpTransport {
 public:
  agent::HttpResponse send(const agent::HttpRequest& request,
                           agent::CancellationToken* token) override {
    // Implement using your TLS stack of choice.
  }
};

// 3. Config-driven factory — registered on the config resolver so loaded
//    JSON configs can refer to an adapter by name.
config.set_native_mcp_transport_factory(make_my_mcp_transport_factory());
```

Registries (model providers, tool bundles, search providers, knowledge
loaders/rerankers/text-indexes/vector-indexes, OCR providers, document
rasterizers, MCP transports, autonomous/approval stores, …) are all
synchronized at registration time and snapshot at execution time, so adapters
can be added or rotated without races.

## Concurrency and Thread Safety

- Public mutating registries (provider/tool/bundle/search/loader/index/MCP
  registries, in-memory pub/sub, in-memory and file-backed stores) take an
  internal mutex and snapshot for read paths. Reads never block writes longer
  than the snapshot copy.
- `AgentRunner` and `AgentLoop` execute synchronously on the calling thread.
  Streaming is implemented as ordered event collection during that synchronous
  run.
- `CancellationToken` is cooperative. Long-running adapters poll the token and
  propagate cancellation to model/embedding/tool/HTTP transports.
- Observability sinks are isolated: an adapter that throws does not break the
  pipeline or sibling sinks.

## Error Model

All framework-specific exceptions derive from `agent::AgentFrameworkError`.
See [Core](core.md) for the specialized error types. Adapters that bubble
external failures should wrap them with `agent::AdapterError` to preserve
context without leaking external types into the framework error surface.

## Defenses Against the Three Classical Harness Defects

Post-mortems from production agent deployments trace the majority of failures
to one of three patterns. The framework's defenses are layered so a single
failed layer doesn't cause a system-level failure.

### 1. Context Drift

Older turns crowd out fresh signal; the agent forgets recent decisions or
contradicts itself across long sessions.

- `SessionMemory` exposes `auto_compact_at` (default `0.8`) and a
  `token_budget`. When estimated usage crosses the threshold, the loop calls
  `compact()` automatically and emits `session.auto_compact` on the event
  bus for observability.
- `SessionMemorySummarizer` is pluggable so hosts can substitute their own
  summarizer (e.g. an LLM-backed one).
- `truncate_for_model` in `detail/helpers.hpp` keeps the head and tail of
  large tool outputs (default 100/100 lines, 16 KiB cap) and replaces the
  middle with a clear elision marker.
- The truncated envelope serialized as
  `{ text, truncated, totalLines, omittedBytes, kept_bytes }` lets the agent
  know there's more on disk and ask for it explicitly if needed.
- `scratch.*` and `todo.*` tools give the agent a place to externalize
  working state instead of stuffing it into message history.

### 2. Schema Misalignment

The agent emits something the tool schema can't accept, or the schema is
loose enough that bad inputs slip through and the tool fails opaquely.

- `JsonSchema` supports `oneOf`, `anyOf`, `allOf`, `pattern`, `const`,
  `enum_values`, `min/max length`, numeric bounds, and typed
  `additional_properties_schema`. `normalize_json_schema` applies Node-style
  defaults (closed objects unless explicitly opened).
- `assert_json_schema` throws a `SchemaValidationError` with all violation
  paths collected, not just the first.
- `tool.search` + `tool.describe` (built-in `core` tools) plus
  `ToolRegistry::lazy_mode` let the agent **discover** tools instead of
  carrying every schema in the system prompt. This keeps the visible tool
  set small (~10 focused tools), which is empirically better than a flat
  registry of dozens.
- Structured-output grammar generation (llama.cpp-native) constrains the
  decoder to a schema-compatible token space.

### 3. State Degradation

Long-running or multi-agent runs corrupt files, repeat work, or take
unrecoverable actions when no one is looking.

- `AgentRunnerDurableState` checkpoints capture full loop state at iteration
  boundaries; runs can be resumed cleanly after crashes or host migration.
- `fs.writeText` accepts `expectedSha256` and refuses with
  `{ ok: false, reason: "stale_read", actualSha256 }` if the file changed
  since the agent last observed it.
- `git.snapshot` (commits to a shadow ref `refs/agent/snapshots` without
  polluting the working branch) plus `undo.last` give cheap, reversible
  rollback for write-tool sequences. Wire `git.snapshot` as a
  `before_fs_write` hook for automatic per-edit snapshots.
- `ProcessHook` (POSIX) lets external scripts gate tool calls: exit code 2
  blocks the call with the script's stderr as the denial reason. This makes
  organization-wide policies enforceable from any language.
- The default hook logger emits successful gate decisions at trace level and
  failed/blocked decisions at warn level, so feedback signal-to-noise stays
  high (`success silent / failures verbose`).

## Relationship to NodeJS

| Aspect | NodeJS workspace | C++ native port |
|---|---|---|
| Distribution | ~70 npm packages | One static library + one CLI binary. |
| Dependencies | Many npm deps (undici, zod, etc.). | C++20 standard library only. |
| Async | Promises / streams. | Synchronous + `CancellationToken`. |
| Plugins | Package boundary. | Interfaces with injected adapters. |
| Behavior parity | Source of truth. | Tracks Node behavior where observable; flagged "Node-style" in code/comments. |

See [`PORTING_STATUS.md`](../PORTING_STATUS.md) for a per-module map of which
NodeJS area maps to which C++ module(s) and what remains.
