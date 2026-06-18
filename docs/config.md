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

`load_native_agent_config` and the short `resolve_native_agent_app(config, "id")`
entry points remain available for callers that already pass the internal native
config value directly.

## Injected Runtime

Config resolution accepts injected adapters through
`NativeAgentAppResolveOptions`: provider transports, true provider streaming
transport, MCP transports, web fetch/search adapters, developer process
execution, browser rendering, llama.cpp bindings, and session-memory
configuration callbacks. The old long positional resolver/load overloads were
removed so new host integrations use named fields. No production
network/database/browser SDK is linked by default.

`configure_session_memory` is called after JSON `sessionOptions` are parsed and
before the configured `SessionStore` is constructed. Use it for native-only
callbacks such as an LLM-backed `SessionMemorySummarizer`; do not encode those
callbacks in JSON:

```cpp
agent::NativeAgentAppResolveOptions options;
options.requested_agent_id = "assistant";
options.configure_session_memory =
    [&](agent::SessionMemoryOptions& memory, const agent::Value& store_config) {
      memory.compaction.summarizer = agent::create_llm_session_summarizer({
          .model = summary_model,
      });
      memory.compaction.summarizer_mode = agent::SummarizerMode::Background;
    };

auto app = agent::resolve_native_agent_app(config, std::move(options));
```

The second callback argument is the resolved session-store config object, so
hosts can branch on `kind`, resource metadata, or other application-owned
fields without extending the framework JSON schema.

## Runtime Tool Calling

Agent runtime config can choose the tool-calling mechanism explicitly:

```json
{
  "agents": {
    "assistant": {
      "runtime": {
        "toolCallingStrategy": "native-tool-calling"
      }
    }
  }
}
```

Supported values are `text-react` and `native-tool-calling`. `text-react` uses
the framework ReAct text protocol. `native-tool-calling` passes structured tool
descriptors to the model adapter and executes `AgentOutput.tool_calls`.

## Built-In Model Providers

Native config recognizes the same first-class chat provider ids as the NodeJS
config package:

- Dedicated adapters: `ollama`, `openai`, `gemini`, `anthropic`, `qwen`,
  `deepseek`, `mimo`, `llamacpp-native`, and `echo`.
- OpenAI-compatible profiles: `lmstudio`, `llamacpp`, `vllm`,
  `openrouter`, `groq`, `siliconflow`, `omlx`, `kimi`, `zhipu`,
  `doubao`, `hunyuan`, `minimax`, and `qianfan`.

The OpenAI-compatible profiles keep their own provider ids in stream events,
usage records, fallback selection, and capability negotiation while sharing the
same `/chat/completions` adapter.

Built-in provider descriptors are assembled in the app-layer provider registry,
not inside the config resolver. Config validation asks the registry for
protocol, profile, capability, env-ref, usage, media, and stream metadata; new
providers should be added by registering descriptors rather than editing
`config.cpp`.

| Provider | Default baseUrl | API key env |
| --- | --- | --- |
| `lmstudio` | `http://127.0.0.1:1234/v1` | optional |
| `llamacpp` | `http://127.0.0.1:8080/v1` | `LLAMACPP_API_KEY` optional |
| `vllm` | `http://127.0.0.1:8000/v1` | `VLLM_API_KEY` optional |
| `openrouter` | `https://openrouter.ai/api/v1` | `OPENROUTER_API_KEY` |
| `groq` | `https://api.groq.com/openai/v1` | `GROQ_API_KEY` |
| `siliconflow` | `https://api.siliconflow.cn/v1` | `SILICONFLOW_API_KEY` |
| `omlx` | `http://127.0.0.1:8000/v1` | `OMLX_API_KEY` optional |
| `kimi` | `https://api.moonshot.ai/v1` | `MOONSHOT_API_KEY` |
| `zhipu` | `https://open.bigmodel.cn/api/paas/v4` | `ZHIPU_API_KEY` |
| `doubao` | `https://ark.cn-beijing.volces.com/api/v3` | `DOUBAO_API_KEY` |
| `hunyuan` | `https://api.hunyuan.cloud.tencent.com/v1` | `HUNYUAN_API_KEY` |
| `minimax` | `https://api.minimax.io/v1` | `MINIMAX_API_KEY` |
| `qianfan` | `https://qianfan.baidubce.com/v2` | `QIANFAN_API_KEY` |

`llamacpp` targets an external `llama-server` HTTP process. `llamacpp-native`
targets the in-process native binding. They are separate provider ids with
separate lifecycle and deployment assumptions.

## Prompt Stats Buckets

When the host owns a composed prompt, especially with
`runtime.reactPromptMode = "external"`, config can describe how that prompt
should be counted in context stats:

```json
{
  "agents": {
    "assistant": {
      "systemPrompt": "Business prompt\n\nReAct rules\n\nTool definitions",
      "runtime": {
        "reactPromptMode": "external",
        "promptStatsBuckets": [
          {
            "id": "external.system",
            "label": "External System",
            "kind": "system_prompt",
            "text": "Business prompt",
            "metadata": {"source": "host"}
          },
          {
            "id": "external.rules",
            "label": "External Rules",
            "kind": "rules",
            "text": "ReAct rules"
          },
          {
            "id": "external.tools",
            "label": "External Tools",
            "kind": "tool_definitions",
            "text": "Tool definitions"
          }
        ]
      }
    }
  }
}
```

Each entry requires `id` and `text`. `label` defaults to `id`, `kind` defaults
to `other`, and `metadata` must be an object when provided. Supported `kind`
values are `system_prompt`, `rules`, `tool_definitions`, `skills`, `mcp`,
`subagents`, `memory`, `knowledge`, `planning`, `conversation`, `context`, and
`other`.
