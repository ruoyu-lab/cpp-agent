# Execution API

The execution module provides cooperative cancellation, retry/timeout policies,
trace propagation, framework events, and realtime bridges. Runtime, tools,
providers, tasks, workflows, and server routes use these shared primitives.

## Cancellation

`CancellationToken` is cooperative:

```cpp
agent::CancellationToken token;
auto callback_id = token.add_callback([](const std::string& reason) {
  // Forward cancellation to an injected runtime.
});

token.cancel("caller aborted");
token.remove_callback(callback_id);
```

Use `throw_if_cancelled(target)` inside long-running work:

```cpp
token.throw_if_cancelled(agent::ExecutionTarget::Tool);
```

The thrown error includes the execution target and cancellation reason. When a
callback is added after cancellation, it is invoked immediately with the stored
reason.

## Execution Targets

`ExecutionTarget` values identify policy and trace scopes:

- `Run`
- `Model`
- `Tool`
- `Retrieval`
- `Permission`
- `Workflow`
- `WorkflowNode`
- `ChildAgent`

`to_string(ExecutionTarget)` returns the stable external label.

## Retry And Timeout Policies

`ExecutionPolicies` groups retry and timeout settings:

```cpp
agent::ExecutionPolicies policies;
policies.retry[agent::ExecutionTarget::Model] = agent::RetryPolicy{
    .max_attempts = 3,
    .strategy = agent::RetryStrategy::Exponential,
    .base_delay_ms = 100,
    .max_delay_ms = 1000,
    .retry_on = [](const agent::RetryContext& context) {
      return context.error.find("temporary") != std::string::npos;
    },
};
policies.timeout.timeout_ms[agent::ExecutionTarget::Model] = 30000;
```

Run work through `execute_with_policies`:

```cpp
auto value = agent::execute_with_policies(
    agent::ExecutionTarget::Model,
    policies,
    agent::Value::object({{"provider", "local"}}),
    &token,
    [] {
      return agent::Value("ok");
    });
```

Retry strategies are `None`, `Fixed`, and `Exponential`. Jitter uses a bounded
random delay below the resolved delay. Permission checks intentionally ignore
timeout values, matching the NodeJS permission-timeout behavior.

`sleep_with_cancellation` sleeps in small chunks and checks the token between
chunks.

## Trace Context

Trace helpers create and derive trace context without depending on an
observability SDK:

```cpp
auto root = agent::create_trace_context(agent::TraceContext{
    .span_name = "agent.run",
    .run_id = "run_1",
});

auto child = agent::derive_child_trace_context(
    root,
    agent::TraceSpanDescriptor{
        .name = "model.generate",
        .target = agent::ExecutionTarget::Model,
    });
```

Helpers include:

- `create_trace_context`
- `derive_child_trace_context`
- `create_child_trace_context`
- `get_trace_context`
- `normalize_framework_event_trace`

`TracePropagationPolicy` controls whether run id and workflow run id are
inherited and whether span ids are required.

## Event Bus

`EventBus` publishes framework events to registered sinks:

```cpp
agent::EventBus bus(agent::EventBus::Options{
    .include_raw = true,
});

auto sink_id = bus.register_sink([](const agent::FrameworkEvent& event) {
  // Inspect event.category, event.target, event.trace, and event.payload.
});

bus.publish("model.started",
            agent::ExecutionTarget::Model,
            agent::Value::object({{"model", "local"}}),
            child);

bus.unregister_sink(sink_id);
```

Each event receives an id and timestamp. Sink failures are isolated so one sink
cannot prevent other sinks from receiving the event.

## Realtime Bridge

The execution module also contains realtime buses and event bridges:

- `InMemoryRealtimeBus`
- `RedisRealtimeBus` through an injected client interface
- `EventBusRealtimeSink`
- `pipe_values_to_realtime`

See the Realtime API documentation for the realtime-specific surface. It is kept
in this module so EventBus and realtime bridging share the same event payload
format.

## Zero-Dependency Boundary

Execution uses standard-library synchronization, time, random, and callbacks.
It does not link a scheduler, tracing SDK, metrics SDK, realtime server,
database, or network client. External cancellation and realtime integrations
are injected through callbacks and interfaces.
