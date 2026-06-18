# Workflow API Notes

The native workflow module provides builder helpers, workflow state
serialization, in-memory/file-backed stores, node and agent registries, built-in
node handlers, persisted resume, human/webhook signals, and workflow inspection
tools.

Workflow stores hold `runtime-owned` run state: definition snapshots, node
state, wait signals, checkpoints, and event logs. They are suitable as the
canonical store for small local deployments, but they are not a framework-owned
business repository for orders, tickets, approvals, or other domain entities.

## File Store

`FileWorkflowStore` supports NodeJS-style config-object construction:

```cpp
agent::FileWorkflowStore store(agent::FileWorkflowStoreConfig{
    .base_dir = "workflow/runs",
});
```

Workflow run ids are persisted as `encodeURIComponent`-style filenames under
the configured base directory. For example, `manual:run/with space` is stored as
`manual%3Arun%2Fwith%20space.json`. Existing native files written with the older
sanitized filename format are still readable.

The older direct path constructor remains available:

```cpp
agent::FileWorkflowStore store("workflow/runs");
```

## Engine

```cpp
agent::ToolRegistry tools;
agent::ToolExecutor executor(tools);
agent::FileWorkflowStore store(agent::FileWorkflowStoreConfig{
    .base_dir = "workflow/runs",
});
agent::EventBus events;
agent::ExecutionPolicies policies;
agent::WorkflowEngine engine(agent::WorkflowEngineConfig{
    .tools = &tools,
    .tool_executor = &executor,
    .store = &store,
    .event_bus = &events,
    .execution_policies = policies,
});

auto definition = agent::create_workflow("review")
    .start()
    .human_wait("approve", agent::WorkflowHumanWaitOptions{
                               .prompt = "Approve?",
                           })
    .end("end", agent::WorkflowEndOptions{
                    .result = agent::workflow_ref("nodes.approve.output.value"),
                })
    .sequence({"start", "approve", "end"})
    .build();

auto waiting = engine.run(agent::WorkflowRunInput{
    .definition = definition,
    .input = agent::Value::object({{"ticket", "T-42"}}),
});
auto completed = engine.submit_human_response(agent::WorkflowHumanResponseInput{
    .workflow_run_id = waiting.workflow_run_id,
    .node_id = "approve",
    .payload = "approved",
});
```

Use `resume`, `submit_human_response`, and `submit_webhook_payload` with a
store-backed engine when a workflow waits for an external signal. These methods
support NodeJS-style object inputs through `WorkflowRunInput`,
`WorkflowResumeInput`, `WorkflowHumanResponseInput`, and
`WorkflowWebhookPayloadInput`; the older positional overloads remain available
for existing C++ callers.

`WorkflowEngineConfig` mirrors the NodeJS constructor shape for the native
runtime: tools, tool executor, tool services, agent registry, store, node
registry, hooks, event bus, and execution policies can be supplied as a single
configuration object. The older positional constructor remains available for
existing C++ callers.

When an event bus is configured, the engine publishes workflow lifecycle, node
lifecycle, edge activation, human/webhook signal, child-agent, and retry
scheduled events using the same category names as the NodeJS workflow runtime.

Builder methods also support NodeJS-style options objects for transform, tool,
agent, condition, human-wait, artifact, router, join, webhook-wait, and end
nodes. `create_workflow` and `create_node` mirror the NodeJS builder helpers.
