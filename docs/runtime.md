# Runtime API

The runtime module implements `AgentLoop` and `AgentRunner`, the two engines
that drive an agent to completion. They are the C++ counterparts of the
NodeJS runtime package.

This page is the overview. Detailed surfaces have their own pages:

- [Runtime Inputs](runtime-inputs.md) — string, content-part, and full
  `AgentMessage` entry points.
- [Runtime Memory](runtime-memory.md) — session and long-term memory wiring.
- [Runtime Planning](runtime-planning.md) — static and model planners,
  plan reporting.
- [Runtime Streaming](runtime-streaming.md) — runner/loop stream events, tool
  failure semantics, cancellation, traces, hooks.

## AgentLoop vs AgentRunner

| Surface | Use for |
|---|---|
| `AgentLoop` | Single agent driving model + tools without external planning, retrieval, or runner-level hooks. The minimal reusable execution engine. |
| `AgentRunner` | The high-level surface most callers should use. Wraps `AgentLoop` with planner injection, knowledge/memory retrieval, runner lifecycle hooks, run lifecycle EventBus events, durable checkpoints, and stream status events. |

Both are synchronous and zero-dependency. Streaming returns ordered event
arrays, not async iterators.

## Minimal Run

```cpp
#include "agent/agent.hpp"

agent::AgentRunner runner(agent::AgentRunnerConfig{
    .adapter = my_model_adapter,
    .tools = my_tool_registry,
    .max_iterations = 6,
});

auto result = runner.run("Plan and execute the task.", "session-1");
std::cout << result.final_message.text << "\n";
```

`run` returns an `AgentRunnerRunResult` containing the final message, usage,
plan (if a planner is wired), knowledge/memory retrieval results, and tool
trace.

## Streaming

```cpp
auto stream = runner.stream("summarize and link", "session-1");
for (const auto& event : stream.events) {
  switch (event.type) {
    case agent::AgentRunnerStreamEventType::Status:
      // High-level status for UI.
      break;
    case agent::AgentRunnerStreamEventType::Loop:
      // Raw AgentLoop event for fidelity-sensitive consumers.
      break;
    default:
      break;
  }
}
auto final_result = stream.result;
```

The `events` array is the complete ordered stream produced during the run.
See [Runtime Streaming](runtime-streaming.md) for the event-type catalog and
tool-failure semantics.

## Lifecycle Sequence

A single `AgentRunner::run` (or `stream`) call performs, in order:

1. Resolve the input through any configured input entrypoint
   ([Runtime Inputs](runtime-inputs.md)).
2. Build the run trace context (or derive a child trace from a caller-supplied
   `traceContext`).
3. Dispatch `before_run` hooks and publish `run.started` on the event bus.
4. Optional knowledge retrieval. Emits `knowledge-retrieval` status; hits are
   exposed in the final result.
5. Optional long-term memory retrieval. Emits `memory-retrieval` status.
6. Optional planner invocation ([Runtime Planning](runtime-planning.md)).
   Emits `planning` status and (when a structured plan is returned) a `plan`
   event.
7. Repeated `AgentLoop` iterations until completion, max iterations, or
   cancellation:
   - `iteration-start` → `model-start` → text/reasoning deltas →
     `model-response` → optional tool calls (`tool-start`/`tool-complete` per
     call, `tools` aggregate after the batch).
   - Tool failures and permission denials are wrapped in failed
     `ToolExecutionResult` envelopes; the loop continues to give the model a
     chance to recover.
8. Optional long-term memory writeback. Records are merged according to the
   configured tri-state defaults and per-call overrides.
9. Dispatch `after_run` hooks and publish `run.completed`.
10. Return the final `AgentRunnerRunResult` (or final stream `Done` event).

On error, `on_run_error` hooks fire and `run.failed` is published before the
error is rethrown.

## Cancellation, Retry, Timeout

Pass a `CancellationToken*` to `run` or `stream` and it propagates through
runner → loop → model → retrieval → tool execution. See
[Execution](execution.md) for token semantics and active callback
registration.

Retry and timeout policies are applied per model call, per retrieval call, and
per tool call. Retry uses Node-style scheduling with optional fixed,
exponential, or jittered delays, and emits `retry.scheduled` EventBus events.

Streaming retries before the first emitted event. Once the stream has emitted
output, retries are skipped to avoid replaying partial output.

## Durable Checkpoints and Resume

`AgentLoop` and `AgentRunner` produce serializable checkpoint state at each
iteration boundary. Pass the checkpoint back through the runner config to
resume from that point — useful for crash recovery, host migration, and
autonomous step bridging.

## Hooks and Events

Two surfaces fire on every run:

- **Lifecycle hooks** (`HookSet`) — `before_run`, `after_run`, `on_run_error`,
  plus model/tool/permission/retrieval hook points.
- **EventBus events** — `run.started`, `run.completed`, `run.failed`,
  knowledge/memory retrieval started/completed/failed, retry scheduled,
  `tool.audit`, `permission.checked`, and more.

See [Hooks](hooks.md) and [Execution](execution.md).

## Tool Services

Runner configuration accepts merged typed and JSON tool execution services.
Runner-bound knowledge services (KnowledgeBase / KnowledgeBaseManager) are
exposed to tools through the merged service map so a tool implementation can
issue knowledge searches without re-injection. Run context is propagated into
embedded contexts and tool services automatically.
