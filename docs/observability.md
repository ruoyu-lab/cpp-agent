# Observability API Notes

The native observability module mirrors the NodeJS observability packages with
structured logs, metrics, traces, memory adapters, console adapters, and an
OpenTelemetry bridge interface.

## Pipeline

`ObservabilityPipeline` can be built with a NodeJS-style config object:

```cpp
agent::ObservabilityPipeline pipeline(agent::ObservabilityPipelineConfig{
    .loggers = {[](const agent::StructuredLogRecord& record) {
      // Send structured log record to a sink.
    }},
    .metrics = {[](const agent::MetricRecord& record) {
      // Record metric.
    }},
    .traces = {[](const agent::TraceRecord& record) {
      // Record trace.
    }},
    .on_adapter_error = [](const agent::ObservabilityAdapterError& error) {
      // Adapter failures are isolated from the rest of the pipeline.
    },
});
```

The pipeline fans out one structured log record, one or more metric records, and
one trace record for each framework event.

## Memory

```cpp
auto collector = std::make_shared<agent::MemoryObservabilityCollector>();
auto memory = agent::create_memory_observability_pipeline(agent::MemoryAdapterConfig{
    .collector = collector,
});
```

The collector stores log, metric, and trace records in memory and exposes
snapshot accessors plus `reset()`.

## Console

```cpp
auto console_pipeline = agent::create_console_observability_pipeline();
```

Console adapters write JSON-serialized log, metric, and trace records to output
streams. Error logs and error traces are routed to the error stream.

## OpenTelemetry Bridge

OpenTelemetry support is provided through an injected zero-dependency bridge:

```cpp
class MyBridge final : public agent::OpenTelemetryBridge {
 public:
  void log(const agent::StructuredLogRecord& record) override;
  void counter(const std::string& name, double value, const agent::Value& attributes) override;
  std::unique_ptr<agent::OpenTelemetrySpan> start_span(
      const std::string& name,
      const agent::Value& attributes) override;
};

MyBridge bridge;
auto otel = agent::create_opentelemetry_observability_pipeline(
    agent::OpenTelemetryAdapterConfig{.bridge = &bridge});
```

No OpenTelemetry SDK is linked by default; production exporters remain injected
through the bridge.

## Prompt Cache Stats

After every model call (run and stream paths alike) the runner publishes a
`model.cache_stats` event on the event bus whenever the provider reported any
prompt-side tokens. Emission is unconditional with respect to `cache_strategy`
— even with the default `CacheStrategy::None`, providers that auto-cache
(Anthropic, Gemini) will still surface cached-token counts.

Event category: `model.cache_stats`
Execution target: `ExecutionTarget::Run`

Payload shape:

```json
{
  "provider": "anthropic",
  "model": "claude-3-7-sonnet",
  "totalInputTokens": 1200,
  "cachedInputTokens": 900,
  "hitRate": 0.75,
  "strategy": "explicit"
}
```

- `totalInputTokens` is the full prompt size. For Anthropic this is recovered
  as `usage.input_tokens + cache_read_input_tokens + cache_creation_input_tokens`
  so that `hitRate = cachedInputTokens / totalInputTokens` is meaningful.
- `cachedInputTokens` is the cache-hit portion only (writes are not counted).
- `hitRate` is `0.0` when `totalInputTokens` is zero.
- `strategy` is the serialized `CacheStrategy` (`"none"` or `"explicit"`).

See [model.md](./model.md#prompt-caching) for the request-side `CacheStrategy`
/ `CacheScope` controls and the trade-offs around enabling explicit caching.
