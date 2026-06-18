# Evals API Notes

The native evals module provides suite/case/report serialization, built-in
assertions, replay-per-case output, baseline comparison, Markdown reports, and
usage/cost assertions.

## Suite Helpers

```cpp
auto suite = agent::define_eval_suite(agent::EvalSuite{
    .pricing = agent::EvalPricing{
        .input_per_1k_tokens = 0.01,
        .output_per_1k_tokens = 0.03,
    },
    .cases = {
        agent::EvalCase{
            .id = "case-1",
            .input = "hello",
            .expect = {
                agent::expect_output_contains("hello"),
                agent::expect_token_under(1000),
            },
            .tags = {"smoke"},
        },
    },
});
```

`define_eval_suite` mirrors the NodeJS helper and returns the suite unchanged,
which keeps configuration-style C++ declarations explicit and type checked.

Eval cases accept either the C++ string convenience field or the NodeJS
`MessageInput` shape through `input_value`:

```cpp
agent::EvalCase{
    .id = "vision-case",
    .input_value = agent::Value::object({
        {"role", "user"},
        {"content", agent::Value::array({
            agent::Value::object({{"type", "text"}, {"text", "inspect"}}),
            agent::Value::object({
                {"type", "image"},
                {"source", agent::Value::object({
                    {"kind", "path"},
                    {"path", "screenshot.png"},
                    {"mimeType", "image/png"},
                })},
                {"altText", "screenshot"},
            }),
        })},
        {"metadata", agent::Value::object({{"source", "eval"}})},
    }),
};
```

## Running

```cpp
agent::AgentRunner runner(config);

auto report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
    .suite = suite,
    .runner = &runner,
    .tags = {"smoke"},
    .report_path = "reports/eval.json",
    .markdown_report_path = "reports/eval.md",
    .replay_dir = "reports/replays",
    .baseline_path = "reports/baseline.json",
    .update_baseline = true,
});
```

The object-style `RunEvalSuiteOptions` overload matches the NodeJS
`runEvalSuite(options)` shape for runner-backed native suites. It can use a
direct runner, a per-case runner factory, an in-memory native config object, or
a config file/directory:

```cpp
auto report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
    .suite = suite,
    .create_runner = [](const agent::EvalCase& test_case) {
        agent::AgentRunnerConfig config;
        config.model_runtime.adapter = model_for_case(test_case.id);
        return std::make_shared<agent::AgentRunner>(std::move(config));
    },
});
```

```cpp
auto config_report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
    .suite = agent::EvalSuite{
        .agent = "assistant",
        .cases = suite.cases,
    },
    .config_path = "node-agent.config.json",
});
```

Pass `loaded_config` when the caller already has a `NativeLoadedAgentConfig`
and needs NodeJS-style `AgentConfigInput` semantics with explicit `cwd/path`:

```cpp
auto loaded = agent::define_native_loaded_agent_config(config, "/srv/agent");
auto report = agent::run_eval_suite(agent::RunEvalSuiteOptions{
    .suite = suite,
    .loaded_config = loaded,
    .agent = "assistant",
});
```

For JavaScript/TypeScript config modules, provide `config_module_loader`; the
native eval runner keeps the resolved app alive for the duration of each case so
runner-owned services such as knowledge, workflow, and tools remain valid.
The same zero-dependency injected adapters accepted by `load_native_agent_app`
are available on `RunEvalSuiteOptions`: `provider_transport`,
`provider_stream_transport`, `provider_streaming_transport`,
`mcp_transport_factory`, `web_adapters`, `developer_adapters`,
`browser_adapters`, and `llama_adapters`. Injected runtime dependencies use
named option fields; the old long positional resolver/load overloads are no
longer part of the public API.

Use `load_eval_report` for reports and `load_eval_baseline` for baselines;
baselines use the same JSON shape as eval reports.
