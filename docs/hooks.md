# Hooks API

Hooks provide synchronous lifecycle callbacks around runner, model, tool,
retrieval, permission, workflow, workflow-node, and child-agent work. They are
plain callbacks so applications can attach logging, policy, or observability
without a plugin runtime.

## Hook Contexts

Every hook context derives from `HookExecutionContext`:

```cpp
struct HookExecutionContext {
  std::string trace_id;
  std::string run_id;
  std::string workflow_run_id;
  agent::ExecutionTarget target;
  agent::Value metadata;
};
```

Specialized contexts add stage-specific data:

- `RunHookContext`: input, context, result, error.
- `ModelHookContext`: request, response, error.
- `ToolHookContext`: tool name, tool call, input, result, error.
- `RetrievalHookContext`: query, source, result, error.
- `PermissionHookContext`: tool name, decision, reason.
- `WorkflowHookContext`: workflow id, input, result, error.
- `WorkflowNodeHookContext`: workflow id, node id, node type, input, result,
  error.
- `ChildAgentHookContext`: workflow id, agent id, input, result, error.

## Hook Set

Attach callbacks through `HookSet`:

```cpp
agent::HookSet hooks;
hooks.before_tool = [](const agent::ToolHookContext& context) {
  // Inspect context.tool_name and context.trace_id.
};
hooks.on_model_error = [](const agent::ModelHookContext& context) {
  // Inspect context.error.
};
```

Available callbacks:

- `before_run`, `after_run`, `on_run_error`
- `before_model`, `after_model`, `on_model_error`
- `before_tool`, `after_tool`, `on_tool_error`
- `before_knowledge_retrieval`, `after_knowledge_retrieval`,
  `on_knowledge_retrieval_error`
- `before_permission_check`, `after_permission_check`
- `before_workflow`, `after_workflow`, `on_workflow_error`
- `before_workflow_node`, `after_workflow_node`, `on_workflow_node_error`
- `before_child_agent`, `after_child_agent`, `on_child_agent_error`
- `before_fs_write` — fired by `fs.writeText` (and other opt-in fs-write tools)
  immediately before the on-disk write. Receives the resolved `path` and
  proposed `content`. Not default-on; intended for snapshot / audit
  integrations such as registering `git.snapshot` as a pre-write hook.

Runtime, tools, and workflow components populate whichever fields are available
for that lifecycle stage.

## Merging

Use `merge_hooks` to combine multiple hook sets:

```cpp
auto merged = agent::merge_hooks({global_hooks, agent_hooks, runtime_hooks});
```

For each callback, the merged hook invokes all non-empty callbacks in the input
order. This preserves layered configuration: global hooks can run before
agent-specific hooks, and runtime hooks can observe the final stage last.

`merge_hooks({})` returns an empty hook set.

## Failure Behavior

Hooks are synchronous. A hook exception propagates to the caller at the stage
where the hook runs. Keep production hooks lightweight and isolate failures in
the hook body when the hook should not affect agent execution.

## Default Logging Convention: success silent, failures verbose

Call `default_logging_hook_set(sink)` to get a `HookSet` pre-populated with the
convention: successful hook outcomes emit at `HookLogSeverity::Trace` and
failures or blocked decisions emit at `HookLogSeverity::Warn` with full
context. The sink callback is invoked for every entry; filter on severity to
get the standard quiet-on-success behaviour:

```cpp
auto sink = [](const agent::HookLogEntry& entry) {
  if (entry.severity < agent::HookLogSeverity::Info) return;  // drop trace
  std::cerr << entry.source << ": " << entry.message << "\n";
};
auto hooks = agent::default_logging_hook_set(sink);
```

If you do not pass a sink, the default hook set still wires up callbacks but
silently drops everything. This is the recommended baseline: it costs nothing
when nothing is wrong, and any failure already comes through as a warn entry.

## Out-of-process hooks

`make_process_tool_hook(ProcessHookConfig)` returns a
`std::function<void(const ToolHookContext&)>` that spawns an external
executable per invocation, pipes the hook payload as JSON to stdin, and reads
stdout. Exit code `0` allows the tool call; exit code `2` blocks it (and the
process's stderr becomes the block reason); any other non-zero exit emits a
warning event but allows the call to proceed. Unix-only.

```cpp
agent::ProcessHookConfig config{
    .executable = "/usr/local/bin/policy-check",
    .timeout = std::chrono::seconds(2),
};
agent::HookSet hooks;
hooks.before_tool = agent::make_process_tool_hook(config);
```

## Zero-Dependency Boundary

Hooks are plain C++ function objects and `Value` payloads. They do not require an
event loop, tracing SDK, metrics SDK, plugin loader, or JavaScript runtime.
