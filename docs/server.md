# Server API

The native server module exposes a **transport-independent HTTP app**. It is
the API contract layer for chat, stream, sessions, approvals, tasks,
autonomous runs, workflows, health, readiness, metrics, audit logs, replay
materialization, auth, rate limiting, API-key quotas, and response governance
— independent of any specific HTTP transport, TLS stack, or web framework.

The native build deliberately does **not** ship an HTTP listener. The
production server tier lives in the host language (NodeJS / Java / PHP /
Go / …). Hosts call `AgentServerApp::handle` from their own HTTP server,
or call it directly through the [agent_capi shim](bindings.md) when embedding
from another language.

## App Entry Point

`AgentServerApp` is the only public type. Hosts own the transport:

```cpp
agent::AgentServerApp app(agent::AgentServerOptions{
    .runner = &runner,
    .server_name = "native-agent",
});

auto response = app.handle(agent::HttpRequest{
    .url = "/chat",
    .method = "POST",
    .headers = {{"content-type", "application/json"}},
    .body = agent::Value::object({
        {"input", "Summarize the session."},
        {"sessionId", "session-1"},
    }).stringify(0),
});
```

`handle` matches a method/path route, applies trace headers, access control,
CORS, metrics, audit handling, and returns an `HttpResponse`. Custom routes can
be added with `add_route`.

Helpers:

- `read_json_body` parses bounded JSON request bodies.
- `send_json` creates JSON responses.
- `send_sse` creates event-stream responses.
- `match_route_pattern` matches route patterns with `:param` segments.
- `HttpRequestError` carries an HTTP status and JSON error payload.

## Built-In Routes

Core routes:

- `GET /health`
- `GET /ready`
- `GET /whoami`
- `GET /metrics`
- `POST /chat`
- `POST /stream`

Session routes:

- `GET /sessions`
- `GET /sessions/:sessionId`
- `DELETE /sessions/:sessionId`

Approval routes:

- `GET /approvals`
- `POST /approvals/:approvalId`

Task routes:

- `GET /tasks`
- `POST /tasks`
- `GET /tasks/:taskId`
- `DELETE /tasks/:taskId`
- `GET /tasks/:taskId/events`
- `GET /tasks/:taskId/checkpoints`
- `GET /tasks/:taskId/state`
- `POST /tasks/:taskId/resume`
- `POST /tasks/:taskId/cancel`

Autonomous routes:

- `GET /autonomous-runs`
- `POST /autonomous-runs`
- `GET /autonomous-runs/:runId`
- `POST /autonomous-runs/:runId/resume`
- `POST /autonomous-runs/:runId/cancel`
- `POST /autonomous-runs/:runId/steps/:stepId/complete`

Workflow routes:

- `POST /workflows/run`
- `GET /workflows/:workflowRunId`
- `GET /workflows/:workflowRunId/checkpoints`
- `POST /workflows/:workflowRunId/resume`
- `POST /workflows/:workflowRunId/human-response`
- `POST /workflows/:workflowRunId/webhook`

Routes whose runtime is not configured return `404` with an explicit error
payload.

## Runner Sources

`AgentServerOptions` can provide a runner in three ways:

- `runner`: fixed runner pointer.
- `create_runner`: factory called with the request body.
- `config` or `config_path`: native config-backed app resolution with cached
  per-agent runners and workflows.

`api_keys[*].agents` can restrict which configured agents a key may call.
Request bodies can include `agent` to select an authorized config agent.

Chat and stream request bodies require `input`. Optional fields include:

- `sessionId`
- `agent`
- `modelSettings`
- `context`
- `governance`

`modelSettings` is validated before execution. Server execution context includes
request id, trace context, route information, session id, and agent id.

`POST /stream` returns an event stream with a replay event and a final `done`
event. The current native stream route executes the runner synchronously and
materializes the final payload before sending the SSE response.

## Access Control

The server is **fail-closed**: constructing an `AgentServerApp` with neither
`auth` nor `api_keys` configured throws a `ConfigurationError`. To run a
deliberately public server, opt in explicitly:

```cpp
agent::AgentServerOptions options;
options.allow_unauthenticated = true;  // no authentication, by explicit choice
```

Bearer and API-key secrets are compared in constant time.

Bearer auth:

```cpp
agent::AgentServerOptions options;
options.auth = agent::AgentServerAuthConfig{
    .bearer_tokens = {"server-token"},
    .exempt_paths = {"/health"},
};
```

API keys:

```cpp
options.api_keys = {
    agent::AgentServerApiKey{
        .id = "team-a",
        .token = "key-token",
        .agents = {"assistant"},
        .quota = agent::AgentServerApiKeyQuotaConfig{
            .max_requests = 100,
            .window_ms = 60000,
        },
    },
};
```

The server accepts either a configured bearer token or a configured API-key
token from the auth header. API-key quota headers are returned as
`x-quota-limit`, `x-quota-remaining`, `x-quota-reset`, and `x-api-key-id`.

Global rate limiting is configured with `AgentServerRateLimitConfig`. It can key
by authorization token or by `x-forwarded-for`, and returns `x-ratelimit-*`
headers.

CORS is controlled by `ServerCorsConfig`; preflight requests use the same route
app and return `204` when allowed.

## Sessions

`AgentServerSessionPolicy` controls generated session ids and deletion:

- `mode`
- `id_prefix`
- `allow_delete`

`session_store` can be supplied directly. Config-backed server apps can resolve
per-agent session stores from the native config. Session delete returns `403`
when policy disables deletion.

## Approvals

Approval records store permission requests, proposed decisions, final
decisions, status, timestamps, and metadata. Available stores:

- `InMemoryApprovalStore`
- `FileApprovalStore`
- `PgApprovalStore` through an injected query client

`ManualApprovalQueue` wraps a store and exposes a `PermissionApprovalHandler`.
Server approval routes list and resolve records. Resolving an approval appends
an `approval.resolved` audit record.

## Tasks, Autonomous, And Workflows

Task routes require `AgentServerTasksConfig` with a store and queue. A worker is
optional and enables active task cancellation. Task creation supports
idempotency via header and/or body, validates model settings, stores server
metadata, enqueues the task, and returns `202` for new tasks or `200` for an
idempotent replay.

Autonomous routes require `AgentServerAutonomousConfig` with a store and
manager. Creation validates a non-empty goal, supports `autoStart`, and enforces
API-key tenant scoping when configured.

Workflow routes can use a directly supplied workflow runtime or a config-backed
workflow runtime. They support run, inspect, checkpoints, resume, human
response, and webhook payload submission.

## Governance

`AgentServerGovernanceConfig` supports:

- PII redaction.
- Citation policies.
- Output JSON Schema validation.

Requests can override supported governance fields through a `governance` object.
Chat responses include a governance summary with redaction, schema validation,
citation count, and citation append status.

## Metrics, Tracing, Audit, And Replay

`AgentServerMetricsCollector` tracks totals by route, agent, and API key. It can
also estimate cost from provider/model usage with configured pricing.

`AgentServerTracingConfig` controls request and trace id headers. When enabled,
responses include normalized trace headers and payload trace data for supported
routes.

`audit_log_path` writes JSONL audit records for request lifecycle, rejections,
session clears, approvals, tasks, autonomous runs, and workflows.

`replay_dir` enables replay materialization for chat/stream responses through
the Replay API directory shape.

## Zero-Dependency Boundary

The server module includes route matching, JSON/SSE helpers, auth, CORS, rate
limit and quota windows, app-level routing, governance, metrics, audit logging,
and approval stores. It does **not** ship any HTTP listener, TLS stack, web
framework, database driver, provider SDK, browser runtime, queue system, or
JavaScript runtime. The host language brings the HTTP transport and feeds
requests through `AgentServerApp::handle`.
