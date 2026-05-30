# Config API Notes

The native config module mirrors the NodeJS config package while keeping all
runtime integrations explicit and zero-dependency.

## Define Config

Use `define_native_agent_config` for in-memory config declarations. It returns
the config unchanged after checking that the value is an object, matching the
NodeJS `defineAgentConfig` helper.

```cpp
auto config = agent::define_native_agent_config(agent::Value::object({
    {"agents", agent::Value::object({
        {"assistant", agent::Value::object({
            {"model", agent::Value::object({
                {"provider", "echo"},
                {"model", "echo"},
            })},
        })},
    })},
}));
```

## Loaded Config

`NativeLoadedAgentConfig` carries the config plus `cwd` and `path`, matching the
NodeJS `LoadedAgentConfig` shape. The native resolver uses `cwd` for relative
paths such as file-backed session stores, knowledge stores, workflow stores,
skills, and MCP config files.

```cpp
auto loaded = agent::load_native_loaded_agent_config("node-agent.config.json");
auto app = agent::resolve_native_agent_app(loaded, "assistant");
```

For in-memory configs that still need relative-path semantics, attach an
explicit cwd:

```cpp
auto loaded = agent::define_native_loaded_agent_config(config, "/srv/agent");
auto app = agent::resolve_native_agent_app(loaded, "assistant");
```

Helpers that mirror NodeJS `AgentConfigInput` accept both raw config values and
loaded configs:

```cpp
auto agent_id = agent::resolve_agent_id(loaded);
auto definition = agent::resolve_agent_definition(loaded, agent_id);
auto env_keys = agent::collect_referenced_env_keys(loaded);
agent::assert_referenced_env_keys(loaded, env);
agent::validate_native_agent_config(loaded);
```

The existing `load_native_agent_config` and `resolve_native_agent_app(Value, ...)`
entry points remain available for callers that already pass the internal native
config value directly.

## Injected Runtime

Config resolution accepts the same injected adapters used elsewhere in the
native runtime: provider transports, MCP transports, web fetch/search adapters,
developer process execution, browser rendering, and llama.cpp bindings. No
production network/database/browser SDK is linked by default.
