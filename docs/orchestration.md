# Orchestration API Notes

The native orchestration module provides artifact stores, shared state,
mailboxes, routing strategies, replanning strategies, and coordinator loops.

Artifact stores, shared state stores, and mailboxes are coordination substrate.
They are useful as canonical state in small local deployments, and as
`runtime-owned` or `derived` infrastructure in larger hosts. Business records
and tenant-owned domain data should remain in the embedding application.

## File Stores

File-backed artifact and shared-state stores support NodeJS-style config-object
construction:

```cpp
agent::FileArtifactStore artifacts(agent::FileArtifactStoreConfig{
    .base_dir = "orchestration/artifacts",
    .namespace_id = "team:ops",
});

agent::FileSharedStateStore shared_state(agent::FileSharedStateStoreConfig{
    .base_dir = "orchestration/state",
    .namespace_id = "team:ops",
});
```

Namespaces are encoded with `encodeURIComponent`-style filenames, matching the
NodeJS stores. For example, `team:ops` is stored as `team%3Aops.json` under the
configured base directory. Shared-state files use the NodeJS array persistence
shape; older native `{ "records": [...] }` files are still readable.

## Artifacts

```cpp
artifacts.set("draft", agent::Value::object({{"text", "initial"}}));
artifacts.append_log("draft", "reviewed", agent::Value::object({{"ok", true}}));

auto snapshot = artifacts.snapshot();
auto history = artifacts.history("draft");
```

Calling `clear()` without a key records a `clear-all` history entry, matching
NodeJS artifact history semantics.

## Shared State

```cpp
shared_state.merge("task", agent::Value::object({{"status", "running"}}));
shared_state.merge("task", agent::Value::object({{"owner", "writer"}}));

auto task = shared_state.get("task");
```

`merge` preserves existing object fields and overlays new fields.
