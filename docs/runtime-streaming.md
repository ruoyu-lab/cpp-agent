# Runtime Streaming API

This page documents the native C++ runtime streaming surface. It tracks the
NodeJS runtime event contract while keeping execution synchronous and
zero-dependency in C++.

## Entry Point

Include the public umbrella header:

```cpp
#include "agent/agent.hpp"
```

Use `AgentRunner::stream` when callers need intermediate runtime events:

```cpp
agent::AgentRunner runner(agent::AgentRunnerConfig{
    .adapter = model,
    .tools = tools,
    .max_iterations = 4,
});

auto stream = runner.stream("summarize with tools", "session-1");
for (const auto& event : stream.events) {
  if (event.type == agent::AgentRunnerStreamEventType::Loop) {
    // Inspect event.loop_event.
  }
}
```

`AgentRunner::stream` returns an `AgentRunnerStreamResult` containing:

- `events`: ordered runner events collected during the run.
- `result`: final `AgentRunnerRunResult`.

The API is synchronous by design. It does not require a Node event loop,
coroutines, or third-party async runtime.

## Runner Events

`AgentRunnerStreamEventType` includes:

- `Status`: normalized high-level status for UI and CLI surfaces.
- `KnowledgeRetrieval`: knowledge retrieval payload and debug summary.
- `MemoryRetrieval`: long-term memory retrieval payload.
- `Planning`: execution plan payload. Its external serialized event type is
  `plan`, matching NodeJS. The event is emitted only when a planner returns a
  structured plan; planning status events still report start/complete when the
  planner runs but returns no plan.
- `Loop`: raw `AgentLoopStreamEvent` parity surface.
- `ToolCallArgumentDelta`: surfaces partial tool-call JSON arguments as the
  model streams them, before any tool executes. The runner event carries
  `tool_call_id`, `tool_call_name`, `tool_call_args_delta`, and
  `tool_call_args_accumulated` directly (it does not nest a `loop_event`).
- `Done`: final result.
- `Error`: error snapshot emitted before rethrow when streaming fails.

Status events are derived from loop events and retrieval/planning phases.
Consumers that need exact NodeJS loop parity should inspect `Loop` events.

`KnowledgeRetrieval` and `MemoryRetrieval` are emitted even when the runner has
no configured knowledge base or long-term memory. In that no-op case the hit
arrays are empty and no retrieval status event is emitted. This matches the
NodeJS runner stream shape, where consumers can rely on those event types being
present without first checking the runner configuration.

## Loop Events

`AgentLoopStreamEventType` includes:

- `IterationStart`: a new loop iteration begins.
- `ModelStart`: model stream begins and provider/model are known.
- `ModelTextDelta`: model text delta.
- `ModelReasoningDelta`: model reasoning delta.
- `ModelResponse`: final model response for the iteration.
- `ToolCallArgumentDelta`: surfaces a partial tool-call JSON argument chunk
  produced by the model. Fires before `ToolStart` for the matching
  `tool_call_id`. See "Tool Call Argument Streaming" below.
- `ToolStart`: one tool call is about to execute.
- `ToolComplete`: one tool call finished, failed, or was converted into a synthetic failure.
- `Tools`: aggregate event emitted after all tool calls for the iteration have been added to memory.
- `Done`: loop result is complete.

`ToolComplete` carries `tool_result`. `Tools` carries `tool_results` and
matches the NodeJS `type: "tools"` aggregate event.

## Tool Call Argument Streaming

While the model streams the JSON arguments for a tool call, the framework
surfaces every partial chunk as it arrives. This is how UIs can show progress
for long-running argument generation (e.g. `fs.writeText` with thousands of
lines of `content`) instead of freezing until the full payload is ready.

Three layers carry the same payload shape:

- Model layer: `ModelStreamEventType::ToolCallDelta` with
  `provider`, `model`, `tool_call_id`, `tool_call_name`,
  `tool_call_args_delta`, `tool_call_args_accumulated`.
- Loop layer: `AgentLoopStreamEventType::ToolCallArgumentDelta` with
  `iteration`, `provider`, `model`, and the same four `tool_call_*` fields.
- Runner layer: `AgentRunnerStreamEventType::ToolCallArgumentDelta` with the
  same four `tool_call_*` fields.

The loop layer also publishes an EventBus event with category
`model.tool_call_delta` and camelCase payload keys (`provider`, `model`,
`toolCallId`, `toolName`, `argsDelta`, `argsAccumulated`, `iteration`). This
lets non-stream subscribers receive the same data.

### Per-tool-call lifecycle

Consumers track each `tool_call_id` through the same state machine:

```
ToolCallArgumentDelta(id, ...) ... ToolCallArgumentDelta(id, ...)  → preparing
ToolStart(id)                                                       → running
ToolComplete(id, ok=true)                                           → completed
ToolComplete(id, ok=false, denied)                                  → denied (or failed)
```

`ToolCallArgumentDelta` fires before the `before_tool` hook runs and before
the permission policy is consulted. A subsequent permission denial surfaces
as a failed `ToolComplete` (existing semantics) — the UI should reconcile by
moving the preparing entry into denied/failed when the matching
`ToolComplete` arrives.

### Delta granularity per provider

Different providers emit tool call arguments at different granularities. The
framework forwards each delta as-is — it never synthesizes character-level
chunks from a one-shot payload, nor batches character-level chunks into one.

| Provider                                  | Delta granularity         |
|-------------------------------------------|---------------------------|
| OpenAI / Qwen / MiMo / DeepSeek-via-OpenAI | Character-level          |
| Anthropic / DeepSeek-via-Anthropic         | Character-level          |
| Ollama / Gemini                            | One-shot per tool call   |
| llama-cpp-native                           | N/A (local, no stream)   |

For one-shot providers, `tool_call_args_delta == tool_call_args_accumulated`
on the single emitted event. UI consumers don't need to branch on this —
debounced rendering handles both granularities the same way.

### UI throttling guidance

OpenAI streams roughly 5-15 characters per argument delta. The framework
intentionally does not throttle — it faithfully forwards what the provider
sends — so consumers control the render rate. A typical UI debounce of
30-50ms before re-rendering keeps the experience smooth without missing
updates.

## Tool Failure Semantics

Tool execution follows the NodeJS layering:

- A tool implementation failure returns a failed `ToolExecutionResult` with a
  framework-level error such as `Tool "name" failed.`
- The raw cause remains in `ToolExecutionResult::output`.
- Permission denial returns `Tool "name" is not permitted.`, while the denial
  reason is preserved in permission and audit events.
- If the executor itself throws during streaming, the loop emits a failed
  `ToolComplete` synthetic result. Non-cancellation failures let the model
  continue and recover; cancellation failures are rethrown after the failed
  `ToolComplete` event is recorded.

## Cancellation

Pass an `agent::CancellationToken*` to `AgentRunner::stream` to propagate
cooperative cancellation through runner, loop, model, retrieval, and tool
execution. Cancellation errors preserve the original cancellation reason.

## Trace Propagation

Runner streaming creates an `agent.stream` root trace context. Model and tool
events derive child trace contexts from that run trace. Supplying a parent
`traceContext` in the context object makes the stream trace a child of the
caller-provided trace.

## Run Hooks

`AgentRunner::stream` dispatches the same run lifecycle hooks as
`AgentRunner::run`:

- `before_run` receives the original input value, caller context, session id,
  and generated run id before `run.started` is published.
- `after_run` receives the final run result before the stream returns the final
  `Done` event.
- `on_run_error` receives the original input value and error message before the
  stream stores an `Error` event snapshot and rethrows.
