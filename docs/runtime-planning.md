# Runtime Planning API

The native C++ runner supports the same planning layers as the NodeJS
`AgentRunner`: a runner-level planner configuration plus a per-call planning
switch.

## Runner Configuration

Attach a planner through `AgentRunnerConfig::planner`:

```cpp
auto planner = std::make_shared<agent::ModelPlanner>(agent::ModelPlannerConfig{
    .model = planning_model,
    .max_steps = 4,
});

agent::AgentRunner runner(agent::AgentRunnerConfig{
    .adapter = answer_model,
    .tools = tools,
    .planner = planner,
    .enable_planning = true,
});
```

When planning is enabled, `AgentRunner::run` and `AgentRunner::stream` build an
`ExecutionPlan`, inject it into the model preface, and expose it through the
final runner result. Stream calls emit planning status events when the planner
runs. They emit the `Planning` enum event, serialized externally as `plan`, only
when the planner returns a structured plan.

## Per-Call Disable

Both `run` and `stream` accept a trailing `bool enable_planning` argument. It
defaults to `true`, so existing calls keep their behavior.

```cpp
auto result = runner.run(
    "answer directly",
    "session-1",
    agent::ModelSettings{},
    agent::RunnerRetrievalOptions{},
    agent::RunnerWritebackOptions{},
    {},
    agent::Value::object({}),
    std::nullopt,
    agent::AgentRunnerDurableOptions{},
    nullptr,
    false);
```

```cpp
auto stream = runner.stream(
    "answer directly",
    "session-1",
    agent::ModelSettings{},
    agent::RunnerRetrievalOptions{},
    agent::RunnerWritebackOptions{},
    {},
    agent::Value::object({}),
    std::nullopt,
    nullptr,
    false);
```

Passing `false` skips planner execution for that call. The result has no plan,
and stream calls do not emit planning status or `plan` events. Runner-level
`enable_planning = false` still disables planning for every call.
