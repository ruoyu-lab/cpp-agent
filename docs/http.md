# HTTP API

The core HTTP module provides small transport abstractions and request/stream
helpers. The optional native HTTP module adds a zero-dependency plain-HTTP
client used by full/app I/O adapters when the host opts into `agent_platform`
directly or through native helper targets such as `agent_runtime_io_native`.
Production network policy stays injectable.

## Transport Interface

Implement `HttpTransport` when the host application owns network execution:

```cpp
agent::HttpTransport transport =
    [](const agent::HttpRequest& request) {
      agent::HttpResponse response;
      response.status = 200;
      response.headers = {{"content-type", "application/json"}};
      response.body = R"({"ok":true})";
      response.final_url = request.url;
      return response;
    };
```

`HttpRequest` contains URL, method, headers, body, and an optional
`CancellationToken*`. `HttpResponse` contains status, headers, body, final URL,
and metadata.

Use `HttpStreamingTransport` when the host can deliver chunks:

```cpp
agent::HttpStreamingTransport streaming =
    [](const agent::HttpRequest& request,
       agent::HttpStreamingChunkHandler on_chunk,
       agent::HttpStreamingDoneHandler on_done) {
      if (on_chunk) {
        on_chunk("data: {\"delta\":\"hi\"}\n\n");
      }
      if (on_done) {
        on_done(200, {{"content-type", "text/event-stream"}}, std::nullopt);
      }
    };
```

## JSON Requests

`RequestJsonOptions` builds JSON POST-style requests:

```cpp
auto value = agent::request_json(
    agent::RequestJsonOptions{
        .base_url = "http://127.0.0.1:8080",
        .path = "/api/status",
        .method = "POST",
        .body = agent::Value::object({{"ping", true}}),
    },
    transport);
```

`build_http_request` combines `base_url` and `path`, sets a default
`content-type: application/json` header when none is present, serializes
`Value` bodies, and copies the cancellation token.

`request_json` parses JSON response bodies. Empty bodies become null values, and
non-JSON bodies are returned as strings. Non-2xx status codes throw
`AdapterError` with the parsed response payload attached as detail text.

`request_stream` uses the same request construction and non-2xx handling but
returns the raw `HttpResponse` body for callers that parse their own stream.

## Native Plain HTTP

`create_native_http_transport` creates a POSIX socket-backed HTTP/1.1 transport:

```cpp
#include "agent/http_native.hpp"

auto transport = agent::create_native_http_transport(agent::NativeHttpClientConfig{
    .timeout_ms = 30000,
    .max_response_bytes = 10 * 1024 * 1024,
});
```

The native transport supports plain `http://` URLs only. It intentionally does
not implement TLS, redirects, cookies, proxy policy, certificate validation, or
organization-specific authentication. Inject a custom transport for HTTPS/TLS
or production networking.

On Windows, the built-in native socket transport reports that it is unavailable.
Provide an injected transport there.

`create_native_http_streaming_transport` exposes the same plain-HTTP boundary
with chunk callbacks and a completion callback.

## Cancellation

`HttpRequest::cancellation` is checked before connecting, before sending, and
after receiving in the native transport. Streaming transport checks before
connect/send and after stream completion. Higher-level helpers in model, web,
media, and tools pass their own cancellation tokens through this field.

## Stream Helpers

The module includes parser helpers for provider and SSE-style streams:

```cpp
auto lines = agent::read_lines("a\nb\r\n");
auto events = agent::read_sse_events("data: one\n\ndata: two\n\n");
```

`read_lines` preserves trailing partial lines and normalizes CRLF endings.
`read_sse_events` collects `data:` lines, joins multi-line data blocks with
newlines, ignores comment lines, and flushes the final unterminated event.

## Zero-Dependency Boundary

`agent/http.hpp` uses only the C++ standard library and stays platform-neutral.
`agent/http_native.hpp` and `agent_platform` use POSIX sockets for the optional
default plain-HTTP client. Neither layer links TLS, HTTP/2, proxy, cookie,
cloud SDK, or JavaScript runtime dependencies. Those features belong in
injected transports.
