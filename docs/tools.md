# Tools API

The native tools module mirrors the NodeJS tool-definition and execution
contract while keeping production integrations injectable. Use
`create_builtin_tool_bundle_registry()` to get the core, local, HTTP, web,
browser, agent, workflow, state, and developer bundle providers, or call a
specific factory when only one bundle is needed.

```cpp
auto registry = agent::ToolRegistry(agent::create_core_builtin_tools());
auto executor = agent::ToolExecutor(registry);

auto result = executor.execute_tool_call(agent::ToolCall{
    .id = "eval-1",
    .name = "math.eval",
    .arguments = agent::Value::object({{"expression", "2+3*4"}}),
});
```

`http.inspectJson` is intentionally strict: the response body must parse as
JSON, matching the NodeJS builtin's `response.json()` behavior. Use
`http.request` when callers need raw text bodies or non-JSON responses.

`web.fetch` can run without an injected fetcher for local `file://` URLs and
plain `http://` URLs by using the native zero-dependency page fetcher. Inject a
custom fetcher when HTTPS/TLS, browser-backed extraction, or authentication is
required.

For HTTP, web, browser rendering, developer process execution, workflow
access, and MCP remotes, pass the existing zero-dependency callback
interfaces or registries into the bundle factories. The native layer does not
link production network, browser, database, or process-management SDKs by
default.

`ToolExecutionContext::cancellation` is propagated into builtin adapter
requests for HTTP transports, developer process execution, browser rendering,
web search, and web fetch. Injected adapters can observe the token and abort
host-specific work using the same cancellation object as the runner or tool
executor.

## ToolRun / BackgroundTask

Long-lived custom tools should not block `ToolDefinition::execute()` until the
work exits. Inject a `ToolRunManager` through `kToolServiceToolRunManager`,
declare the service requirement on the tool, start a generic `custom` run,
return the snapshot as the tool result, and let the host update/read/cancel it
through the manager.

```cpp
agent::InMemoryToolRunManager tool_runs;
agent::ToolExecutionContext context;
context.service_refs.service_container.set(agent::kToolServiceToolRunManager, &tool_runs);

auto tool = agent::define_tool(agent::ToolDefinition{
    .name = "custom.index.start",
    .service_requirements = {
        agent::tool_service_requirement(agent::kToolServiceToolRunManager),
    },
    .long_running = true,
    .execute = [](const agent::Value&, agent::ToolExecutionContext& ctx) {
      auto& runs = ctx.service_refs.service_view.require(agent::kToolServiceToolRunManager);
      auto run = runs.start(agent::ToolRunStartOptions{
          .tool_name = "custom.index.start",
          .kind = "custom",
          .label = "index workspace",
      });
      return agent::Value::object({{"toolRun", agent::tool_run_snapshot_to_value(run)}});
    },
});
```

The generic layer manages `queued` / `running` / `waiting` / `completed` /
`failed` / `cancelled` snapshots, cursor-based event/log reads, `list`,
`status`, `wait`, and cooperative `cancel`. It is intentionally not a terminal
API: a process runner, workflow runner, browser session, remote job, or any
business-specific background task can all publish the same ToolRun shape.
Synchronous tools and `shell.exec` continue to behave as before.

## Lazy tool discovery

When a runner integrates many tools — typically several MCP servers' worth —
prompting the model with the full schema for every tool wastes tokens and
distracts the model from the few tools it actually needs. Set
`ToolRegistry::lazy_mode = true` (or call `set_lazy_mode(true)`) to hide every
tool whose name does not start with `tool.` from `list()` / `descriptors()`.
Hidden tools remain fully executable when invoked by name.

The recommended bootstrap is:

```cpp
registry.set_lazy_mode(true);
// Optional: pin a few hot tools so the model still sees them up-front.
registry.force_visible("fs.readText");
```

The agent then uses the always-visible `tool.search` / `tool.describe` core
builtins to find and inspect tools on demand:

- `tool.search({ query, limit? })` — case-insensitive substring search across
  every tool's name and description (the entire registry, not the visible
  subset). Returns `{ tools: [{ name, description, capabilities, bundle }] }`.
- `tool.describe({ name })` — returns `{ name, description, inputSchema,
  capabilities, bundle, tags }`, or `{ found: false }` if missing.

Use `ToolRegistry::list_all()` to enumerate every tool regardless of
visibility — primarily useful from custom discovery tools.
