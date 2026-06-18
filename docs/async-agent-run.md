# Async Agent Run Contract

`AsyncAgentRun` is the language-neutral execution ledger for durable agent
runs. It is not a Lumi-specific API, not a database adapter, and not a UI
model. NodeJS, C++, C, C#, Rust, Swift, Python, and other embeddings can share
the same JSON protocol while implementing their own stores, workers, schedulers,
and presentation layers.

The executable observable contract lives at
[`../contracts/observable/async-agent-run.json`](../contracts/observable/async-agent-run.json).

## Run Kinds

`kind` identifies the entrypoint:

- `agent.run` — a normal background AgentRunner execution.
- `subagent.run` — an asynchronous subagent. Existing synchronous
  `skill_subagents` remain synchronous fork/join calls.
- `workflow.run` — a workflow-submitted run.
- `autonomous.run` — a long-running autonomous manager run.

Async subagents do not create a separate runtime stack. They reuse the same
state machine, event ledger, checkpoint stream, transcript stream, cancellation,
resume, and scheduler boundary as every other async agent run.

## State Machine

Statuses are fixed strings at the JSON boundary:

| status | Meaning |
| --- | --- |
| `queued` | The run is durable and waiting for a worker claim. |
| `running` | A worker owns the lease and is executing. |
| `waiting` | Execution is blocked on approval, external input, host scheduling, or another external condition. The specific reason is stored in `waiting.reason`. |
| `completed` | Successful closed state. |
| `failed` | Failed state that may be explicitly resumed or retried. |
| `cancelled` | Explicitly cancelled closed state. |

The allowed transition table is part of the observable contract. `completed`
and `cancelled` are closed states and must not be implicitly resumed. `waiting`
and `failed` can only become `queued` through an explicit resume path, an
approval resolution, or a scheduler wakeup. Worker or lease interruptions are
attempt-level facts, surfaced through `async_run.interrupted` or
`async_run.lease.expired`, not a top-level run status.

## Snapshot Shape

The run snapshot is plain JSON:

```json
{
  "schemaVersion": 1,
  "runId": "run_async_001",
  "parentRunId": "run_parent_001",
  "rootRunId": "run_parent_001",
  "kind": "subagent.run",
  "depth": 1,
  "role": "leaf",
  "parentChildRelation": "child",
  "spawnedByToolCallId": "tool_call_001",
  "agent": {
    "id": "researcher",
    "displayName": "Research SubAgent"
  },
  "status": "queued",
  "activity": {
    "phase": "queued",
    "currentTarget": "run",
    "currentToolName": "",
    "currentIteration": -1,
    "interruptible": true,
    "stallReason": "",
    "lastHeartbeatAt": "",
    "lastActivityAt": "2026-06-13T00:00:00Z",
    "lastHeartbeatAtMs": 0,
    "lastActivityAtMs": 1781360000000
  },
  "resourceLedger": {
    "tokenUsage": {
      "inputTokens": 0,
      "outputTokens": 0,
      "totalTokens": 0,
      "reasoningTokens": 0,
      "cachedInputTokens": 0,
      "reasoningSource": "unknown",
      "provider": ""
    },
    "toolCalls": 0,
    "filesRead": [],
    "filesWritten": [],
    "artifacts": [],
    "networkRequests": [],
    "approvalRequests": [],
    "childRuns": [],
    "iterationCount": 0,
    "toolSuccessCount": 0,
    "toolErrorCount": 0,
    "childRunCount": 0,
    "details": {}
  },
  "result": {
    "schemaVersion": 1,
    "runId": "run_async_001",
    "kind": "subagent.run",
    "status": "queued",
    "rootRunId": "run_parent_001",
    "parentRunId": "run_parent_001",
    "depth": 1,
    "role": "leaf",
    "parentChildRelation": "child",
    "spawnedByToolCallId": "tool_call_001",
    "text": "",
    "output": null,
    "error": "",
    "resourceLedger": {
      "tokenUsage": {},
      "toolCalls": 0,
      "childRunCount": 0,
      "details": {}
    },
    "metadata": {}
  },
  "input": {
    "messages": [
      {
        "role": "user",
        "content": "research the topic"
      }
    ],
    "sessionId": "session-parent"
  },
  "attempt": 0,
  "checkpoint": {
    "latestCheckpointId": "",
    "sequence": 0
  },
  "transcript": {
    "latestEntryId": "",
    "sequence": 0
  },
  "createdAt": "2026-06-13T00:00:00Z",
  "updatedAt": "2026-06-13T00:00:00Z",
  "startedAt": "",
  "completedAt": "",
  "cancelledAt": "",
  "metadata": {}
}
```

Boundary rules:

- IDs are opaque UTF-8 strings.
- Timestamps are RFC3339 UTC strings. Empty strings or absent fields represent
  unset optional timestamps.
- `attempt` starts at `0`; worker claims increment execution attempts.
- `activity.phase` is the fine-grained runtime phase. Top-level `status`
  remains one of `queued`, `running`, `waiting`, `completed`, `failed`, or
  `cancelled`.
- `result` uses the standard ChildAgentResult shape for every async run so
  roots and children can be consumed through the same ABI.
- `metadata` carries correlation and tenancy hints, but authorization remains
  host-owned.

## Child Agent Policy

Child starts are checked before they are persisted. The framework-level policy
is language-neutral:

```json
{
  "childAgentPolicy": {
    "maxGlobalChildRuns": 32,
    "maxChildRunsPerParent": 8,
    "maxSpawnDepth": 3,
    "maxChildrenPerRun": 4,
    "allowChildSpawn": true
  }
}
```

Violations fail the start call with an explicit `ChildAgentPolicy violation`
message. A child run requires `parentRunId`; if `rootRunId` or `depth` are not
provided, the runtime derives them from the parent snapshot.

`maxGlobalChildRuns` limits active child runs across the current runtime store;
terminal child runs do not consume that capacity. `maxChildRunsPerParent`
remains a cumulative per-parent limit, while `maxChildrenPerRun` limits active
direct children for one parent run.

## Events

Events are append-only and ordered per run:

```json
{
  "schemaVersion": 1,
  "eventId": "evt_003",
  "runId": "run_async_approval",
  "sequence": 7,
  "type": "async_run.approval.requested",
  "status": "waiting",
  "payload": {
    "approvalId": "approval_001",
    "toolName": "shell.exec"
  },
  "createdAt": "2026-06-13T00:04:30Z"
}
```

The core event vocabulary includes `async_run.created`, `async_run.queued`,
`async_run.started`, `async_run.activity`, checkpoint and transcript
append events, approval request/resolution, waiting, resume, cancel request,
cancellation, completion, failure, interruption, and lease expiry. Child
lifecycle events use `child.run.*` names, for example `child.run.queued`,
`child.run.started`, `child.run.completed`, `child.run.failed`, and
`child.run.cancelled`. Resource aggregation updates are emitted as
`child.run.ledger.updated` for child runs. Event sequences are monotonic within
a run; the framework does not require global sequence numbers across runs.

## Worker Stream Observation

`AsyncAgentRunWorkerConfig::on_stream_event` is the generic host integration
point for realtime async-run projection. The callback receives the immutable
worker context, the raw `AgentRunnerStreamEvent`, and the framework-normalized
JSON `payload` that is also written to transcript `stream-event` entries.
The worker also records a durable `async_run.stream_event` event whose payload
wraps the same stream payload with run topology and attempt metadata.

Hosts can attach this observer to drive a CLI stream, web socket, native UI,
remote protocol bridge, or language binding without replacing the framework
worker loop. The observer is deliberately not tied to a storage engine or UI
shape. Persistence adapters remain responsible for durable storage, and product
layers decide how to render or throttle events.

`async_run.stream_event.payload.streamEvent` preserves the complete normalized
runner event. Frequently indexed fields are also flattened on the event payload:
`eventType`, `iteration`, `provider`, `model`, `toolCallId`, `toolName`,
`argsDelta`, and `argsAccumulated`.

`ToolCallArgumentDelta` payloads include:

```json
{
  "type": "tool-call-argument-delta",
  "iteration": 1,
  "provider": "openai",
  "model": "gpt-4.1",
  "toolCallId": "call_001",
  "toolName": "fs.writeText",
  "argsDelta": "{\"path\"",
  "argsAccumulated": "{\"path\""
}
```

Observer exceptions propagate through `run_once()` and fail the current async
run, matching the framework hook model. Observability-only consumers should
catch and isolate their own transport errors inside the observer.

## Checkpoints and Transcript

Checkpoints are durable execution snapshots for recovery. Transcript entries
are append-only audit and replay records. A run snapshot only stores the latest
pointer:

```json
{
  "checkpoint": {
    "latestCheckpointId": "ckpt_001",
    "sequence": 3
  },
  "transcript": {
    "latestEntryId": "entry_006",
    "sequence": 6
  }
}
```

Embeddings expose full lists through JSON operations such as
`async_agent_run.list_checkpoints` and `async_agent_run.list_transcript`.

## Cancel and Resume

The control plane is operation-oriented JSON:

- `async_agent_run.start`
- `async_agent_run.read`
- `async_agent_run.cancel`
- `async_agent_run.resume`
- `async_agent_run.list_events`
- `async_agent_run.list_checkpoints`
- `async_agent_run.list_transcript`
- `child_agent.start`
- `child_agent.read`
- `child_agent.cancel`
- `child_agent.resume`

Cancellation is cooperative: the store records the cancellation request and an
active worker receives cancellation through the runtime cancellation token.
Store state remains authoritative when no worker is active. Resume can requeue
`waiting` and `failed` runs; it must not reopen `completed` or `cancelled`
runs.

## Approval Waiting

Async execution must not bypass permission confirmation. A permission `ask`
decision moves the run to `waiting` with `waiting.reason = "approval"` and
records the pending approval:

```json
{
  "status": "waiting",
  "waiting": {
    "reason": "approval",
    "message": "Tool permission approval is pending."
  },
  "approval": {
    "approvalId": "approval_001",
    "status": "pending",
    "request": {
      "toolName": "shell.exec",
      "actions": ["process.execute"],
      "resources": []
    }
  }
}
```

The host resolves the approval and calls resume. Workers must not busy-wait on
UI approval and must not default-allow blocked tools.

## Scheduler Boundary

The framework defines the scheduler interface but does not own platform timers:

```cpp
// Conceptual JSON ABI, not a required C++ virtual interface.
createWakeup({
  "runId": "run_async_sleep",
  "notBefore": "2026-06-13T01:00:00Z",
  "reason": "user_requested_timer",
  "metadata": {}
}) -> {
  "wakeupId": "wake_001",
  "provider": "host.scheduler"
}

cancelWakeup({
  "runId": "run_async_sleep",
  "wakeupId": "wake_001",
  "reason": "run_cancelled"
}) -> {
  "cancelled": true
}
```

Framework responsibilities:

- Persist `waiting` with `waiting.reason = "schedule"` before calling the host scheduler.
- Store `wakeupId`, `notBefore`, and `provider` on the run snapshot.
- Requeue due runs by transitioning `waiting` to `queued`.
- Emit `async_run.waiting` and `async_run.resumed`.

Host responsibilities:

- Implement durable timers or lifecycle wakeups using platform facilities.
- Call the framework resume entrypoint when a wakeup fires.
- Surface scheduler failures as `failed`, optionally with an
  `async_run.interrupted` event when the failure came from worker interruption.
- Keep platform APIs out of the framework core.

## C ABI and FFI

Bindings exchange only UTF-8 JSON strings. No C++ futures, Node promises,
thread handles, timer handles, or platform objects cross the ABI. A future C
ABI surface should add functions rather than changing existing signatures, for
example:

```c
int32_t agent_async_run_start_json(
    agent_runtime_t* runtime,
    const char* request_json,
    char** out_run_json);

int32_t agent_async_run_operation_json(
    agent_runtime_t* runtime,
    const char* operation_json,
    char** out_response_json);
```

The JSON contract remains the source of truth for C, C#, Rust, and other FFI
bindings. ABI functions only transport and own strings.
