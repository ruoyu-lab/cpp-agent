# Builtins API

The native builtins module provides factory functions for NodeJS-style builtin
tool bundles while keeping host-sensitive execution behind injected callbacks or
service references. The declarations live in `agent/builtins.hpp`.

## Bundle Registry

Use `ToolBundleRegistry` when an app wants to expose tool bundles by name:

```cpp
auto registry = agent::create_builtin_tool_bundle_registry();
auto tools = registry.create_tools({"core", "local", "http"});
```

`ToolBundleRegistry` supports:

- `register_provider`
- `get`
- `find`
- `list`
- `create_tools`

`register_provider` requires `metadata.name`. Providers are listed in
registration order, and registering the same name replaces the provider without
moving it in the order.

When `create_tools` receives an empty bundle list, it creates tools from every
registered provider. When a list is supplied, unknown names are skipped. Tool
names are deduplicated in selection order. Bundle metadata is applied to each
tool, including the bundle name, `bundle:<name>` tag, bundle tags, and bundle
capabilities.

## Metadata

`ToolBundleMetadata` describes bundle presentation and policy defaults:

- `name`
- `tier`
- `title`
- `description`
- `tags`
- `capabilities`
- `default_risk_profile`

The default registry exposes these bundles:

- `core`: time, ids, JSON path query, math evaluator, text diff/patch, plus
  `tool.search` / `tool.describe` for lazy tool discovery (see `docs/tools.md`).
- `local`: local text file read/write and directory listing. The `fs.*` tools
  are **confined to the process working directory by default**: a `path`
  resolving outside the allowed roots is rejected (path-traversal defense).
  Pass `create_local_builtin_tools(allowed_roots)` to widen the roots, or
  `create_local_builtin_tools({}, /*allow_unconfined=*/true)` to disable
  confinement entirely. `fs.writeText` also accepts an optional `expectedSha256`
  field; when supplied, the write is refused with
  `{ ok:false, reason:"stale_read", actualSha256 }` if the on-disk contents have
  changed since the agent last read them.
- `http`: injected or native plain-HTTP request helpers. `http.request`
  and other large-output tools return body text wrapped as
  `{ text, truncated, totalLines, omittedBytes, kept_bytes }` once the body
  crosses the truncation threshold (default 16 KiB).
- `developer`: injected process execution via `shell.exec`. Also exposes
  `git.snapshot` / `undo.last`, which record working-tree snapshots into a
  private `refs/agent/snapshots` ref and restore from it. These tools require
  `git` in PATH; when git is unavailable they return
  `{ ok:false, reason:"git_not_available" }`. Use the
  `HookSet::before_fs_write` hook to attach `git.snapshot` as a pre-write
  hook for `fs.writeText`.
- `browser`: injected browser render, extract, and screenshot tools.
- `web`: configured search and fetch tools. `web.fetch` returns the
  page's `text` field wrapped in a truncated-output envelope when the
  extracted text exceeds the threshold.
- `agent`: session snapshot and knowledge search helpers.
- `workflow`: workflow run inspection, resume, and checkpoint listing.
- `state`: per-session scratchpad (`scratch.*`) and todo list (`todo.*`) tools
  for externalizing agent working memory across iterations.
- `media`: image, video, and audio generation tools backed by
  `MediaGenerationProviderRegistry`. The tools validate through the selected
  provider, forward cancellation and progress events, and return
  artifact-friendly asset values instead of large inline base64.

### Media generation

Register media generators separately from chat models:

```cpp
agent::MediaGenerationProviderRegistry media;
media.register_provider(std::make_shared<MyMediaGenerator>());

agent::InMemoryArtifactStore artifacts;
agent::MediaGenerationToolOptions options;
options.output_options.artifact_writer =
    agent::create_in_memory_media_artifact_writer(&artifacts);
options.output_options.artifact_key_prefix = "media/generated";

agent::ToolRegistry tools(agent::create_media_generation_builtin_tools(&media, options));
```

The builtin bundle exposes:

- `media.generateImage`
- `media.generateVideo`
- `media.generateAudio`

Hosts can also create the generic `media` bundle and provide the registry at
execution time through `ToolExecutionServices::media_generation_registry`.

### Output truncation envelope

`shell.exec`, `http.request`, `web.fetch`, and `fs.readText` all
return their large text payloads through a stable envelope:

```jsonc
{
  "text": "...head...\n... [N lines / M bytes omitted] ...\n...tail...",
  "truncated": true,
  "totalLines": 500,
  "omittedBytes": 12345,
  "kept_bytes": 4096
}
```

When the original output already fits, `truncated` is `false` and `text`
contains the full payload. The helper lives in
`src/agent/detail/helpers.hpp` as `truncate_for_model()`.

Call `list_builtin_tool_bundle_metadata()` to inspect the default metadata
without creating tool instances.

## Factory Functions

Use a focused factory when the host wants to inject only the services for one
bundle:

```cpp
auto http_tools = agent::create_http_builtin_tools(my_transport);
auto browser_tools = agent::create_browser_builtin_tools(&renderer);
auto agent_tools = agent::create_agent_builtin_tools(
    &session,
    &knowledge_base,
    nullptr);
```

Available factories:

- `create_core_builtin_tools`
- `create_local_builtin_tools`
- `create_http_builtin_tools`
- `create_developer_builtin_tools`
- `create_browser_builtin_tools`
- `create_web_builtin_tools`
- `create_agent_builtin_tools`
- `create_workflow_builtin_tools`
- `create_state_builtin_tools`
- `create_builtin_tool_bundle_providers`
- `create_builtin_tool_bundle_registry`
- `create_builtin_tools`

`create_builtin_tools()` defaults to the `core` bundle.

## Injection And Service Tokens

Host-sensitive builtins accept injected services directly or resolve them from
declared service tokens on `ToolExecutionContext::service_refs.service_view`:

- `shell.exec` requires a `DeveloperProcessExecutor`.
- Browser tools use the injected `BrowserRenderer*` or
  `kToolServiceBrowserRenderer`.
- `web.search` uses an injected `WebSearchProviderRegistry*` or
  `kToolServiceWebSearchRegistry`.
- `web.fetch` uses an injected `NativeWebPageFetcher*`,
  `kToolServiceWebFetcher`, or the native zero-dependency page fetcher.
- Agent tools use injected memory/knowledge services or service tokens.
- Workflow tools use an injected `WorkflowEngine*` or
  `kToolServiceWorkflowEngine`.

HTTP builtins use the provided `HttpTransport`. If none is provided,
`create_native_http_transport()` is used, which supports plain `http://` URLs.
Use an injected transport for HTTPS/TLS, proxy policy, authentication, or
organization-specific network controls.

Cancellation is propagated from `ToolExecutionContext::cancellation` into HTTP,
developer process, browser, web search, web fetch, and web crawl requests.

## Tool Names

Current builtins include:

- Core: `time.now`, `uuid.generate`, `json.query`, `math.eval`, `text.diff`,
  `text.patch`, `tool.search`, `tool.describe`.
- Local: `fs.readText`, `fs.writeText`, `fs.listDirectory`.
- HTTP: `http.request`, `http.inspectJson`.
- Developer: `shell.exec`, `git.snapshot`, `undo.last`.
- Browser: `browser.render`, `browser.extract`, `browser.screenshot`.
- Web: `web.search`, `web.fetch`.
- Agent: `session.snapshot`, `knowledge.search`.
- Workflow: `workflow.getRun`, `workflow.resume`,
  `workflow.listCheckpoints`.
- State: `scratch.set`, `scratch.get`, `scratch.list`, `scratch.delete`,
  `todo.create`, `todo.update`, `todo.list`, `todo.complete`.

## Core tool details

- `math.eval` — `{ expression }` → `{ value, expression }`. Shunting-yard
  evaluator with no I/O. Supports `+ - * / % ^` and functions
  `abs/sqrt/cbrt/exp/log/log2/log10/sin/cos/tan/asin/acos/atan/sinh/cosh/tanh/floor/ceil/round/trunc/sign/pow/atan2/hypot/mod/min/max`
  plus constants `pi`, `e`, `tau`. Throws `ConfigurationError` on bad input.
  Example: `{"expression": "2+3*4"}` → `{"value": 14, ...}`.
- `text.diff` — `{ before, after, labelBefore?, labelAfter? }` →
  `{ diff, hasChanges }`. Returns a Myers unified diff with 3 lines of context.
- `text.patch` — `{ source, patch }` → `{ text, conflicts, appliedCleanly }`.
  Applies a unified diff back to a source. Conflicts are reported per hunk
  without aborting; `appliedCleanly` is false when any hunk failed.

### State bundle (`state`)

These tools externalize an agent's working memory so it survives across
iterations without consuming the context window. Storage is routed through
an injected `ScratchStore` backend (see [Scratch](scratch.md)) — by default
the runner constructs an `InMemoryScratchStore`, but hosts can supply
`FileScratchStore` for restart-safe persistence or any custom implementation
(SQLite, Redis, …) by setting `AgentRunnerConfig::memory_runtime.scratch_store` or
`kToolServiceScratchStore`. When invoking these tools through a
raw `ToolExecutor` without going through `AgentRunner`, the caller is
responsible for registering `kToolServiceScratchStore` in
`ctx.service_refs.service_container`; otherwise the tool raises
`ConfigurationError` mentioning both `Scratch` and `kToolServiceScratchStore`.

State is keyed by `SessionMemory::session_id()` (or the literal `_` when no
session is attached to the execution context). Todo lists live under a
reserved `__todo:` key prefix and are hidden from `scratch.list`; the filter
is applied at the tool layer so custom backends can store entries verbatim.

- `scratch.set` — `{ key, value }` → `{ ok, key }`. `value` accepts any JSON
  type, including nested objects and arrays.
- `scratch.get` — `{ key }` → `{ key, value, found }`. Missing keys return
  `value: null` and `found: false`.
- `scratch.list` — `{ prefix? }` → `{ entries: [{ key, value }] }`. Skips
  internal keys.
- `scratch.delete` — `{ key }` → `{ deleted }`.
- `todo.create` — `{ items: [{ text, status? }] }` →
  `{ items: [{ id, text, status, createdAt, updatedAt }] }`. Status defaults
  to `pending`. Valid statuses: `pending | in_progress | completed | cancelled`.
- `todo.update` — `{ id, status?, text? }` → `{ item }`.
- `todo.list` — `{ status? }` → `{ items }`. Filters by status when provided.
- `todo.complete` — `{ id }` → `{ item }`. Convenience wrapper for
  `todo.update` with `status: "completed"`.

## Zero-Dependency Boundary

The builtin factories create native `ToolDefinition` objects only. They do not
link shell-management libraries, browser engines, cloud search SDKs, database
clients, TLS stacks, or a JavaScript runtime. Those capabilities remain
host-provided through the injected interfaces above.
