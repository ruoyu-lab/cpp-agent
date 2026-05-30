# Native Agent Framework — Documentation Index

This directory documents the public C++20 surface of `agent_native`. The native
port tracks the TypeScript `@node-agent` workspace in `../NodeJS` but keeps the
framework single-library, zero-dependency, synchronous, and friendly to
embedding from other host languages.

Start here if you are new to the project:

1. [Architecture](architecture.md) — module map, zero-dependency boundary,
   injection model, thread/concurrency model, and how the native port differs
   from the NodeJS workspace.
2. [Build](build.md) — CMake targets, optional features
   (`AGENT_NATIVE_ENABLE_LLAMA_CPP`), tests, and zero-dependency verification.
3. [Bindings](bindings.md) — guidance for exposing `agent_native` to Python,
   Go, Rust, Java, or any other host language through a stable C ABI shim.

## Core Foundations

- [Core](core.md) — errors, `Value`, JSON, JSON Schema, identifiers, base64,
  hashing.
- [Sandbox](sandbox.md) — capability axes (`FsAccess` / `NetAccess` /
  `ProcessAccess` / `SyscallAccess`), `SandboxRequest`, `ToolSandboxPolicy`,
  contract enforcement. Framework defines the abstraction;
  platform implementations (Seatbelt / seccomp / AppContainer / …) live
  in the host.
- [Messages](messages.md) — `AgentMessage`, `MessageContentPart`, tool-call
  envelopes, multimodal normalization.
- [Hooks](hooks.md) — hook sets, ordered merge, lifecycle dispatch surface.
- [HTTP](http.md) — request construction, injected JSON/stream transports,
  zero-dependency plain-HTTP transport, SSE line parsing.
- [Execution](execution.md) — `CancellationToken`, timeout/retry policy,
  `EventBus`, retry scheduled events, realtime sinks.
- [Observability](observability.md) — structured logs/metrics/traces, memory
  collector, console adapter, OpenTelemetry bridge interface.

## Models, Tools, and Media

- [Model](model.md) — chat/text/image embedding interfaces, provider
  registries, fallback chains, OpenAI/Qwen/Anthropic/DeepSeek/Gemini/Ollama
  and llama.cpp-native bindings, reasoning settings.
- [Tools](tools.md) — tool definitions, registry, permission policies,
  executor, audit events.
- [Builtins](builtins.md) — packaged tool bundles (core/data/local/HTTP/web/
  browser/repository/agent/workflow/developer) and bundle metadata.
- [Tool Client](tool-client.md) — tool-client protocol, broker, runtime,
  security policy, idempotency cache, paired/injected transports.
- [MCP](mcp.md) — in-process MCP tools/resources/prompts, JSON-RPC client,
  stdio/HTTP/callback transports, `.mcp.json` loader.
- [Media](media.md) — inline/path/URL/data-URL/artifact resolution, document
  preprocessor pipeline, OCR/rasterizer provider registries.
- [Browser](browser.md) — renderer interface, callback adapter, render/extract/
  screenshot builtin tools.

## Memory and Knowledge

- [Memory](memory.md) — session memory, in-memory/file session stores,
  long-term vector memory, writeback hooks.
- [Scratch](scratch.md) — injectable per-session scratchpad backing the
  `scratch.*` / `todo.*` builtins; in-memory and file backends, custom
  backend guidance.
- [Skills](skills.md) — Anthropic-style skills, filesystem loader, registry,
  automatic selection, fork/subagent bridge.

## Runtime and Orchestration

- [Multi-Agent Patterns](multi-agent.md) — the six multi-agent patterns this
  framework supports out of the box (skill subagent, workflow agent node,
  seven coordinators, tool-client remote, mailbox, autonomous). Decision
  tree, templates, and anti-patterns.
- [Runtime](runtime.md) — overview of `AgentRunner` and `AgentLoop`. Pages
  below cover individual surfaces:
  - [Runtime Inputs](runtime-inputs.md) — string/content-part/`AgentMessage`
    entrypoints.
  - [Runtime Memory](runtime-memory.md) — session and long-term memory
    integration.
  - [Runtime Planning](runtime-planning.md) — static and model planners,
    plan reporting.
  - [Runtime Streaming](runtime-streaming.md) — runner/loop stream events,
    tool failure semantics, cancellation, traces, hooks.
- [Context](context.md) — embedded context registry, planning render.
- [Workflow](workflow.md) — node registry, builders, engine, stores, lifecycle
  hooks, child-agent bindings.
- [Orchestration](orchestration.md) — artifacts, shared state, mailbox,
  plan/act/observe coordinator, supervisor/evaluator coordinators.
- [Autonomous](autonomous.md) — autonomous run/step/event model, planner and
  executor wiring, manager, persistence.
- [Tasks](tasks.md) — task model, in-memory/file/Postgres stores, queues,
  workers, BullMQ-compatible adapter.
- [Realtime](realtime.md) — in-memory pub/sub, EventBus realtime sinks,
  Redis-compatible adapter.

## Server, CLI, Evals, Replay

- [Server](server.md) — transport-independent app, agent/task/autonomous/
  workflow routes, governance, audit log, replay integration. No HTTP
  listener; hosts own the transport.
- [CLI](cli.md) — `native_agent_cli`, config validation, chat/REPL, knowledge
  sources, skills, MCP, eval suite execution.
- [Evals](evals.md) — eval suites, assertion catalog, runner integration,
  JSON/Markdown reports, baselines, replay-per-case.
- [Replay](replay.md) — HTML/materialization helpers, manifest loading,
  session run listing.

## Config

- [Config](config.md) — native JSON config loader, env-key auditing,
  resource refs, planner/knowledge/web/skill/MCP/server wiring, optional
  injected JS/TS module loader.
