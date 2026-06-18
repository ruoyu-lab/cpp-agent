# Messages API

The native messages module defines the shared conversation, multimodal content,
tool-call, and media-source shapes used by models, memory, runtime, tools,
knowledge, evals, replay, and server routes.

## Agent Messages

Create text messages with `create_message`:

```cpp
auto user = agent::create_message(
    agent::MessageRole::User,
    "Summarize the attached report.",
    agent::Value::object({{"source", "api"}}));
```

`AgentMessage` preserves:

- `role`: `System`, `User`, `Assistant`, or `Tool`.
- `content`: ordered `MessageContentPart` values.
- `name`: used by tool messages.
- `tool_call_id`: the tool-call id a tool result answers.
- `tool_calls`: assistant tool-call requests.
- `metadata`: arbitrary structured data.

Tool result messages require a tool name:

```cpp
auto result = agent::create_tool_result_message(
    "call_1",
    "search.docs",
    "Found two matching documents.");
```

## Content Parts

Messages support text, image, file, audio, and video parts:

```cpp
agent::MediaSource source;
source.kind = agent::MediaSourceKind::Path;
source.path = "/workspace/report.pdf";
source.mime_type = "application/pdf";

auto message = agent::create_message(
    agent::MessageRole::User,
    std::vector<agent::MessageContentPart>{
        agent::text_part("Use this file."),
        agent::file_part(source, "Report", "Quarterly figures"),
    });
```

`image_part` stores `alt_text`, optional `detail` (`low`, `high`, or `auto`),
and part metadata. `audio_part` stores `title` and `transcript_hint`.
`video_part` stores `title`, `text_hint`, `transcript_hint`, and optional
clip/frame hints. Invalid image detail strings are normalized to empty when
parsing from `Value`.

`extract_text_content` concatenates text parts only. Image, file, audio, and
video parts remain available to media preprocessing, provider serializers, and
knowledge ingestion.

## Media Sources

`MediaSource` supports four source kinds:

- `Inline`: base64 or text data plus MIME type.
- `Url`: remote URL plus optional MIME type and filename.
- `Path`: local path plus optional MIME type and filename.
- `Artifact`: artifact key plus optional MIME type and filename.

`DefaultMediaResolver` can resolve inline data, data URLs, file URLs, local
paths, and artifacts. The richer OCR/rasterizer preprocessing flow is described
in the Media API documentation.

## Value Serialization

Use `agent_message_to_value` for the Node-style serialized shape:

```cpp
auto encoded = agent::agent_message_to_value(user);
auto decoded = agent::agent_message_from_value(encoded);
```

The serialized form uses:

- `role`
- `content`
- `name`
- `toolCallId`
- `toolCalls`
- `metadata`

Content parts use `type`, `text`, `source`, `altText`, `detail`, `title`,
`textHint`, `transcriptHint`, `startTimeSeconds`, `endTimeSeconds`,
`frameRateHint`, and `metadata`. Media sources use `kind`, `data`, `url`,
`path`, `key`, `mimeType`, and `filename`.

## Normalization

`agent_message_from_value` accepts the NodeJS-compatible input variants used
throughout runtime and evals:

- String `content` becomes one text part.
- Array `content` becomes ordered content parts.
- Object `content` becomes one content part.
- Missing or non-object metadata becomes an empty object.
- Missing tool-call ids become `tool_call_1`, `tool_call_2`, and so on.
- String tool-call arguments are parsed as JSON when possible.
- Non-object parsed tool arguments are wrapped as `{ "value": ... }`.
- Non-JSON string tool arguments are wrapped as `{ "raw": ... }`.

Unsupported roles, content part types, media source kinds, or malformed media
sources fail with `ConfigurationError` instead of silently producing an empty
message.

## Assistant Responses

`assistant_message_from_output` converts a `AgentOutput` into an assistant
message for memory and tool-loop history. It preserves visible content, tool
calls, provider/model ids, finish reason, and reasoning metadata when present.

## Zero-Dependency Boundary

The messages module contains only data structures, serialization helpers,
normalization, basic media resolution, MIME inference, and text decoding. It
does not link OCR, browser, provider SDK, database, or network dependencies.
