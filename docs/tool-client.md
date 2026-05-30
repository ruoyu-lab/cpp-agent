# Tool Client API

The native tool-client module lets one process broker tool calls to another
process or runtime without adding a network dependency to the core library.
Transports are injectable, and the built-in in-memory transport is enough for
tests and embedded hosts.

## Message Protocol

`ToolClientMessageType` serializes to the same event names used by the NodeJS
tool-client protocol:

- `register`
- `registered`
- `tool-call`
- `tool-result`
- `tool-error`
- `cancel`
- `heartbeat`
- `disconnect`

`ToolClientMessage` carries the client id, message id, timestamp, registered
tool descriptors, capabilities, tool call, request id, idempotency key, trace
context, result, output, error, and reason.

## Transports

Every transport implements `ToolClientTransport`:

```cpp
class MyTransport final : public agent::ToolClientTransport {
 public:
  void send(const agent::ToolClientMessage& message) override;
  std::string subscribe(agent::ToolClientMessageHandler handler) override;
  void unsubscribe(const std::string& subscription_id) override;
};
```

For local use, create a paired in-memory transport:

```cpp
auto pair = agent::create_in_memory_tool_client_transport_pair();
```

For realtime hosts, `SocketIoToolClientServerTransport` and
`SocketIoToolClientClientTransport` adapt injected socket/server interfaces. The
C++ library defines the interface shape only; the application supplies the
actual network implementation.

## Runtime

`ToolClientRuntime` registers local tools and executes incoming calls through
`ToolExecutor`:

```cpp
agent::JsonSchema schema;
schema.type = agent::JsonSchemaType::Object;
schema.required = {"text"};
schema.properties["text"].type = agent::JsonSchemaType::String;

auto echo = agent::define_tool(agent::ToolDefinition{
    .name = "remote.echo",
    .description = "Echo from a remote client.",
    .input_schema = schema,
    .capabilities = {"remote.exec"},
    .risk_level = agent::ToolRiskLevel::Low,
    .execute = [](const agent::Value& input,
                  agent::ToolExecutionContext&) -> agent::ToolInvokeResult {
      return agent::Value::object({{"text", input.at("text").as_string()}});
    },
});

agent::ToolClientRuntime runtime(agent::ToolClientRuntimeOptions{
    .client_id = "client-one",
    .tools = {echo},
    .transport = pair.client,
    .capabilities = {"remote.exec"},
});

runtime.start();
```

`start` subscribes to the transport, sends a `register` message, and publishes a
`client.registered` audit event when an event bus is configured. `stop` cancels
active calls, unsubscribes, and sends `disconnect`.

The runtime honors the same tool permissions and execution policies as local
tool execution. Active calls receive a `CancellationToken*`; incoming `cancel`
messages cancel the matching token.

## Broker

`ToolClientBroker` tracks registered clients and sends calls to the selected
runtime:

```cpp
agent::ToolClientBroker broker(
    pair.server,
    {},
    agent::ToolClientSecurityPolicy{
        .allowed_client_ids = {"client-*"},
        .allowed_tools = {"remote.*"},
        .allowed_capabilities = {"remote.exec"},
    });

auto result = broker.call_tool(agent::ToolClientBrokerCallOptions{
    .tool_call = agent::ToolCall{
        .id = "call_1",
        .name = "remote.echo",
        .arguments = agent::Value::object({{"text", "hello"}}),
    },
    .idempotency_key = "request-1",
});
```

The broker resolves a client by tool name and optional preferred client id. It
waits for a matching result/error message, applies timeout handling, sends a
`cancel` message on timeout, and publishes `call.started`, `call.completed`, or
`call.failed` audit events when configured.

## Security And Idempotency

`ToolClientSecurityPolicy` supports allow/deny lists with `*` wildcards for:

- client ids
- tool names
- capabilities

It also limits registered tool counts, serialized argument size, and serialized
result size.

Broker-side idempotency deduplicates pending calls with the same key and caches
completed results or errors. Runtime-side idempotency caches completed tool
results by explicit idempotency key, falling back to the tool-call id when no
key is provided. `max_idempotency_cache_size` bounds the runtime cache.

## Zero-Dependency Boundary

The module includes message types, registries, broker/runtime coordination,
security checks, cancellation handling, idempotency caches, audit events,
in-memory paired transports, and socket-shaped adapter interfaces. It does not
link a realtime, HTTP, database, or JavaScript runtime library by default.
