# Realtime API Notes

The native realtime module has two separate layers:

- `RealtimeBus`: in-memory or injected pub/sub for events, replay, and
  cross-component notification.
- `RealtimeSession`: a zero-dependency protocol surface for phone-style live
  AI sessions. It models a long-lived duplex session with text/audio input,
  audio/text output, interruptions, session updates, and tool calls.

The native layer does not ship WebSocket, WebRTC, microphone, speaker, codec,
VAD, or provider SDK implementations. Embedding applications provide transport
and audio behavior by implementing `RealtimeSessionProvider` and
`RealtimeSession`.

## Realtime Sessions

`RealtimeSessionProvider` opens long-lived `RealtimeSession` instances:

```cpp
class MyRealtimeProvider final : public agent::RealtimeSessionProvider {
 public:
  std::string name() const override { return "my-provider"; }

  std::unique_ptr<agent::RealtimeSession> open_session(
      agent::RealtimeSessionConfig config) override {
    // Create a business-owned WebSocket/WebRTC/native audio session here.
  }
};
```

Sessions expose a common lifecycle without owning the underlying transport:

```cpp
session->send_text("hello");
session->send_audio(agent::RealtimeAudioChunk{
    .data = base64_pcm16,
    .encoding = "pcm16",
    .sample_rate = 24000,
    .channels = 1,
});
session->commit_input_audio();
session->request_response();
session->cancel_response("barge-in");
```

Events use canonical type strings such as `session.opened`, `input.text`,
`input.audio.delta`, `input.audio.committed`, `response.started`,
`response.interrupted`, `output.text.delta`, `output.audio.delta`,
`tool.call.arguments.delta`, `tool.call.ready`, and `tool.call.result`.

Use `realtime_session_event_to_value` to produce the language-neutral JSON
shape consumed by bindings:

```cpp
agent::RealtimeSessionEvent event;
while (session->next_event(event)) {
  auto json = agent::safe_json_stringify(agent::realtime_session_event_to_value(event));
}
```

`RealtimeToolBridge` adapts realtime tool-call events to the existing
`ToolExecutor` path, preserving permissions, sandboxing, hooks, and
`ToolRunManager` injection. It can accumulate `tool.call.arguments.delta`
events until `tool.call.ready` arrives:

```cpp
agent::RealtimeToolBridge bridge(agent::RealtimeToolBridgeOptions{
    .executor = &tool_executor,
    .context = tool_context,
});

agent::RealtimeToolResult result;
if (bridge.execute_event(event, result)) {
  session->send_tool_result(result);
}
```

Long-running tools should return a ToolRun handle through the normal tool
result. The realtime layer should forward the handle and optionally mirror
ToolRun events as session events; it should not invent a second background task
system.

## Realtime Bus

The native realtime bus provides in-memory pub/sub, injected Redis-compatible
pub/sub, EventBus forwarding, replay buffers, and Value sequence piping.

## In-Memory Bus

`InMemoryRealtimeBus` supports NodeJS-style config-object construction:

```cpp
agent::InMemoryRealtimeBus bus(agent::InMemoryRealtimeBusOptions{
    .replay_limit = 100,
});
```

Publish messages with optional ids, timestamps, and metadata:

```cpp
agent::RealtimePublishOptions publish;
publish.id = "message-1";
publish.metadata = agent::Value::object({{"source", "runner"}});

auto message = bus.publish("runs.created",
                           agent::Value::object({{"runId", "run-1"}}),
                           publish);
```

Subscriptions can replay recent matching messages and can be removed by id or by
the returned subscription object:

```cpp
auto subscription = bus.subscribe(
    "runs.*",
    [](const agent::RealtimeMessage& message) {
      // Handle message.
    },
    agent::RealtimeSubscribeOptions{.replay = true});

bus.unsubscribe(subscription);
```

Set `replay_limit` to `0` to disable replay storage.

## Redis-Compatible Bus

`RedisRealtimeBus` uses injected zero-dependency publisher/subscriber clients:

```cpp
agent::RedisRealtimeBus redis(agent::RedisRealtimeBusOptions{
    .publisher = publisher,
    .subscriber = subscriber,
    .channel_prefix = "node-agent:realtime:",
    .replay_limit = 100,
});
```

The native layer does not ship a Redis network client. Production clients remain
injected through `RedisRealtimeClient`.

## EventBus Sink

```cpp
agent::EventBusRealtimeSink sink(bus, agent::EventBusRealtimeSinkOptions{
    .topic = "agent.events",
    .metadata = agent::Value::object({{"source", "event-bus"}}),
});

sink.attach(event_bus);
```

`topic_mapper` and `metadata_mapper` can derive topics and metadata from each
framework event.

## Value Pipe

```cpp
auto messages = agent::pipe_values_to_realtime(values, bus, "pipe.items");
```

Use a mapper to customize per-item topic, payload, and metadata.
