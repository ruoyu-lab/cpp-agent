# Tasks API Notes

The native tasks module provides task records, task runs, steps, events,
checkpoints, in-memory/file-backed stores, queues, workers, and injected
BullMQ/Postgres-compatible adapters.

## Stores

`FileTaskStore` supports NodeJS-style config-object construction:

```cpp
agent::FileTaskStore store(agent::FileTaskStoreConfig{
    .file_path = "tasks/state.json",
});
```

The file store persists tasks, runs, steps, events, and checkpoints in a single
JSON file and reloads them lazily on first access.

## Queues

`InMemoryTaskQueue` can be constructed with an options object:

```cpp
agent::InMemoryTaskStore store;
agent::InMemoryTaskQueue queue(agent::InMemoryTaskQueueOptions{
    .store = &store,
    .lease_ms = 30000,
});
```

Use `enqueue`, `claim`, `heartbeat`, `complete`, `fail`, and `cancel` to manage
task execution lifecycle state.

## Workers

```cpp
agent::AgentTaskWorker worker(agent::AgentTaskWorkerOptions{
    .store = &store,
    .queue = &queue,
    .handler = [](const agent::AgentTask& task, agent::TaskHandlerContext& context) {
      context.event("task.progress", agent::Value::object({{"stage", "running"}}));
      context.checkpoint("input", task.input);
      return agent::Value::object({{"ok", true}});
    },
    .lease_ms = 30000,
});

worker.run_once();
```

The task handler context exposes event and checkpoint helpers and carries a
cooperative cancellation token for active task cancellation.
