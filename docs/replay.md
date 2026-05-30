# Replay API

The native replay module materializes agent runs into a portable directory with
JSON inputs, JSON events, optional JSON results, a manifest, and a standalone
HTML view.

## Writing Replays

Use `write_run_replay` when an application already has a run id, session id,
input, result, and stream events:

```cpp
auto written = agent::write_run_replay(agent::WriteRunReplayOptions{
    .base_dir = "replays",
    .run_id = "run_1",
    .session_id = "session_1",
    .agent_id = "assistant",
    .input = agent::Value::object({{"prompt", "Summarize"}}),
    .result = agent::Value::object({{"text", "Done"}}),
    .events = {
        agent::Value::object({{"type", "status"},
                              {"status", agent::Value::object({{"stage", "model"}})}}),
    },
});
```

The result includes:

- `dir_path`: run directory.
- `manifest_path`: `manifest.json`.
- `html_path`: generated `replay.html`.
- `manifest`: the in-memory manifest record.

The convenience overload returns only the manifest:

```cpp
auto manifest = agent::write_run_replay(
    "replays",
    "session_1",
    agent::Value("input"),
    agent::Value::object({{"text", "output"}}));
```

## Directory Layout

Runs are written under:

```text
<baseDir>/<sanitizedSessionId>/<sanitizedRunId>/
```

Each run directory contains:

- `manifest.json`
- `input.json`
- `events.json`
- `result.json` when a non-null result is provided
- `replay.html`

`RunReplayManifest` stores version, run id, session id, agent id, timestamps,
duration, status, event count, tool-call count, file names, error, and metadata.
The manifest serializer emits Node-style camelCase keys and the parser also
accepts snake_case aliases.

## Loading And Listing

Load a replay from a run directory, `manifest.json`, or `replay.html` path:

```cpp
auto replay = agent::load_run_replay("replays/session_1/run_1");
auto html = agent::render_run_replay_html(replay);
```

List known run directories for a session:

```cpp
auto runs = agent::list_session_replays("replays", "session_1");
```

Only directories containing `manifest.json` are returned.

## Event Summary

Replay writing and rendering summarize stream events:

- `eventCount` is the number of stored events.
- `toolCallCount` is derived from `tool-start` events and nested loop
  `tool-start` events.
- The HTML view lists status stages collected from `status` events.

The event payload remains unmodified in `events.json`; summaries are additional
manifest/view metadata.

## Server And Eval Integration

Server chat/stream routes can materialize replays through the same writer.
Eval execution also uses replay paths for per-case artifacts. Those integrations
store the same replay directory shape, so `load_run_replay` can inspect either
source.

## Zero-Dependency Boundary

Replay uses JSON helpers, filesystem writes, and a small built-in HTML renderer.
It does not require a browser engine, template engine, database, JavaScript
runtime, or web server.
