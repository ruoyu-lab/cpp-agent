# Model API

The native model module mirrors the NodeJS provider and embedding surfaces while
keeping transport, local inference, and media processing behind explicit C++
interfaces. The core library does not link provider SDKs, TLS stacks, or
llama.cpp by default.

## Chat Models

All chat providers implement `ChatModelAdapter`:

```cpp
#include "agent/agent.hpp"

auto model = std::make_shared<agent::EchoChatModelAdapter>();

agent::GenerateParams params;
params.messages = {
    agent::create_message(agent::MessageRole::User, "Write a short status update."),
};
params.settings.model = "echo";

agent::ModelResponse response = model->generate(params);
```

`GenerateParams` carries messages, tool descriptors, model settings, and an
optional `CancellationToken*`. `ModelResponse` preserves provider/model ids,
content parts, plain text, reasoning, tool calls, finish reason, and raw
provider data.

Use `resolve_settings` when callers combine adapter defaults with per-call
settings:

```cpp
agent::ModelSettings settings;
settings.temperature = 0.1;
settings.max_output_tokens = 800;

auto resolved = model->resolve_settings(settings);
```

`FallbackChatModelAdapter` accepts multiple adapters and tries them in order for
both non-streaming and streaming calls. The fallback path resolves settings per
adapter so each provider keeps its own default model and capability set.

## Provider Transports

Provider adapters build provider-specific JSON requests and parse native
responses, but they do not own network policy. Supply a `NativeProviderTransport`
for non-streaming calls and either a `NativeProviderStreamTransport` or
`NativeProviderStreamingTransport` for stream calls:

```cpp
agent::NativeProviderTransport transport =
    [](const agent::NativeProviderRequest& request) {
      // Send request.body to request.base_url + request.endpoint and return JSON.
      return agent::Value::object({});
    };

auto openai = std::make_shared<agent::OpenAICompatibleChatModelAdapter>(
    "openai",
    "model-id",
    transport,
    "/v1/chat/completions",
    "https://api.openai.com/v1",
    api_key);
```

Native provider adapters are available for:

- `OpenAICompatibleChatModelAdapter`
- `QwenChatModelAdapter`
- `MiMoChatModelAdapter`
- `AnthropicChatModelAdapter`
- `DeepSeekChatModelAdapter`
- `OllamaChatModelAdapter`
- `GeminiChatModelAdapter`
- `LlamaCppNativeChatModelAdapter`

The helper `create_native_provider_http_transport` bridges provider requests to
the native HTTP helper. Its default transport is intentionally limited to plain
HTTP. Use an injected transport for HTTPS/TLS, retries, proxying, tracing, or
organization-specific credentials.

Provider request builders and parsers are public for tests and custom adapters,
including OpenAI chat/responses, Qwen chat/responses, MiMo chat, Anthropic
messages, DeepSeek messages, Ollama chat, and Gemini generateContent.

`MiMoChatModelAdapter` is a pure OpenAI-compatible adapter for Xiaomi MiMo
(`https://api.xiaomimimo.com/v1`). It reads the `MIMO_API_KEY` env (or accepts
an explicit `apiKey`), defaults to `MIMO_MODEL` env → `mimo-v2-flash`, and
honors `MIMO_BASE_URL`. Config files select it via `{"provider": "mimo"}`. MiMo
does not expose provider-specific thinking fields, so no `enable_thinking` /
`thinking_budget` body keys are emitted and no reasoning capabilities are
advertised.

## Streaming

Streaming returns normalized `ModelStreamEvent` values:

```cpp
for (const auto& event : model->stream(params)) {
  if (event.type == agent::ModelStreamEventType::TextDelta) {
    // Append event.delta to the visible answer.
  }
}
```

Adapters also support callback streaming:

```cpp
model->stream(params, [](const agent::ModelStreamEvent& event) {
  if (event.type == agent::ModelStreamEventType::ReasoningDelta) {
    // Inspect event.reasoning.
  }
});
```

When an adapter has no streaming transport, the base implementation falls back
to a non-streaming `generate` call and emits a start event plus the final
response event.

## Reasoning

`ReasoningSettings` is shared across provider adapters and config validation:

```cpp
agent::ReasoningSettings reasoning;
reasoning.enabled = true;
reasoning.budget = std::string("medium");
reasoning.include_thoughts = false;
reasoning.tag_name = "think";

params.settings.reasoning = reasoning;
```

Helpers validate and normalize Node-style reasoning payloads:

- `assert_reasoning_settings`
- `reasoning_settings_from_json_value`
- `merge_reasoning_settings`
- `normalize_reasoning_budget_to_effort`
- `normalize_reasoning_budget_to_tokens`

Provider-native reasoning is parsed where the provider exposes it. For models
that emit tagged text, `extract_tagged_reasoning` and
`TaggedReasoningStreamParser` split visible text from `<think>...</think>`
style reasoning in both non-streaming and streaming flows.

## Prompt Caching

`ModelSettings` exposes opt-in prompt-cache controls. Observability is always
on; explicit cache markers are only injected when the caller asks for them.

```cpp
agent::ModelSettings settings;
settings.cache_strategy = agent::CacheStrategy::Explicit;
settings.cache_scope = agent::CacheScope::SystemAndTools;
// Optional verbatim key for OpenAI; leave empty to derive from a
// stable fingerprint of (system prompt + serialized tools).
settings.cache_key = "tenant-42:prompt-v3";
```

`CacheStrategy`:

- `None` (default) — never injects cache markers. The provider's own auto-cache
  still applies and observability still reports any cached tokens the provider
  reports back.
- `Explicit` — injects provider-specific cache markers.

`CacheScope` (only meaningful when `Explicit`):

- `SystemOnly` — only the system prompt block is marked.
- `SystemAndTools` (default) — system block plus the last tool entry are marked
  (this caches everything up through tool definitions).
- `SystemToolsAndSkills` — additionally marks the trailing user/system message
  treated as the skills preamble.

Use `to_string(CacheStrategy)` / `to_string(CacheScope)` for serialization;
`cache_strategy_from_string` / `cache_scope_from_string` round-trip the JSON
forms (`"none"`, `"explicit"`, `"system-only"`, `"system-and-tools"`,
`"system-tools-and-skills"`). `model_settings_to_json_value` emits
`cacheStrategy`, `cacheScope`, and (when set) `cacheKey`.

### Provider behavior

- **Anthropic** — adds `cache_control: {type: "ephemeral"}` to the last block
  of the `system` array and the last `tools` entry (and, in
  `SystemToolsAndSkills`, the last block of the most recent user/system
  message). When `cache_strategy` is `None` the system field stays a plain
  string and no `cache_control` is emitted anywhere.
- **OpenAI** — sends a top-level `prompt_cache_key` field. The key defaults to
  `"agent:" + sha256_hex(system_prompt + serialized_tools).substr(0, 16)`; set
  `cache_key` to override it verbatim. The field is only emitted when the
  adapter's provider id is `"openai"` — Qwen, DeepSeek, Ollama, and other
  OpenAI-compatible endpoints that share the chat-completions path are not
  affected.
- **Other providers** (Gemini, llama.cpp, etc.) — auto-cache only; no explicit
  markers are emitted regardless of `cache_strategy`.

### Usage parsing

`ModelUsage` carries `input_tokens`, `output_tokens`, `total_tokens`,
`cached_input_tokens`, `reasoning_tokens`, and `reasoning_source`.
`extract_model_usage` reads cache hits from:

- Anthropic `usage.cache_read_input_tokens` (and rolls
  `cache_read_input_tokens + cache_creation_input_tokens` back into
  `input_tokens` so the total prompt size is recovered for hit-rate math).
- OpenAI `usage.prompt_tokens_details.cached_tokens`.
- Gemini `usage_metadata.cached_content_token_count` (auto-cache only).

JSON round-trips as `cachedInputTokens` (camelCase, matching Node conventions).

### Reasoning tokens

`reasoning_tokens` reports how many tokens the model spent on internal thinking,
and `reasoning_source` records how that count was obtained:

| Source | Meaning |
|---|---|
| `Provider` | Provider directly reported the count. |
| `Estimated` | Estimated from response reasoning text using `chars / 4`. |
| `Unknown` | No data; the model may not have reasoned, or the provider does not expose it. |

Per-provider extraction:

| Provider | Reasoning source |
|---|---|
| `openai` | `usage.completion_tokens_details.reasoning_tokens` → `Provider`. Output_tokens already INCLUDES reasoning. |
| `qwen` | OpenAI-compat reasoning details; falls back to DashScope `output.reasoning_content_tokens`. |
| `deepseek` | OpenAI-compat reasoning details; otherwise estimated from `reasoning_content` text length. |
| `gemini` | `usage_metadata.thoughts_token_count` → `Provider`. Output_tokens is composed as `candidates_token_count + thoughts_token_count` (Gemini reports them separately). |
| `anthropic` | Provider does not report reasoning; falls back to estimation from `ModelResponse.reasoning` text. |
| `ollama`, `llama-cpp-native`, others | OpenAI-compat path attempted first; estimation fallback when reasoning text is present. |

Helpers:

- `to_string(ReasoningSource)` and `reasoning_source_from_string` round-trip
  the JSON form (`"provider" | "estimated" | "unknown"`).
- `merge_reasoning_source(a, b)` returns the worst-case (least authoritative)
  source — `Unknown` if either side is `Unknown`, else `Estimated` if either
  side is `Estimated`, else `Provider`. `merge_model_usage` uses this when
  aggregating per-call usage values across a run.

Note on `output_tokens` arithmetic: for OpenAI/Qwen/DeepSeek, `reasoning_tokens`
is a SUBSET of `output_tokens` (visible-output tokens ≈ `output_tokens
- reasoning_tokens`). For Gemini, `reasoning_tokens` is ADDED to
`candidates_token_count` to form `output_tokens`.

The estimation heuristic (`chars / 4`) is calibrated for English; it
under-counts CJK and other multi-byte scripts. Treat estimated counts as
order-of-magnitude when comparing across languages.

### Aggregated per-run usage

`AgentLoopRunResult` and `AgentRunnerRunResult` carry a `usage` field with the
sum of all model-call usages observed during the run. Token counts are summed
field-wise; `reasoning_source` is merged with the worst-case rule above so a
single iteration that lacked provider reasoning data degrades the overall
`reasoning_source` to `Estimated` or `Unknown` accordingly.

The `model.cache_stats` event payload includes `reasoningTokens` and
`reasoningSource` alongside `totalInputTokens`, `cachedInputTokens`, `hitRate`,
and `strategy`.

### When to enable

Explicit caching helps in long sessions, sustained traffic, and multi-tenant
workloads where the same system prompt / tool set is reused frequently. It can
hurt in dev/test or low-traffic scenarios — Anthropic charges a ~25% premium on
the first write of an ephemeral cache block, so paying for write amplification
without later reads is a net loss. The default `None` reflects that trade-off:
opt in once you have measured hit rate via the `model.cache_stats` event.

## Tools And Structured Output

`GenerateParams::tools` accepts `ChatToolDescriptor` values. Provider request
builders serialize those descriptors into each provider's tool format and parse
tool calls back into `ModelResponse::tool_calls`.

For local llama.cpp use cases, `json_schema_to_gbnf` and
`llama_tool_envelope_gbnf` build grammar constraints for structured output and
tool envelopes. Provider adapters that support structured content advertise the
`output.structuredContent` capability.

## Embeddings

Text embeddings implement `TextEmbeddingAdapter`; image embeddings implement
`ImageEmbeddingAdapter`:

```cpp
agent::HashEmbeddingAdapter text_embeddings(256);
auto vector = text_embeddings.embed_one("native runtime status");

agent::HashImageEmbeddingAdapter image_embeddings(256);
agent::ImageEmbeddingInput image;
image.alt_text = "diagram";
auto image_vector = image_embeddings.embed_one(image);
```

Built-in embedding adapters include:

- `HashEmbeddingAdapter` and `HashImageEmbeddingAdapter` for deterministic
  zero-dependency local vectors.
- `ClipTextEmbeddingAdapter` and `ClipImageEmbeddingAdapter` for injected CLIP
  batch functions.
- `OpenAIEmbeddingAdapter`, `QwenEmbeddingAdapter`, `OllamaEmbeddingAdapter`,
  and `GeminiEmbeddingAdapter` through `NativeProviderTransport`.
- `LlamaCppNativeTextEmbeddingAdapter` through the llama.cpp native binding.

`EmbeddingSettings` can override model, dimensions, task type, space id, and
extra provider options per call. `EmbeddingSpaceDescriptor` reports the vector
space id, modalities, and dimensions used by knowledge indexes.

Embedding methods accept a `CancellationToken*`; provider embeddings pass that
token through `NativeProviderRequest::cancellation`.

## Provider Registries

Use registries when a host application wants config-style provider creation:

```cpp
agent::ChatProviderRegistry chat_registry;
chat_registry.register_provider(
    "echo",
    [](const agent::Value&) {
      return std::make_shared<agent::EchoChatModelAdapter>();
    });

auto created = chat_registry.create("echo");
```

`EmbeddingProviderRegistry` and `ImageEmbeddingProviderRegistry` provide the
same factory pattern for text and image embedding adapters. The native config
resolver uses these registry shapes along with injected provider transports.

## llama.cpp Native Binding

The default build uses a stub llama.cpp binding that reports a configuration
error. To use the built-in binding, build with `AGENT_NATIVE_ENABLE_LLAMA_CPP=ON`
and provide llama.cpp headers/libraries at configure time. A host can also
inject its own `LlamaCppNativeBinding`.

```cpp
agent::LlamaCppNativeChatModelAdapterConfig config;
config.model = "local";
config.model_path = "/models/model.gguf";
config.library_path = "/opt/llama/lib/libllama.dylib";

agent::LlamaCppNativeChatModelAdapter local_model(config);
```

`LlamaCppNativeRuntimeConfig` carries runtime options such as context size,
batch size, threads, GPU layers, mmap/mlock flags, LoRA adapters, multimodal
projector paths, and media markers. Chat and embedding adapters register a
cancellation callback so cancelling the native token forwards the request id to
`LlamaCppNativeBinding::cancel`.

## Zero-Dependency Boundary

The model module contains provider-independent data structures, request
serializers, response parsers, stream parsers, deterministic hash embeddings,
registries, and injected-adapter bridges. It does not link cloud SDKs,
JavaScript runtimes, TLS libraries, CLIP runtimes, or llama.cpp in the default
build.
