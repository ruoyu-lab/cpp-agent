# MCP API

The native MCP module mirrors the NodeJS MCP package with JSON-RPC helpers,
Anthropic `.mcp.json` loading, transport contracts, in-process servers, and
adapters that expose remote MCP tools, resources, and prompts to the C++
runtime.

`agent_mcp` stays protocol-only and platform-neutral. `agent_mcp_native` adds
the optional zero-dependency stdio subprocess and plain HTTP JSON-RPC POST
transports. HTTPS, SSE, WebSocket, and hosted production MCP transports should
be provided through injected transport implementations.

## Transport Governance Contract

The native boundary is intentionally narrower than the NodeJS package:

- stdio and plain HTTP are first-party native transports.
- HTTPS, SSE, and WebSocket are supported through injected `MCPTransport`
  implementations so embedders can provide their own TLS/event-loop/socket
  stack.
- stdio and HTTP transports use explicit response/request timeouts and fail
  closed on timeout.
- `MCPClient` serializes request/response matching with a request mutex. This is
  the native concurrency contract today: embedders that need broader
  concurrency should provide a transport/client wrapper with an explicit limit.
- Tool adapters always tag tools with `mcp` and `mcp:<server>`, and optionally
  `server:<server>`. NodeJS additionally exposes `scope:<scope>` tags for
  `.mcp.json` server scope; native embedders should model scope in their host
  registry or add equivalent tags when constructing tools.
- Native adapters do not truncate tool descriptions by default. Hosts that
  expose untrusted MCP servers should truncate before presenting tool catalogs
  to a model, matching the NodeJS `maxDescriptionLength` policy.

## Anthropic Config

Use the Anthropic-style `.mcp.json` helpers when loading MCP servers from disk:

```cpp
#include "agent/mcp_native.hpp"

auto config = agent::load_anthropic_mcp_config_file("/workspace/.mcp.json", {
    {"HOME", "/Users/service"},
    {"TOKEN", "local-token"},
});

auto server = config.mcp_servers.at("local");
auto transport = agent::create_native_mcp_transport(server);
```

`parse_anthropic_mcp_config` expands `${ENV}` and `${ENV:-fallback}` values in
commands, args, env, cwd, URLs, and headers. `resolve_anthropic_mcp_server_config`
resolves relative `command` and `cwd` values against the config file location.

When `type` is omitted, configs with `url` default to `http`; configs without
`url` default to `stdio`.

## Transports

All transports implement `MCPTransport`:

```cpp
class MCPTransport {
 public:
  virtual void start(agent::MCPMessageHandler handler) = 0;
  virtual void send(const agent::Value& message) = 0;
  virtual void close() = 0;
};
```

Available transports:

- `MCPCallbackTransport` / `MCPHttpTransport`: adapter-friendly wrappers around
  a synchronous request/response callback.
- `MCPLineTransport`: newline-delimited JSON-RPC over caller-owned streams.
- `MCPStdioTransport` (`agent/mcp_native.hpp`): starts a subprocess, writes newline-delimited JSON-RPC
  requests to stdin, and waits for matching responses on stdout.
- `MCPNativeHttpTransport` (`agent/mcp_native.hpp`): sends JSON-RPC messages with HTTP POST to a plain
  `http://` URL.

`create_native_mcp_transport` supports `stdio` and `http`. It intentionally
rejects unsupported native transport types instead of silently falling back to a
different transport.

For SSE/WebSocket parity, implement `MCPTransport::start/send/close` and inject
the transport into `MCPClient`. This keeps the native library independent from a
specific event-loop or WebSocket implementation while preserving the same
JSON-RPC client contract.

## Client

`MCPClient` performs the MCP initialize handshake and exposes tool, resource,
and prompt operations:

```cpp
#include "agent/mcp_native.hpp"

auto client = std::make_shared<agent::MCPClient>(
    "docs",
    std::make_shared<agent::MCPStdioTransport>(agent::MCPStdioTransportConfig{
        .command = "/usr/local/bin/docs-mcp",
        .args = {"--project", "/workspace"},
    }));

client->connect();
auto tools = client->list_tools();
auto resource = client->read_resource("resource://docs/readme");
client->close();
```

Requests are synchronized and matched by JSON-RPC id. Delayed injected
transports can respond asynchronously through the transport handler as long as
the response arrives before the configured request timeout.

## In-Process Server

Use `InProcessMCPServer` when a native process wants to expose local tools,
resources, or prompts without spawning a subprocess:

```cpp
agent::InProcessMCPServer server("local", "1.0.0");
server.register_tool(agent::define_tool(agent::ToolDefinition{
    .name = "echo",
    .description = "Echo text",
    .execute = [](const agent::Value& input, const agent::ToolExecutionContext&) {
      return input.at("text").as_string();
    },
}));

agent::InProcessMCPClient client(server);
client.connect();
auto result = client.call_tool("echo", agent::Value::object({{"text", "hello"}}));
```

In-process resources preserve multi-content reads. Prompt render callbacks
return `AgentMessage` values so they can be inserted directly into runtime
context.

## Runtime Adapters

`create_mcp_tool_definitions` converts remote MCP tools into native
`ToolDefinition` values:

```cpp
auto remote_tools = agent::create_mcp_tool_definitions(client, "docs");
agent::ToolRegistry registry(remote_tools);
```

By default tool names are prefixed with the MCP client name, matching the
NodeJS adapter shape. Schema-less remote tools receive a default object input
schema.

Resource and prompt helpers provide direct runtime content:

```cpp
auto text = agent::read_mcp_resource_text(*client, "resource://docs/readme");
auto message = agent::create_mcp_resource_message(*client, "resource://docs/readme");
auto prompt_messages = agent::create_mcp_prompt_messages(
    *client,
    "review",
    agent::Value::object({{"path", "src/agent.cpp"}}));
```

Tool errors preserve MCP error content and metadata. Structured MCP results can
be rendered for logs or prompts with `render_mcp_structured_value`.
