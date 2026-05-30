# CLI API

The native CLI module exposes the same command handler used by the
`native_agent_cli` executable. It is callable from C++ tests and host
applications by passing argument vectors and streams.

## Entry Points

Use `run_native_agent_cli` with explicit streams:

```cpp
std::istringstream input;
std::ostringstream output;
std::ostringstream error;

auto result = agent::run_native_agent_cli(
    {"validate", "--config", "node-agent.config.json"},
    input,
    output,
    error);
```

Overloads are available for:

- explicit stdin/stdout/stderr streams
- stdout/stderr with `std::cin`
- an injected `NativeConfigModuleLoader` for host-loaded JS/TS config modules

`NativeCliResult::exit_code` is `0` on success and `1` when command parsing or
execution throws an error.

## Commands

Supported command forms:

```text
native-agent [chat] [options] [prompt]
native-agent eval --config <file> --suite <file> [options]
native-agent replay <list|show> [options]
native-agent validate --config <file> [--agent <id>]
native-agent config validate --config <file> [--agent <id>]
```

`--help` prints usage and returns success. `--version` prints the native CLI
version string.

## Chat

Chat can run from a config file:

```cpp
agent::run_native_agent_cli(
    {"chat", "--config", "node-agent.config.json", "--agent", "assistant",
     "--session", "cli-session", "hello"},
    in,
    out,
    err);
```

Without `--config`, chat builds a manual native config from CLI options:

```text
native-agent chat --provider ollama --model llama3.2 "hello"
```

Manual mode supports these providers:

- `echo`
- `ollama`
- `openai`
- `gemini`
- `anthropic`
- `qwen`
- `deepseek`
- `llamacpp-native`

Provider credentials and endpoints are resolved through environment variables
such as `OPENAI_API_KEY`, `OPENAI_BASE_URL`, `OLLAMA_BASE_URL`,
`GEMINI_API_KEY`, `ANTHROPIC_API_KEY`, `QWEN_API_KEY`, `DEEPSEEK_API_KEY`, and
the llama.cpp native path variables.

Useful chat options:

- `--system <text>` for manual-mode system prompt.
- `--session <id>` for session id.
- `--sessions-dir <path>` for file-backed manual sessions.
- `--source <path|url>` to add manual knowledge sources.
- `--top-k <number>` and `--min-score <number>` for retrieval.
- `--no-stream` or `--stream`.
- `--debug` for retrieval details.

If no prompt is provided, chat enters the REPL.

## REPL

REPL commands:

- `:exit` or `:quit`: exit.
- `:clear`: clear the current session if a session store is configured.
- `:debug on` or `:debug off`: toggle retrieval debug output.

All other non-empty lines are sent to `AgentRunner`.

## Eval

Eval runs a suite against a config-backed runner:

```text
native-agent eval --config node-agent.config.json --suite evals/suite.json
```

Options:

- `--agent <id>` overrides the suite agent.
- `--report <file>` writes JSON.
- `--markdown-report <file>` writes Markdown.
- `--baseline <file>` compares against an existing baseline.
- `--update-baseline` overwrites the baseline.
- `--stop-on-failure` stops after the first failing case.
- `--tag <name>` and `--case <id>` filter cases.
- `--replay-dir <path>` sets replay artifact output.

When no report path is provided, the CLI writes
`.node-agent/reports/latest-eval-report.json`.

## Replay

List runs for a session:

```text
native-agent replay list --session cli --replay-dir .node-agent/runs
```

Show a run summary:

```text
native-agent replay show --run .node-agent/runs/cli/run-id
```

`show` prints run id, session id, status, duration, event count, generated HTML
path, and error when present.

## Validate

Validate and resolve a config:

```text
native-agent validate --config node-agent.config.json --agent assistant
```

The command loads the native resolved app, including injected host-loaded
config modules when a `NativeConfigModuleLoader` is supplied.

## Zero-Dependency Boundary

The CLI module parses arguments, builds native config values, invokes config,
runner, eval, and replay APIs, and writes reports through the framework's
filesystem helpers. It does not link provider SDKs, browser automation,
database drivers, package managers, or JavaScript runtimes by default. JS/TS
config execution is available only through an injected host loader.
