# Runtime Input API

The native runtime accepts the same practical input shapes as the NodeJS
runner while preserving existing C++ convenience overloads.

## Entry Points

Include the public umbrella header:

```cpp
#include "agent/agent.hpp"
```

Use a string for simple user text:

```cpp
auto result = runner.run("summarize this", "session-1");
auto stream = runner.stream(
    "summarize this",
    [](const agent::AgentRunnerStreamEvent& event) {
      (void)event;
    },
    "session-1");
```

Use content parts for the existing C++ multimodal convenience path:

```cpp
std::vector<agent::MessageContentPart> input{
    agent::text_part("describe this image"),
    agent::image_part(source, "screenshot", "high"),
};

auto result = runner.run(std::move(input), "session-1");
```

Use `agent::AgentMessage` when callers need NodeJS `AgentMessage` /
`MessageInput` parity:

```cpp
auto message = agent::create_message(
    agent::MessageRole::User,
    std::vector<agent::MessageContentPart>{
        agent::text_part("/vision inspect this"),
        agent::image_part(source, "screenshot", "high"),
    },
    agent::Value::object({{"source", "api"}}));

auto stream = runner.stream(
    message,
    [](const agent::AgentRunnerStreamEvent& event) {
      (void)event;
    },
    "session-1");
```

`AgentRunner` is the main public entry and exposes the same string,
content-part, and `AgentMessage` overloads.

## Shape Semantics

- String input is stored as a user message and remains a string in run hooks and
  runner event payloads.
- Content-part input is stored as a user message. This keeps the earlier C++
  convenience behavior, including slash-skill text rewriting for the first text
  part.
- `AgentMessage` input preserves role, message metadata, content-part metadata,
  tool fields, and multimodal parts when added to session memory and sent to the
  model.
- For `AgentMessage` input, slash-skill activation can still add the skill
  preface, but the original message content is not rewritten. This matches the
  NodeJS rule where only raw string input becomes `effectiveInputText`.

Durable runner and loop checkpoints keep both the text projection and the full
message/value input fields so resumes can preserve the original message shape.

## Text Projection

Runtime subsystems that require text, such as planning, memory retrieval, and
knowledge retrieval, use `extract_text_content(message.content)`. Image-only
messages therefore keep their full message shape for the model while producing
an empty text projection for text-only subsystems.

## Knowledge Retrieval

Knowledge retrieval follows the NodeJS query selection rule:

- String input uses the string as the knowledge query.
- Inputs with non-empty text content use the extracted text as the knowledge
  query.
- Inputs with no text and at least one image use the first image as an
  `ImageEmbeddingInput` knowledge query.

For image-only knowledge queries, the runtime carries the image source and
`altText`; `title` and `textHint` are read from the input message metadata.
The query label used in hooks and retrieval events is the joined
`title altText textHint`, or `[image]` when all three are empty.
