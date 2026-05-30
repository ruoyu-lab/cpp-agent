# Native Agent Framework

This directory is the parallel native C++ version of the TypeScript `@node-agent` workspace in `../NodeJS`.

Current migration stage:

- Pure C++20, standard library only.
- Static library target: `agent_native`.
- CLI executable target: `native_agent_cli`.
- No plugin loader, dynamic module system, Node runtime, npm package, or external dependency.
- Public include entrypoint: `include/agent/agent.hpp`.
- Internal modules: `include/agent/*.hpp` and `src/agent/*.cpp`.
- Ported first-pass core surfaces:
  - value / JSON-like payloads
  - schema validation, including Node-style top-level object schema normalization and typed `additionalProperties` schemas
  - message and multimodal content protocol, including Node-style message/content/tool-call normalization
  - model and text/image embedding abstractions, including fallback chat model adapters
  - provider registries, default model stream events, hash text/image embeddings, injected CLIP text/image embeddings, embedding cancellation propagation, Node-style provider config default model/env resolution, config-created injected llama.cpp-native chat/text embedding adapters with tool-envelope/structured-output grammar request support, and native OpenAI/Qwen/Ollama/Gemini text embedding adapters
  - tool definitions, registry, permission policy builders, approval handlers, execution, Node-style result-envelope message metadata, `tool.audit` lifecycle events, and `permission.checked` allow events
  - event bus and trace context
  - session memory and long-term vector memory
  - session snapshot and knowledge search result serialization
  - knowledge text/file/directory/markdown/repository/GitHub/web/website/sitemap loader surfaces, loader/chunker/reranker/text-index/vector-index provider registries, GitHub ref URL encoding, native PDF text extraction, browser-rendered website ingestion, in-memory/file-backed knowledge stores, native memory/minisearch text indexes, namespace-aware in-memory/file-backed/Qdrant/client-backed pgvector vector indexes, text/image cross-modal ingest/search with Node-style per-base search defaults and production preset defaults, vector/lexical top-K, retrieval mode, oversampling, fusion, modality-weight options, candidate debug output, knowledge-base title citations, configurable context titles, full search-hit record serialization, and build-context-message results, direct ingest dedupe/skip-unchanged semantics, ingestion cancellation and embedding batch control, ingestion pipeline with pluggable dedupe/incremental/retry strategies, sync state stores/jobs including config-triggered sync and delete-missing handling, KnowledgeBaseManager manifest/search lifecycle, config wiring, and heuristic/basic/overlap/hybrid/recency/MMR rerankers
  - file-backed session, vector, task, approval, artifact, and shared-state stores, plus injected-Postgres approval persistence
  - embedded context
  - execution plan rendering, JSON normalization/parsing/serialization, static planners, model-backed planners, and config-created planner wiring
  - agent loop and runner, including runner planner injection/result reporting/stream planning events, automatic knowledge-base context retrieval with knowledge hit/debug propagation, AgentLoop and AgentRunner durable checkpoint/resume state, string/content-part/full `AgentMessage` runtime input entrypoints with message metadata preservation, CancellationToken propagation through runner/loop/model/retrieval/tool execution with active model cancellation callbacks, timeout enforcement and retry scheduling events for model/retrieval/tool execution policies, run lifecycle EventBus events, stream status events for knowledge/memory/planning/model/tool stages, synchronous stream-event execution for model deltas, tool progress, and aggregate tool-batch events, lifecycle hook dispatch including retrieval hooks/events, and merged typed/JSON tool execution services with runner-bound knowledge references
  - workflow state/checkpoint/signal serialization, node registry, built-in start/transform/tool/agent/condition/human-wait/artifact/router/join/webhook-wait/end handlers, direct `workflow-node-human` and injected-transport `workflow-node-webhook` handler factories, builder helpers, lifecycle hooks, in-memory/file-backed stores, callback and AgentRunner child-agent bindings, persisted resume, and native config engine wiring
  - core and developer builtin tools
  - browser renderer interfaces, callback-backed renderer adapter, and browser render/extract/screenshot builtin tools
  - hook sets and ordered hook merging
  - HTTP helper request construction, injected JSON/stream transports, zero-dependency native HTTP/1.1 client transport for plain HTTP, provider HTTP transport bridge, line parsing, and SSE event parsing
  - usage extraction and cost estimation
  - in-memory realtime pub/sub with replay, static/dynamic EventBus realtime sinks, and Value-sequence pipe helpers, plus injected Redis-compatible realtime pub/sub semantics
  - observability pipeline with structured logs, metrics, traces, memory collector, console adapters, and OpenTelemetry bridge interfaces
  - in-memory/file/injected-Postgres task store, queue, lease, checkpoints, events, worker, synchronized task mutations, active worker cancellation propagation, and injected BullMQ-compatible push queue/worker semantics
  - autonomous run model, in-memory/file/injected-Postgres stores, static and AgentRunner-backed planners, callback and AgentRunner-backed step executors, run manager, waiting/resume/cancel/checkpoint flow
  - tool-client protocol foundation with registry, broker, runtime, security checks, idempotency cache, async broker timeout/cancel handling, runtime-side cooperative cancellation, in-memory transport, and injected Socket.IO-compatible transport semantics
  - orchestration artifacts, shared state, mailbox, routers, plan-act-observe coordinator with replanning strategies, evaluator-loop coordination, supervisor/worker/evaluator coordination, and workflow-backed/supervisor-workflow coordination
  - media resolution for inline/path/file URL/data URL/artifact sources, zero-dependency plain-HTTP document media fetch, document preprocessors, native PDF text extraction, OCR provider registry, and document rasterizer registry
  - static, injected, and zero-dependency native web search providers with Node-style provider query parameters, API-key/default-env validation, and host-aware domain filtering, native registered/file/injected/plain-HTTP page fetcher with Node-style default timeout and extract-mode field semantics, browser-backed fetch fallback, robots/domain-aware sitemap-discovering crawler with Node-style default page limits, result shape, failure propagation, and absolute link output, and web config wiring
  - Anthropic-style skills parsing with typed frontmatter metadata, filesystem loading, registry, Node-style argument/env prompt rendering, runtime/config activation, deterministic automatic selection, fork/subagent bridge hooks, and allowed-tools preapproval
  - in-process MCP tools/resources/prompts including multi-content resource reads, prompt argument metadata, tool summary, structured-value rendering helpers, tool error content/_meta preservation, default object schemas for schema-less remote tools, JSON-RPC client with string/numeric id normalization, line/callback/HTTP-injected transports, native plain-HTTP JSON-RPC POST transport for URL-based `.mcp.json` servers, Anthropic `.mcp.json` loading, config resolver wiring, and MCP tool/resource/prompt adapters
  - run replay HTML/materialization helpers, Node-style timestamped replay write options/result paths, optional-result manifest loading, manifest/HTML-or-directory replay loading, stream status/tool-call summaries, and session run listing
  - eval suite execution with per-case model settings, basic assertions, JavaScript-style regex output assertions, JSON Schema structured-output assertions, status-stage and permission assertions/reporting including knowledge-retrieval stages, cancellation propagation, serializable assertion descriptors, JSON reports, Markdown reports, and baseline comparison
  - native CLI executable foundation with config validation, config-backed prompt chat, manual provider/model chat with system prompts, persistent sessions, local/URL knowledge sources, retrieval debug output, project/user Anthropic skills, Anthropic `.mcp.json` tools, environment-driven web search providers, streaming terminal output with `--no-stream` fallback, interactive REPL commands, eval suite execution/reporting with usage/cost assertions and Node-style per-case replay HTML paths, and replay list/show commands
  - transport-independent server app foundation (no HTTP listener; hosts own the transport) with route matching, JSON/SSE responses, CORS, bearer/API-key auth, API-key agent allowlists, fixed-runner, createRunner-style, and config-backed cached per-agent runner/workflow sources, task/autonomous owner and tenant access scopes, API-key quotas, rate limiting, route/agent/API-key metrics with usage and estimated cost, request/trace ID headers and execution-context propagation, JSONL audit log persistence, modelSettings reasoning validation, PII/schema/citation governance for AgentRunner responses, replay materialization for chat/stream responses, AgentRunner chat/stream routes, session inspection/deletion routes, approval list/resolve routes with manual approval queue/store support and injected-Postgres approval store semantics, task runtime routes including Node-style idempotency headers, session policy, modelSettings validation, and cancel/delete semantics, autonomous runtime routes with Node-style create/lifecycle statuses and strict optional-field validation plus injected-Postgres store semantics, and workflow run/resume/signal routes
  - JSON parsing and file read/write helpers
  - native JSON agent config validator/resolver/loader for pure C++ apps, including NodeJS default config filename discovery order, injected host-runtime loading for JS/TS config modules through direct app loading, server config paths, and programmatic CLI runs, referenced environment-variable auditing, model reasoning schema checks, model fallback chains with required-capability negotiation, knowledge store/text-index/vector-index/chunking/ingest option validation including sqlite vector dimension/embedder consistency, Node-style knowledge production preset defaults and sync ingestion, config-created text/image knowledge embedders, builtin/MCP/web/browser tools, stdio MCP subprocess servers, runner-bound knowledge bases/managers with retrieval options, workflow engines, static/model planners, skills, and permissions
  - OpenAI-compatible, Qwen, Anthropic, DeepSeek, Ollama, Gemini, and injected llama.cpp-native provider request serialization and response parsing through native transport callbacks, OpenAI/Qwen Responses API request/stream parsing for reasoning and non-text inputs, provider transport cancellation propagation for chat and embeddings, llama.cpp-native chat/embedding request cancellation forwarding, Node-style tool argument normalization, reasoning settings validation/merge/budget normalization and non-stream/stream tagged reasoning extraction, plus provider-native chat stream parsing through injected stream transports
  - core `http.fetch` and JSON query helpers including array-index paths, data, local filesystem, injected-transport HTTP, injected process developer, browser render/extract/screenshot, web search/fetch/crawl, repository, agent memory/knowledge, and workflow builtin tool bundles with registry metadata, bundle-level tag/capability merging, and context-service fallback

Build:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Examples:

- [`examples/`](examples/README.md) — runnable single-file programs for the
  most common flows (runner, tools+permissions, RAG, workflow, evals). All
  use the built-in echo model so they run with zero external configuration.

API documentation:

- [Documentation index](docs/README.md)
- [Architecture](docs/architecture.md)
- [Build](docs/build.md)
- [Host-language bindings](docs/bindings.md)
- [Runtime overview](docs/runtime.md)
- [Autonomous API](docs/autonomous.md)
- [Browser API](docs/browser.md)
- [Builtins API](docs/builtins.md)
- [CLI API](docs/cli.md)
- [Config API](docs/config.md)
- [Context and planning API](docs/context.md)
- [Core API](docs/core.md)
- [Evals API](docs/evals.md)
- [Execution API](docs/execution.md)
- [Hooks API](docs/hooks.md)
- [HTTP API](docs/http.md)
- [Media API](docs/media.md)
- [MCP API](docs/mcp.md)
- [Memory API](docs/memory.md)
- [Messages API](docs/messages.md)
- [Model API](docs/model.md)
- [Observability API](docs/observability.md)
- [Orchestration API](docs/orchestration.md)
- [Realtime API](docs/realtime.md)
- [Replay API](docs/replay.md)
- [Runtime input API](docs/runtime-inputs.md)
- [Runtime memory API](docs/runtime-memory.md)
- [Runtime planning API](docs/runtime-planning.md)
- [Runtime streaming API](docs/runtime-streaming.md)
- [Server API](docs/server.md)
- [Skills API](docs/skills.md)
- [Tasks API](docs/tasks.md)
- [Tool Client API](docs/tool-client.md)
- [Tools API](docs/tools.md)
- [Web API](docs/web.md)
- [Workflow API](docs/workflow.md)

Optional llama.cpp native binding:

```bash
cmake -S . -B build-llama \
  -DAGENT_NATIVE_ENABLE_LLAMA_CPP=ON \
  -DLLAMA_CPP_INCLUDE_DIR=/path/to/llama.cpp/include \
  -DGGML_INCLUDE_DIR=/path/to/llama.cpp/ggml/include
cmake --build build-llama
```

The default build keeps the framework zero-dependency and uses a stub binding that reports a clear
configuration error. Enabling `AGENT_NATIVE_ENABLE_LLAMA_CPP` compiles the built-in binding; at
runtime provide `modelPath` plus `libraryPath` or `libraryDir`. Image input additionally requires
`mmprojPath` and a llama.cpp build that provides `libmtmd`.

The port intentionally keeps capabilities in one native library instead of reproducing the NodeJS workspace's plugin package split. External service adapters from the TypeScript version are represented by C++ interfaces first; native provider implementations can be added behind those interfaces without introducing plugins.
# cpp-Agent
