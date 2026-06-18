# Runtime Streaming API

This page documents the native C++ runtime streaming surface. It tracks the
NodeJS runtime event contract while keeping execution synchronous and
zero-dependency in C++.

## Entry Point

Include the public umbrella header:

```cpp
#include "agent/agent.hpp"
```

Use `AgentRunner::stream` when callers need intermediate runtime events.
The callback overload forwards each runner event as it is produced:

```cpp
auto stream = runner.stream(
    "summarize with tools",
    [](const agent::AgentRunnerStreamEvent& event) {
      if (event.type == agent::AgentRunnerStreamEventType::UserVisibleDelta) {
        // Render event.delta immediately.
      }
    },
    "session-1");
```

The native C++ callback API is a realtime UI stream surface. It covers runner
status, user-visible deltas, raw loop fidelity, tool lifecycle, parse errors,
and terminal events without relying on EventBus side channels.

`AgentRunner::stream` returns an `AgentRunnerStreamResult` containing the final
`AgentRunnerRunResult` in `result`. It does not retain a framework-owned event
array. Callers that need a collected transcript should append events in the
callback:

```cpp
std::vector<agent::AgentRunnerStreamEvent> events;
auto stream = runner.stream(
    "summarize with tools",
    [&](const agent::AgentRunnerStreamEvent& event) {
      events.push_back(event);
    },
    "session-1");
```

Execution is synchronous by design. It does not require a Node event loop,
coroutines, or third-party async runtime, but the callback overload is invoked
inline during execution rather than replayed after completion.

For host languages that need backpressure or `for await` style consumption, use
`AgentRunner::stream_events` instead. It starts the runner on an internal worker
and returns an `AgentRunnerEventStream` backed by a bounded queue:

```cpp
agent::StreamQueueOptions options;
options.capacity = 16;

auto events = runner.stream_events("summarize with tools", options, "session-1");
agent::AgentRunnerStreamEvent event;
while (events.next(event)) {
  // Serialize, render, or await the next event in the host binding.
}
```

Queue capacity is the backpressure contract. When the queue is full, the runner
worker blocks until the host pulls the next event. Calling `close()` unblocks the
worker and ends subsequent `next()` calls. Language bindings should expose this
as a native async iterator where each await maps to one pull from the queue:

```text
open(input, sessionId, capacity) -> stream handle
next(stream) -> { done: false, value: AgentRunnerStreamEventJson }
next(stream) -> { done: true }
cancel(stream, reason)
close(stream)
release(stream)
```

`close()` closes the queue and cooperatively cancels the internally owned
runner cancellation token when the caller did not provide one. The C ABI
implements the same iterator contract with
`agent_runner_stream_events`, `agent_runner_stream_events_json`,
`agent_runner_event_stream_next_json`, `agent_runner_event_stream_cancel`,
`agent_runner_event_stream_close`, and `agent_runner_event_stream_release`.
Every serialized event includes
`schemaVersion`, `sequence`, and `type`; the formal event schema lives in
`contracts/observable/stream-events.schema.json`.

`EventBus` remains an observability and audit channel (`model.delta`,
`tool.started`, `tool.completed`, `tool.failed`, etc.). Hosts should prefer the
callback or pull stream APIs for realtime UI rendering and use EventBus for
independent subscribers, logs, metrics, and replay.

## Runner Events

`AgentRunnerStreamEventType` includes:

- `Status`: normalized high-level status for UI and CLI surfaces.
- `KnowledgeRetrieval`: knowledge retrieval payload and debug summary.
- `MemoryRetrieval`: long-term memory retrieval payload.
- `Planning`: execution plan payload. Its external serialized event type is
  `plan`, matching NodeJS. The event is emitted only when a planner returns a
  structured plan; planning status events still report start/complete when the
  planner runs but returns no plan.
- `UserVisibleDelta`: the only runner event type intended for direct UI/CLI
  rendering. It carries `delta` and accumulated `text`.
- `Loop`: raw loop-event parity surface.
- `ToolCallArgumentDelta`: surfaces partial tool-call JSON arguments as the
  model streams them, before any tool executes. The runner event carries
  `tool_call_iteration`, `tool_call_provider`, `tool_call_model`,
  `tool_call_id`, `tool_call_name`, `tool_call_args_delta`, and
  `tool_call_args_accumulated` directly (it does not nest a `loop_event`).
- `Done`: final result.
- `Cancelled`: cancellation snapshot emitted before rethrow when cooperative
  cancellation stops the stream.
- `Error`: error snapshot emitted before rethrow when streaming fails.

Status events are derived from loop events and retrieval/planning phases.
Consumers that need exact NodeJS parity should inspect `Loop` events.

`KnowledgeRetrieval` and `MemoryRetrieval` are emitted even when the runner has
no configured knowledge base or long-term memory. In that no-op case the hit
arrays are empty and no retrieval status event is emitted. This matches the
NodeJS runner stream shape, where consumers can rely on those event types being
present without first checking the runner configuration.

The C ABI serializer forwards these richer runner event payloads directly:
status details, retrieval payloads, planning payloads, loop payloads, tool-call
argument deltas, and the terminal `done` result all stay distinguishable at the
foreign-function boundary.

## Loop Events

`AgentLoopStreamEventType` includes:

- `IterationStart`: a new loop iteration begins.
- `ModelStart`: model stream begins and provider/model are known.
- `ModelTextDelta`: raw model text delta for trace/observability. It can
  contain protocol text such as `Thought`, `Actions`, or `Final Answer`; do not
  render it directly in business UI.
- `UserVisibleDelta`: framework-approved user-visible text. ReAct final
  answers are emitted here only after the final-answer validator accepts them.
- `ModelReasoningDelta`: model reasoning delta.
- `ModelReasoningCompleted`: turn-scoped model reasoning is complete.
- `AgentOutput`: final model response for the iteration.
- `ToolCallArgumentDelta`: surfaces a partial tool-call JSON argument chunk
  produced by the model. Fires before `ToolStart` for the matching
  `tool_call_id`. See "Tool Call Argument Streaming" below.
- `ReActActionBatch`: ReAct parser produced a validated batch of actions.
- `ToolBatchStart`: one validated tool batch is about to execute.
- `ToolStart`: one tool call is about to execute.
- `ToolDelta`: realtime tool output/progress delta.
- `ToolComplete`: one tool call finished, failed, or was converted into a synthetic failure.
- `ToolBatchComplete`: aggregate event emitted after all tool calls for the batch finish.
- `ReActFinalRejected`: final answer failed the completion gate and was converted into an Observation.
- `ReActReasoningProtocolLeak`: ReAct protocol appeared in model reasoning instead of assistant content and was converted into an Observation.
- `Done`: loop result is complete.

`ToolBatchStart` carries `tool_calls`. `ToolComplete` carries `tool_result`.
`ToolBatchComplete` carries both `tool_calls` and `tool_results`.

## ReAct Visible Deltas

Managed ReAct prompts are strict: the parser expects either an `Actions` JSON
array or a `Final Answer` block, the response must start with `Thought:`, and
malformed output is reported as a parse error. `ModelTextDelta` is raw model
text. `UserVisibleDelta` is the rendering contract for UI and CLI consumers.

For Text ReAct, intermediate `Message:`, `Assistant:`, and `Progress:` blocks
can stream as `UserVisibleDelta`. `Final Answer:` content is withheld until the
final-answer validator accepts it, so rejected finals are never emitted as
user-visible deltas. Because intermediate visible messages can exist, hosts
should treat `result.text` as the final answer and `UserVisibleDelta` as the
render stream, not assume one is always the concatenation of the other.

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
  same `iteration`, `provider`, `model`, and `tool_call_*` fields, exposed as
  direct runner-event fields so async workers and language bindings do not need
  to inspect a nested loop event.

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
- `ToolExecutionResult::structuredOutput` preserves the machine value when the
  tool returned one; `output` remains the model-facing text representation.
- The raw failure cause remains in `ToolExecutionResult::output`.
- Permission denial returns `Tool "name" is not permitted.`, while the denial
  reason is preserved in permission and audit events.
- If the executor itself throws during streaming, the loop emits a failed
  `ToolComplete` synthetic result. Non-cancellation failures let the model
  continue and recover; cancellation failures are rethrown after the failed
  `ToolComplete` event is recorded.

## Cancellation

Pass an `agent::CancellationToken*` to `AgentRunner::stream` to propagate
cooperative cancellation through runner, loop, model, retrieval, and tool
execution. Cancellation raises `CancellationError`, emits `run.cancelled`, and
uses `AgentRunnerStreamEventType::Cancelled` instead of the normal error path.
Cancellation errors preserve the original cancellation reason.

Non-cancellation framework errors use `AgentFrameworkError::error_name()`,
`error_category()`, and `error_code()` for stream error payloads. Generic
`std::exception` failures still preserve `what()` and are reported as
`Error` / `unknown` / `unknown-error`.

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
- `on_run_error` receives the original input value and error message before a
  non-cancellation failure stores an `Error` event snapshot and rethrows.
