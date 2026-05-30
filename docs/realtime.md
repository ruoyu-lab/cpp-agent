# Realtime API Notes

The native realtime module provides in-memory pub/sub, injected Redis-compatible
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
