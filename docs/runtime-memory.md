# Runtime Memory API

`AgentRunner` can retrieve long-term memory before model execution and write a
conversation turn back after a successful run or stream.

## Retrieval Defaults

Attach memory with `AgentRunnerConfig::long_term_memory`:

```cpp
auto memory = std::make_shared<agent::LongTermMemory>();
agent::AgentRunner runner(agent::AgentRunnerConfig{
    .adapter = model,
    .long_term_memory = memory,
});
```

Runner memory retrieval follows the NodeJS tri-state `enabled` behavior:

- omitted `enabled`: retrieve when long-term memory exists and the input text is
  not empty.
- `enabled = true`: force retrieval for that config or call.
- `enabled = false`: skip retrieval.

Per-call retrieval options are merged over runner defaults. `top_k`,
`min_score`, and `namespace_id` override the configured values when supplied.

## Writeback Defaults

Writeback follows the same NodeJS default:

- omitted `enabled`: write back when `LongTermMemory::auto_remember()` is true.
- `enabled = true`: write back even when the memory instance has
  `auto_remember = false`.
- `enabled = false`: skip writeback.

```cpp
auto result = runner.run(
    "remember this turn",
    "session-1",
    agent::ModelSettings{},
    agent::RunnerRetrievalOptions{.enabled = false},
    agent::RunnerWritebackOptions{
        .enabled = true,
        .namespace_id = "support",
        .metadata = agent::Value::object({{"source", "manual"}}),
    });
```

Writeback is skipped for empty input or empty model output. When `namespace_id`
is set, the memory record is written to that namespace. If the runner produced
an execution plan, the stored conversation turn includes a Node-style plan
summary such as `Plan: Inspect -> Reply`; otherwise it stores `Plan: none`.
