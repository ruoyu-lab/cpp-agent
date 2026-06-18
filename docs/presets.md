# Presets

Native presets live above `agent_runtime`. They are convenience assembly
functions for hosts that want a standard starting point without accepting a
larger core contract.

## Default vs Advanced Entry Points

| Entry point | Layer | Use for |
|---|---|---|
| `AgentRuntimeBuilder` | `agent_runtime` | Advanced embedding. The host supplies model adapters, tools, memory, governance, and observability directly. |
| `create_standard_runtime()` | `agent_app` | Standard local assembly. Adds default core tools and uses an Echo model when the host has not supplied a model. |
| `create_local_model_runtime()` | `agent_app` | Local-model assembly. The host supplies a `ChatModelAdapter`, or a llama.cpp-native config with `model_path`. |
| `create_server_app()` | `agent_server` | Standard server assembly. It preserves the fail-closed auth posture of `AgentServerApp`. |

The rule is simple: defaults belong in preset/app/server layers, not in
`agent_core`, `agent_model`, or `agent_runtime`. C++ remains embeddable by
default; the preset layer only shortens common setup.

## Standard Runtime

```cpp
#include "agent/app_api.hpp"

agent::StandardRuntimePresetOptions options;
options.system_prompt = "You are a concise assistant.";
options.max_iterations = 4;
options.model = my_model_adapter;

auto runner = agent::create_standard_runtime(std::move(options)).build();
auto result = runner.run("Summarize this.", "session-1");
```

`create_standard_runtime()` adds the `core` tool bundle unless
`tool_bundles` is replaced. Extra host tools can be appended through
`options.tools`.

## Local Model Runtime

```cpp
#include "agent/app_api.hpp"

agent::LocalModelRuntimePresetOptions options;
options.runtime.tool_bundles = {"core"};
options.llama_cpp.model_path = "/models/local.gguf";

auto runner = agent::create_local_model_runtime(std::move(options)).build();
```

If neither `options.runtime.model` nor `options.llama_cpp.model_path` is set,
`create_local_model_runtime()` throws `ConfigurationError`. That keeps the
embedded host in control of the actual model runtime.

## Server App

```cpp
#include "agent/server_api.hpp"

agent::AgentServerOptions options;
options.runner = &runner;
options.auth = agent::AgentServerAuthConfig{.bearer_tokens = {"dev-token"}};

auto app = agent::create_server_app(std::move(options));
```

`create_server_app()` does not silently create a public server. Set
`allow_unauthenticated = true` only for an explicitly public or development
server.
