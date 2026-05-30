# Autonomous API

The native autonomous module provides durable long-running runs made of planned
steps, checkpoints, events, waiting states, resume, and cancellation. It is
separate from the task queue: tasks schedule work; autonomous runs model a
multi-step goal.

## Data Model

An autonomous run contains:

- `AutonomousRun`: id, goal, input, status, output, error, timestamps, metadata.
- `AutonomousStep`: step id, index, objective, dependencies, status, attempts,
  output, wait reason, timestamps, metadata.
- `AutonomousEvent`: append-only lifecycle event.
- `AutonomousCheckpoint`: named durable state snapshot.
- `AutonomousRunSnapshot`: run plus steps, events, and checkpoints.

Run statuses are `queued`, `running`, `waiting`, `completed`, `failed`,
`cancelled`, and `interrupted`. Step statuses are `pending`, `running`,
`waiting`, `completed`, `failed`, `cancelled`, and `skipped`.

## Stores

Use an in-memory or file-backed store:

```cpp
agent::FileAutonomousStore store("autonomous/state.json");
```

`AutonomousStore` also supports an injected Postgres-style query client through
`PgAutonomousStore`. The native library owns the SQL semantics and lifecycle,
but the database client remains supplied by the host application.

Stores expose run creation, snapshot loading, run/step updates, step
replacement/appending, event appending, checkpoint appending, and filtered run
listing.

## Planning

Planners implement `AutonomousPlanner`:

```cpp
agent::StaticAutonomousPlanner planner(agent::AutonomousPlan{
    .summary = "Ship the change",
    .steps = {
        agent::AutonomousPlanStep{
            .id = "inspect",
            .title = "Inspect state",
            .objective = "Read current files",
        },
        agent::AutonomousPlanStep{
            .id = "edit",
            .title = "Apply edit",
            .objective = "Make the scoped change",
            .depends_on = {"inspect"},
        },
    },
});
```

`StaticAutonomousPlanner` accepts a plan, a step list, or a callback.
`AgentAutonomousPlanner` can ask an `AgentRunner` to produce the plan from the
run goal and input.

## Step Execution

Executors implement `AutonomousStepExecutor`:

```cpp
agent::CallbackAutonomousStepExecutor executor(
    [](const agent::AutonomousStepExecutionInput& input) {
      input.checkpoint("executor.state", agent::Value::object({
          {"stepId", input.step.id},
          {"previousSteps", input.previous_steps.size()},
      }));

      return agent::AutonomousStepExecutionResult{
          .output = agent::Value::object({{"done", true}}),
      };
    });
```

An executor can:

- Return `Completed` with output.
- Return `Waiting` with `wait_reason` to pause for external input.
- Return `next_steps` to append follow-up steps.
- Add checkpoints through the provided callback.
- Throw to fail the run and step.

`AgentRunnerStepExecutor` runs each step through an `AgentRunner`, using either
a fixed session id or a session resolver.

## Run Manager

`AutonomousRunManager` wires store, planner, and executor:

```cpp
agent::AutonomousRunManager manager(agent::AutonomousRunManagerConfig{
    .store = &store,
    .planner = &planner,
    .executor = &executor,
    .max_steps_per_run = 64,
});

auto run = manager.create_run(agent::CreateAutonomousRunInput{
    .id = "run_1",
    .goal = "Ship the feature",
    .input = agent::Value::object({{"scope", "docs"}}),
});

auto snapshot = manager.run(run.id);
```

`run` plans if needed, requeues interrupted running steps, executes pending
steps whose dependencies are complete, pauses on waiting steps, and completes
the run when no executable work remains.

Lifecycle events include:

- `autonomous.run.created`
- `autonomous.run.planned`
- `autonomous.run.started`
- `autonomous.step.started`
- `autonomous.step.completed`
- `autonomous.step.waiting`
- `autonomous.step.interrupted`
- `autonomous.step.retry-scheduled`
- `autonomous.run.completed`
- `autonomous.run.failed`
- `autonomous.run.cancelled`

## Resume, Waiting, And Cancel

`resume(runId)` restarts incomplete work. Failed steps are requeued by default;
pass `false` to keep a failed step failed.

`complete_waiting_step(runId, stepId, output)` marks a waiting step as
completed, records an external completion event, and returns the run to queued
state so it can continue.

`cancel(runId, reason)` marks the run cancelled and cancels pending, running, or
waiting steps with the same reason.

## Serialization

Use `autonomous_run_to_value`, `autonomous_step_to_value`,
`autonomous_event_to_value`, `autonomous_checkpoint_to_value`, and
`autonomous_run_snapshot_to_value` for API responses and persisted artifacts.
The matching `*_from_value` helpers restore native records from serialized
values.

## Zero-Dependency Boundary

The autonomous module includes data records, in-memory and file stores,
injected-query persistence, planners, executors, manager lifecycle logic,
events, checkpoints, and serialization. It does not link a database driver,
queue system, scheduler, or external agent runtime by default.
