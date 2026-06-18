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

The native port intentionally keeps the NodeJS workspace's package sprawl out
of C++, but it is no longer a single implementation blob. `agent_native` is
the default embeddable runtime aggregate; applications that intentionally want
the complete app/server surface opt into `agent_full`. This page describes the
design rules that the rest of the documentation assumes.

## Design Rules

1. **Layered native targets.** Capabilities are split into `agent_core`,
   `agent_platform`, `agent_model`, `agent_tools`, `agent_mcp`,
   `agent_mcp_native`, `agent_runtime`, `agent_runtime_io`,
   `agent_runtime_io_native`,
   `agent_runtime_modules`, `agent_app`, and `agent_server`. `agent_native` links only the embeddable
   runtime surface. `agent_full` links the complete app/server surface for explicit full-stack
   opt-in. Calling code should include the smallest matching API header:
   `agent/core_api.hpp`, `agent/model_api.hpp`, `agent/tools_api.hpp`,
   `agent/runtime_api.hpp`, `agent/app_api.hpp`, `agent/server_api.hpp`, or
   `agent/full.hpp`.
2. **Zero external dependency.** The library compiles against only the C++20
   standard library. It does not pull in a JSON library, schema library, HTTP
   client, TLS stack, database driver, scripting engine, OpenTelemetry SDK,
   browser engine, or model runtime.
3. **Interfaces over implementations.** Anything that would require an external
   library is exposed as an abstract interface plus a default zero-dependency
   fallback when one is possible. Production bindings are supplied by the
   embedding host through dependency injection.
4. **Descriptor-driven providers.** Configured chat providers are registered
   through `NativeChatProviderDescriptor`; config validation, protocol
   selection, and adapter creation read the descriptor registry instead of a
   hard-coded provider switch. Built-in descriptor registration lives in
   `src/agent/config_provider_registry.cpp`; `config.cpp` stays focused on
   validation and app assembly.
5. **Scoped tool services.** New tools declare `ToolServiceRequirement` entries
   and access dependencies through `ToolServiceView`. Runtime service refs
   remain available for existing built-ins, but custom tools should not depend
   on the full service bag.
6. **Synchronous public API.** Streaming, retries, cancellation, hooks, and
   tool execution are all synchronous from the caller's perspective. No
   coroutine runtime, no `std::async`, no thread pool is required to call any
   public function.
7. **Stable behavior parity with NodeJS where it is observable.** Event names,
   message normalization, default policies, JSON shapes, and error wrapping
   follow the NodeJS framework so that traces, replays, and eval reports stay
   comparable across the two implementations.

## Module Map

| Target | API header | Purpose |
|---|---|---|
| `agent_core` | `agent/core_api.hpp` | Errors, `Value`, messages, HTTP contracts, hooks, observability, shared helpers. |
| `agent_platform` | `agent/http_native.hpp`, `agent/process_hook.hpp` | Optional zero-dependency native platform helpers, currently the POSIX plain-HTTP transport and POSIX process hooks. |
| `agent_model` | `agent/model_api.hpp` | Provider-neutral chat/embedding contracts, model capabilities, usage metadata, and adapter interfaces. |
| `agent_model_providers` | `agent/model_providers.hpp` | Optional built-in provider adapters, request protocol marshalling, stream parsing, reasoning mappers, and native provider transports. |
| `agent_tools` | `agent/tools_api.hpp` | Tool definitions, registry, executor, sandbox, scratch, tool-run services, security governance. |
| `agent_mcp` | `agent/mcp.hpp` | MCP JSON-RPC protocol helpers, callback/line transports, in-process server/client, and tool/resource/prompt adapters. |
| `agent_mcp_native` | `agent/mcp_native.hpp` | Optional native MCP stdio subprocess and plain-HTTP transports. |
| `agent_runtime` | `agent/runtime_api.hpp` | Embeddable runner surface: model/tool wiring, memory, context, execution policies, streaming, and `AgentRunner`. |
| `agent_runtime_io` | `agent/knowledge_io.hpp` and explicit I/O headers | Host I/O runtime modules: browser renderer adapter, web search/fetch/crawl helpers, media/document preprocessing, and web-enabled knowledge loaders. |
| `agent_runtime_io_native` | `agent/web_native.hpp`, `agent/media_native.hpp` | Optional native plain-HTTP web/media helpers. |
| `agent_runtime_modules` | explicit module headers | High-level runtime modules: async runs, tasks, autonomous execution, plan-and-execute, realtime, orchestration, workflow. |
| `agent_app` | `agent/app_api.hpp` | Config resolution, built-in bundles, built-in provider descriptor registry, MCP integrations, CLI/eval/replay helpers, default app assembly. |
| `agent_server` | `agent/server_api.hpp` | Transport-independent server app and route modules. |
| `agent_native` | `agent/agent.hpp` | Embeddable runtime aggregate. Does not link app/server. |
| `agent_full` | `agent/full.hpp` | Full aggregate umbrella for applications that intentionally want every layer. |
| `agent_capi` | `agent_capi.h` | Embeddable C ABI over `agent_runtime`; no app/server/I/O by default. |
| `agent_capi_full` | `agent_capi_full.h` | Full C ABI over `agent_app` for config-backed constructors and async-agent-run modules. |

The umbrella header `agent/agent.hpp` is intentionally limited to the
embeddable runner surface. Higher-level runtime modules such as async runs,
autonomous execution, plan-and-execute, realtime, tasks, workflow, orchestration,
and ReAct expert APIs require explicit headers (`agent/async.hpp`,
`agent/autonomous.hpp`, `agent/plan.hpp`, `agent/realtime.hpp`,
`agent/tasks.hpp`, `agent/workflow.hpp`, `agent/orchestration.hpp`,
`agent/react.hpp`, etc.) and explicit target opt-in. Browser, web, media,
document preprocessing, and web-enabled knowledge loader implementations live
in `agent_runtime_io`; hosts include `agent/knowledge_io.hpp` for web/repository
knowledge loaders. The core-safe default knowledge loader only covers text,
file, directory, markdown, and composite loader composition. New framework code
should include a layer-specific API header or an individual header instead of
depending on the aggregate. Use `agent/full.hpp` only when the full runtime and
app/server surface is intentional.

Memory and knowledge are also separated at the public-header level. The
default embeddable runner sees `memory_retrieval.hpp`, `memory_session.hpp`,
and `knowledge_runtime.hpp`: lightweight ports and value types only. Concrete
vector memory, run transcript, layered memory, loaders, stores, indexes,
rerankers, `KnowledgeBase`, and `KnowledgeBaseManager` live behind explicit
`agent_memory`, `agent_knowledge`, or full/app target opt-in. Host I/O
knowledge loaders stay behind `agent/knowledge_io.hpp` plus the
`agent_runtime_io` target.

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
  plain-HTTP web/media helpers through `agent_runtime_io_native`, POSIX stdio
  subprocess MCP transport through `agent_mcp_native`, PDF text extraction).

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

## Storage Ownership

Built-in stores follow the repository-level vocabulary in
`../../CONTRACT_MATRIX.md`: `runtime-owned`, `derived`, `small-deploy canonical`,
and `business-owned`.

- Session, workflow, task, autonomous, approval, replay, scratch, and audit
  stores are runtime state used to execute, resume, govern, and inspect runs.
- Knowledge stores, vector indexes, text indexes, rerankers, replay
  materializations, and retrieval caches are derived substrate.
- In small local deployments those stores may be the canonical application
  state.
- In host-owned deployments, users, tenants, long-term chat archives, product
  entities, reporting sources, and authorization facts remain business-owned
  and should be exposed through host tools, repositories, or injected adapters.

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

Chat provider config uses the same rule. Embedders add provider support by
calling `register_native_chat_provider_descriptor(...)` with a provider name,
adapter factory, and request-protocol resolver. A custom provider can then be
referenced from JSON config without modifying `config.cpp`; built-in provider
descriptors are registered from the app layer's provider registry source.

## Concurrency and Thread Safety

- Public mutating registries (provider/tool/bundle/search/loader/index/MCP
  registries, in-memory pub/sub, in-memory and file-backed stores) take an
  internal mutex and snapshot for read paths. Reads never block writes longer
  than the snapshot copy.
- `AgentRunner` executes synchronously on the calling thread.
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
