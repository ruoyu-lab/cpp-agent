# Runtime API

The runtime module exposes `AgentRunner` as the main supported agent execution
entry point. `AgentRunner` runs the ReAct loop by default; the legacy
function-calling loop is no longer a top-level public entry point and is kept
under `agent::internal::AgentLoop` only for low-level tests.

This page is the overview. Detailed surfaces have their own pages:

- [Runtime Inputs](runtime-inputs.md) — string, content-part, and full
  `AgentMessage` entry points.
- [Runtime Memory](runtime-memory.md) — session and long-term memory wiring.
- [Runtime Planning](runtime-planning.md) — static and model planners,
  plan reporting.
- [Runtime Streaming](runtime-streaming.md) — runner stream events, tool
  failure semantics, cancellation, traces, hooks.

## Runner Surfaces

| Surface | Use for |
|---|---|
| `AgentRunner` | Main high-level surface. Runs ReAct with planner injection, knowledge/memory retrieval, runner lifecycle hooks, run lifecycle EventBus events, durable checkpoints, and stream status events. |

The runner surface is synchronous and zero-dependency. Streaming uses an inline
callback for realtime events, not async iterators or replayed event arrays.

## Plan-And-Execute

`PlanAndExecuteRunner` is a separate long-task orchestrator, not a second
chat-style runner. It lives in `agent/plan.hpp` and composes:

1. A `Planner` that turns the user request into an `ExecutionPlan`.
2. Validation that every plan step has a stable id and sane dependencies.
3. Conversion from `ExecutionPlan` into an `AutonomousPlan`.
4. An `AutonomousRunManager` that owns run state, checkpoints, and dependency
   ordering.
5. An `AgentRunnerStepExecutor` that executes each plan step by calling the
   supplied `AgentRunner`.

The execution shape is therefore:

`input -> planner -> plan steps -> autonomous run -> AgentRunner per step -> final snapshot/result`

Use it when one request should be decomposed into durable, resumable steps.
Use `AgentRunner` directly when the task is still a normal interactive
conversation or a single ReAct loop.

## Minimal Run

```cpp
#include "agent/agent.hpp"

auto runner = agent::AgentRuntimeBuilder()
                  .model(my_model_adapter)
                  .tools(my_tools)
                  .max_iterations(6)
                  .build();

auto result = runner.run("Plan and execute the task.", "session-1");
std::cout << result.text << "\n";
```

`run` returns an `AgentRunnerRunResult` containing the final message, usage,
plan (if a planner is wired), knowledge/memory retrieval results, and tool
trace.

## Tool Calling Strategy

`AgentRunnerConfig::tool_runtime.calling_strategy` controls how the runner lets the
model call tools:

- `AgentToolCallingStrategy::TextReAct`: the runner uses the Text ReAct
  protocol (`Thought`, `Actions`, `Final Answer`) and parses tool calls from
  assistant text. This is the default because it works with plain text chat
  adapters.
- `AgentToolCallingStrategy::NativeToolCalling`: the runner passes native
  `ChatToolDescriptor` values to the model adapter and executes
  `AgentOutput::tool_calls`. Use this for providers/adapters with first-class
  tool calling support.

These strategies have different contracts. `react_prompt_mode`,
`react_parser_options`, and final-answer validation apply to `TextReAct`.
Native tool calling relies on the provider adapter to return structured
`tool_calls`.

`ChatToolDescriptor` also carries the tool governance metadata declared on
`ToolDefinition`: `read_only`, `mutates_files`, `interactive`, `long_running`,
`batchable`, `concurrency_key`, and `side_effect_level`. These fields are part
of the cross-language Tool Governance Contract with the NodeJS runtime. Defaults
are conservative (`false`, empty key, and `"unknown"`), so hosts should mark
read-only or explicitly batchable tools when parallel execution is safe and mark
file mutations with `mutates_files` / a write-oriented `side_effect_level`.

## Prompt Strategy

`AgentRunner` defaults to `ReActPromptMode::Managed`. In this mode the framework
injects a low-leakage internal tool-use format prompt so tools work without
host setup. The managed prompt intentionally does not describe the assistant as
"ReAct-based" and tells the model not to mention internal protocol details.

Hosts that already own prompt composition can choose a stricter boundary:

```cpp
agent::AgentRunnerConfig config;
config.context_runtime.system_prompt =
    "Your full business prompt, including Actions JSON array / Final Answer rules.";
config.react_runtime.prompt_mode = agent::ReActPromptMode::External;
```

- `Managed`: framework injects the built-in tool-use prompt.
- `Custom`: framework injects `react_prompt_builder`; the host owns the wording.
- `External`: framework injects no tool-use prompt; the host must make the model
  produce parser-compatible `Actions` JSON array or `Final Answer` output.

The managed ReAct prompt now also supports an optional visible intermediate
block:

```text
Thought:
Message:
Actions:
[{"tool":"tool.name","input":{}}]
```

`Message:` is user-visible but non-final. `Final Answer:` remains the final
user-visible answer. `Thought:` stays private, and `Actions` remains the
internal protocol block.

If a host wants those intermediate visible messages to become part of the
session transcript, set
`AgentRunnerConfig::react_runtime.persist_visible_messages = true`. The default
is `false`, which keeps intermediate visible messages out of `SessionMemory`
while still exposing them through stream events and the `react.message` EventBus
event.

## Streaming

```cpp
auto stream = runner.stream(
    "summarize and link",
    [](const agent::AgentRunnerStreamEvent& event) {
      switch (event.type) {
        case agent::AgentRunnerStreamEventType::Status:
          // High-level status for UI.
          break;
        case agent::AgentRunnerStreamEventType::Loop:
          // Raw loop event for fidelity-sensitive consumers.
          break;
        default:
          break;
      }
    },
    "session-1");
auto final_result = stream.result;
```

The callback receives the ordered stream as it is produced. If a host needs a
transcript, it should collect events inside that callback.
See [Runtime Streaming](runtime-streaming.md) for the event-type catalog and
tool-failure semantics.

## Context Stats

`AgentRunner` now exposes two context-inspection surfaces:

- `estimate_context_stats(...)`
- `last_context_stats()`

`estimate_context_stats(...)` is a cheap, static estimate for the next run. It
reuses the runner's prompt assembly rules, current `SessionMemory`, skill
activation rules, context-manager message assembly, and tool visibility, but it
does not execute planners, retrieval, MCP reads, or subagent forks. This makes
it safe for UI previews and quota displays across different host applications.

`last_context_stats()` returns the most recent real pre-model snapshot captured
during an actual `run(...)` or `stream(...)` call. The snapshot is emitted just
after prompt assembly and just before `model.started` as a `context.stats`
EventBus event, so hosts can observe the exact prompt footprint that preceded
the real model call.

Every snapshot reports:

- `totalTokens`
- per-bucket token counts
- `estimator`
- `accurate`

Prompt-derived buckets are assembled from the same runtime path that builds the
model request:

- Managed Text ReAct splits the framework-owned system prompt, ReAct rules,
  regular tool definitions, and MCP tool definitions.
- External or custom prompt modes can provide `prompt_stats_buckets` in
  `AgentRunnerConfig` so a host-owned combined prompt can still be counted as
  separate system/rules/tool-definition buckets.
- Native tool calling does not count ReAct rules. It counts the system prompt
  or configured prompt stats buckets, plus the structured tool descriptors sent
  to the model, split into regular tools and MCP tools.
- Embedded runtime context is counted from
  `EmbeddedContextManager::build_assembly()`, so each
  `EmbeddedContextBlock` becomes its own bucket instead of all blocks being
  collapsed into the final `embedded-context` system message.

The default estimator is the shared `chars / 4` fallback (`estimator =
"char_div_4"`, `accurate = false`). If a host installs a custom
`SessionMemoryTokenCounter`, the live runner path reuses it through the context
stats counter bridge instead of inventing a separate token-estimation path.

## Lifecycle Sequence

A single `AgentRunner::run` (or `stream`) call performs, in order:

1. Resolve the input through any configured input entrypoint
   ([Runtime Inputs](runtime-inputs.md)).
2. Build the run trace context (or derive a child trace from a caller-supplied
   `traceContext`).
3. Resolve configured/default/user skills. This dispatches
   `before_skills_resolve`, per-skill activation hooks, and
   `after_skills_resolve`; failures dispatch the matching `on_*_error` hooks.
   When an event bus is present, `skills.resolve.*` and `skill.activation.*`
   events are published with target `skill`.
4. Dispatch `before_run` hooks and publish `run.started` on the event bus.
5. Optional knowledge retrieval. Emits `knowledge-retrieval` status; hits are
   exposed in the final result.
6. Optional long-term memory retrieval. Emits `memory-retrieval` status.
7. Optional planner invocation ([Runtime Planning](runtime-planning.md)).
   Emits `planning` status and (when a structured plan is returned) a `plan`
   event.
8. Repeated ReAct iterations until completion, max iterations, or
   cancellation:
   - `model-start` → visible Final Answer text/reasoning deltas →
     `model-response` → optional ReAct action and tool observation.
   - Tool failures and permission denials are wrapped in failed
     `ToolExecutionResult` envelopes and returned as observations; the loop
     continues to give the model a chance to recover.
9. Optional long-term memory writeback. Records are merged according to the
   configured tri-state defaults and per-call overrides.
10. Dispatch `after_run` hooks and publish `run.completed`.
11. Return the final `AgentRunnerRunResult` (or final stream `Done` event).

On non-cancellation errors, `on_run_error` hooks fire and `run.failed` is
published before the error is rethrown. Cooperative cancellation raises
`CancellationError` and publishes `run.cancelled` instead.

## Execution Pipeline

`AgentRunner::run` and `AgentRunner::stream` now share the same internal
pipeline stages:

1. Resolve input, slash-skill activation, effective input text, and effective
   model settings.
2. Prepare runner preface state: skill preface, knowledge retrieval, memory
   retrieval, and optional planning.
3. Build the merged tool/runtime service context and enter the ReAct loop.
4. Adapt loop output into either a final run result or the runner stream event
   sequence.

This keeps synchronous run and stream behavior aligned instead of maintaining
parallel implementations for retrieval, planning, and tool execution setup.

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

`AgentRunner` produces serializable checkpoint state at each ReAct phase. Pass
the checkpoint back through the runner config to resume from that point —
useful for crash recovery, host migration, and autonomous step bridging.

Runner durable checkpoints carry the resolved effective input and effective
model settings. Resume therefore preserves the original execution shape even if
the host's default model settings or skill resolution defaults change between
attempts.

## Hooks and Events

Two surfaces fire on every run:

- **Lifecycle hooks** (`HookSet`) — `before_run`, `after_run`, `on_run_error`,
  skill resolve/activation hooks, plus model/tool/permission/retrieval hook
  points.
- **EventBus events** — `run.started`, `run.completed`, `run.cancelled`, `run.failed`,
  knowledge/memory retrieval started/completed/failed, retry scheduled,
  `tool.audit`, `permission.checked`, and more.

See [Hooks](hooks.md) and [Execution](execution.md).

## Tool Services

Runner configuration accepts merged typed and JSON tool execution services.
Runner-bound knowledge services (KnowledgeBase / KnowledgeBaseManager) are
exposed to tools through the merged service map so a tool implementation can
issue knowledge searches without re-injection. Run context is propagated into
embedded contexts and tool services automatically.
