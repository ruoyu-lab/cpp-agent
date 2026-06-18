# Memory API

The native memory module mirrors the NodeJS session and vector memory surfaces
while staying synchronous and dependency-free.

## Storage Role

Memory storage is intentionally dual-mode:

- In small local deployments, `SessionMemory`, `FileSessionStore`, and
  `FileVectorStore` may be the canonical application store.
- In host-owned or larger deployments, session memory is `runtime-owned` state
  for continuity, resume, and compaction. Vector memory, knowledge stores,
  text indexes, vector indexes, and rerank caches are `derived` substrate that
  can be rebuilt from host-owned business data or runtime events.

The framework does not define business entities, tenant records, reporting
tables, or the long-term chat archive format for a production host. Those stay
owned by the embedding application and are exposed to the runner through tools,
repositories, or injected adapters.

## Header Boundary

`agent/memory.hpp` is limited to session memory, session stores, long-term
memory records, vector stores, and writeback hooks. The embeddable runner only
needs `agent/knowledge_runtime.hpp` and a `KnowledgeContextProvider`; full
knowledge-base assembly is a separate opt-in concern:

- include `agent/knowledge_core.hpp` or `agent/knowledge.hpp` for
  `KnowledgeSourceLoader`, `KnowledgeStore`, indexes, rerankers,
  `KnowledgeBase`, and `KnowledgeBaseManager`;
- include `agent/knowledge_io.hpp` and link `agent_runtime_io` for
  repository, web, website, and sitemap loaders.

This keeps embeddable hosts from pulling host I/O or knowledge-base assembly
details when they only need chat history or long-term memory.

## Session Memory

`SessionMemory` stores chat history, compacts old messages into a summary, and
can expose a summary-prefixed message list:

```cpp
agent::SessionMemoryOptions options;
options.storage.session_id = "support";
options.compaction.compaction_budget.max_messages = 24;
agent::SessionMemory session(options);
session.add(agent::create_message(agent::MessageRole::User, "hello"));
auto messages = session.get_messages();
```

NodeJS-style `MessageInput` objects can be added through `Value` and are
normalized by the core message parser. `get_messages()` also accepts an options
object:

```cpp
session.add(agent::Value::object({
    {"role", "user"},
    {"content", "hello from Value"},
})).add(agent::create_message(agent::MessageRole::Assistant, "hi"));

agent::SessionMemoryGetMessagesOptions get;
get.include_summary = false;
auto live_messages = session.get_messages(get);
```

### CompactionBudget

`CompactionBudget` holds composable caps that govern post-compaction state:

```cpp
struct CompactionBudget {
  std::optional<std::size_t> max_messages;  // post-compaction message count
  std::optional<std::size_t> max_tokens;    // post-compaction estimated tokens
};
```

Truncation drops the oldest messages until **every** cap that is set evaluates
true. The NodeJS mirror exposes the same struct as `compactionBudget` /
`maxMessages` / `maxTokens` on `SessionMemoryOptions`.

The three idiomatic shapes:

```cpp
// Count-only: keep the most recent N messages, no token awareness.
options.compaction.compaction_budget = {.max_messages = 24};

// Token-only: drop oldest until estimated tokens fit the cap.
options.compaction.compaction_budget = {.max_tokens = 100000};

// Both: cap by N messages, then drop more if tokens still exceed cap.
options.compaction.compaction_budget = {.max_messages = 24, .max_tokens = 100000};
```

#### Unit alignment (and the bug this prevents)

Earlier, `SessionMemoryOptions::max_messages` was a single scalar but
`should_auto_compact()` evaluated `tokens >= auto_compact_at * token_budget`.
The trigger compared *tokens* while the truncator compared *message count* —
mixing units. If a host accidentally set `max_messages` to a token-shaped value
the runtime would call `compact()` every iteration but the count comparison
held, so nothing was dropped: silent context overflow.

`CompactionBudget` cuts this by making each cap unit-explicit. Setting a
`token_budget` and `auto_compact_at` without setting `compaction_budget` is now
an error — explicit `compact()` (and the auto-compaction path that calls it)
throw `ConfigurationError` containing `"no CompactionBudget"`. Sessions that
never opt in to compaction (no `token_budget`, no `compaction_budget`) keep
appending freely; only explicit compaction or the auto-compact runtime path
surfaces the misconfiguration.

Custom summarizers receive the session id, previous summary, and archived
messages when compaction moves old turns out of the live window:

```cpp
agent::SessionMemoryOptions options;
options.storage.session_id = "support";
options.compaction.compaction_budget.max_messages = 24;
options.compaction.summarizer = [](const agent::SessionMemorySummaryInput& input) {
  return "Archived " + std::to_string(input.archived_messages.size()) +
         " messages for " + input.session_id;
};

agent::SessionMemory session(options);
```

#### LLM summarizer

The native module also exposes a reusable LLM-backed summarizer factory. It is
still just a `SessionMemorySummarizer`: `plan_compaction` remains deterministic
and decides which messages leave the live window; the model only merges
`previous_summary` and `archived_messages` into a better summary.

```cpp
auto model = std::make_shared<agent::OpenAICompatibleChatModelAdapter>(
    "openai",
    "gpt-5-mini",
    transport,
    "/v1/chat/completions",
    "https://api.openai.com",
    api_key);

agent::SessionMemoryOptions options;
options.compaction.compaction_budget.max_tokens = 80000;
options.compaction.token_budget = 128000;
options.compaction.summarizer_mode = agent::SummarizerMode::Background;
options.compaction.summarizer = agent::create_llm_session_summarizer({
    .model = model,
    .max_input_chars = 24000,
    .max_summary_chars = 8000,
    .max_summary_tokens = 2000,
});
```

Defaults target agent-state summaries: current goal, durable facts, user
preferences, decisions, open tasks, constraints, tool/environment state, and
recent progress. Advanced hosts can replace `format_message`, `build_messages`,
and `parse_response` on `LLMSessionSummarizerOptions` for redaction, auditing,
provider-specific schemas, or strict structured output.

`on_error` defaults to `LLMSessionSummarizerErrorMode::Throw`, which lets
`SessionMemory` preserve pending overflow and emit a `Failed` compaction event.
Set `on_error = LLMSessionSummarizerErrorMode::Fallback` to use the deterministic
summary fallback when the model is unavailable. The factory is explicit opt-in;
the default `SessionMemory` never sends history to an external model.

### Auto-compaction by token budget

Set `token_budget` (and optionally tune `auto_compact_at`, default `0.8`) to
have the runtime call `compact()` automatically just before the next model
call once the estimated token count crosses
`auto_compact_at * token_budget`. The framework uses a character-based
estimate (`chars / 4`) by default; pass a `token_counter` if you have a
provider-accurate tokenizer.

```cpp
agent::SessionMemoryOptions options;
options.compaction.token_budget = 8000;
options.compaction.auto_compact_at = 0.8;
options.compaction.token_counter = [](const std::vector<agent::AgentMessage>& msgs) {
  return my_tokenizer_count(msgs);
};
```

The runner emits a `session.auto_compact` event on its `EventBus` whenever
auto-compaction fires, carrying `tokensBefore`, `tokensAfter`, and
`tokenBudget` for observability. When `token_budget == 0` (the default)
auto-compaction is disabled and behaviour is unchanged.

### Manual compaction

Call `SessionMemory::compact()` whenever the host wants to reduce the live
window immediately. It uses the same three-phase pipeline as the automatic
runtime trigger and requires at least one `compaction_budget` cap:

```cpp
session.compact();
```

When working through a runner, `AgentRunner::compact_session("session-1")`
fetches the session from the configured store, calls `compact()`, returns the
post-compaction snapshot, and emits a `session.compact` event on the runner
`EventBus`.

### Compaction pipeline (`plan_compaction` + three-phase contract)

`compact()` is internally a three-phase pipeline:

1. **Plan + erase under lock** — `plan_compaction(messages, summary,
   summary_label, budget, token_counter)` is a pure function that returns a
   `CompactionPlan{ drop_count, overflow }`. It never acquires any lock and
   never calls the summarizer. `SessionMemory::compact()` runs it under
   `mutex_`, erases the planned prefix, and snapshots the previous summary
   and the dropped messages.
2. **Run summarizer outside the lock** — the summarizer can be a multi-
   second LLM call. It runs OUTSIDE `mutex_`, so it can re-enter the
   `SessionMemory` for reads (e.g. compute `estimated_token_count()` on
   itself) without deadlock, and so concurrent readers are not blocked
   for the duration of the LLM round-trip.
3. **Write summary back under a short lock + notify** — the new summary
   is stored, a snapshot is captured, and `on_change` runs outside the
   lock.

`plan_compaction` is exposed publicly so hosts can predict what a
compaction would do without actually running one. The fix in this
refactor: the planner uses the SAME ruler the trigger uses
(`token_counter` if set, otherwise char/4) AND folds the existing summary
into the budget snapshot, so `CompactionBudget::max_tokens` and
`should_auto_compact()` agree on what "tokens" means. Previously the two
sides could disagree on CJK-heavy content and produce silent compaction
no-ops.

### Per-append compaction opt-out (`compact_on_append`)

By default `add()` / `add_many()` do not compact on the append path. Hosts that
want eager per-append compaction can opt in; hosts that run an external
compaction driver (for example, runtime-side `maybe_auto_compact` between
iterations) should keep the default so append latency stays bounded:

```cpp
agent::SessionMemoryOptions options;
options.compaction.compaction_budget.max_tokens = 100000;
options.compaction.compact_on_append = true;  // add()/add_many() may compact immediately
```

NodeJS mirror name: `compactOnAppend` on `SessionMemoryOptions`.

### Concurrency contract

- `SessionMemory::mutex_` is held only across pure, fast operations
  (append, planner, erase, summary write-back, snapshot construction).
- The summarizer callback runs OUTSIDE `mutex_`. It may re-enter
  `SessionMemory` for reads without deadlocking.
- `on_change` runs OUTSIDE `mutex_`.
- `plan_compaction` acquires no locks and invokes no callbacks; it is
  safe to call from any thread, with or without `mutex_` held.
- `add()` / `add_many()` notify once with the post-append (pre-summary)
  snapshot and a second time with the post-summary snapshot when
  compaction actually fires. `compact()` notifies once with the final
  post-summary snapshot.

Restore accepts full snapshots or NodeJS-style partial snapshots. Missing
summary/messages are reset to empty values and still trigger the configured
change handler:

```cpp
agent::SessionMemoryRestoreInput restore;
restore.summary = "Resolved customer context";
session.restore(restore);
```

`InMemorySessionStore` keeps sessions in process memory. `FileSessionStore`
persists each session as JSON. File-backed sessions are written immediately
after `add`, `add_many`, `restore`, or `clear`, and can still be flushed
explicitly:

```cpp
agent::SessionMemoryOptions options;
options.storage.summary_label = "Support summary";
options.compaction.compaction_budget.max_messages = 32;

agent::FileSessionStore store("sessions", options);
auto session = store.get("srv:one/path");
session->add(agent::create_message(agent::MessageRole::User, "persist me"));
```

NodeJS-style store config objects are also available:

```cpp
agent::InMemorySessionStore memory_store(agent::InMemorySessionStoreConfig{
    .session_options = options,
});

agent::FileSessionStore file_store(agent::FileSessionStoreConfig{
    .base_dir = "sessions",
    .file_extension = "json",
    .session_options = options,
});
```

Session ids are encoded using NodeJS `encodeURIComponent`-style filenames, so
ids such as `srv:one/path` are stored safely as one file and are decoded by
`list_session_ids()`.

Both session stores accept `SessionMemoryOptions`; stores override the session
id with the id passed to `get()`, while file-backed sessions load persisted
summary/messages before falling back to configured initial values.

## Long-Term Memory

Create a memory store with an embedder and a vector store:

```cpp
agent::LongTermMemoryConfig config;
config.embedder = std::make_shared<agent::HashEmbeddingAdapter>();
config.store = std::make_shared<agent::InMemoryVectorStore>();
config.namespace_id = "default";
config.top_k = 4;
config.min_score = 0.2;

agent::LongTermMemory memory(config);
```

Store one memory with NodeJS-style id, metadata, and namespace fields:

```cpp
auto record = memory.remember(agent::RememberMemoryInput{
    .id = "manual-1",
    .content = "Native C++ memory entry",
    .metadata = agent::Value::object({{"source", "manual"}}),
    .namespace_id = "docs",
});
```

Store many memories:

```cpp
auto records = memory.remember_many({
    agent::RememberMemoryInput{
        .id = "alpha",
        .content = "Alpha memory",
        .metadata = agent::Value::object({{"kind", "note"}}),
        .namespace_id = "docs",
    },
    agent::RememberMemoryInput{
        .id = "beta",
        .content = "Beta memory",
        .namespace_id = "docs",
    },
});
```

Conversation writeback returns the vector record that was stored:

```cpp
auto turn = memory.remember_conversation_turn(agent::RememberConversationTurnInput{
    .session_id = "support-1",
    .input = "How do I persist memory?",
    .output = "Use a vector store.",
    .namespace_id = "docs",
    .metadata = agent::Value::object({{"source", "chat"}}),
    .plan = agent::RememberConversationPlan{.steps = {
        agent::RememberConversationPlanStep{.title = "Read request"},
        agent::RememberConversationPlanStep{.title = "Answer"},
    }},
});
```

For existing C++ callers, `.plan_steps = {"Read request", "Answer"}` remains
supported and is used when `plan` is not provided.

Search uses the configured defaults unless call options are provided:

```cpp
agent::SearchMemoryOptions search;
search.top_k = 2;
search.min_score = 0.0;
search.namespace_id = "docs";

auto hits = memory.search("Alpha", search);
```

Build a context message in one call:

```cpp
auto context = memory.build_context_message("Alpha", search);
if (context.message) {
  // Use context.hits and *context.message.
}
```

Long-term memory return types can be serialized with NodeJS-style field names:

```cpp
auto record_json = agent::vector_memory_record_to_value(record);
auto hit_json = agent::retrieved_memory_to_value(hits.front());
auto context_json = agent::long_term_memory_context_result_to_value(context);
```

Runtime hooks, runner durable checkpoints, and server run summaries use the same
hit serialization shape, including `namespace` for the memory namespace.

## Vector Stores

`InMemoryVectorStore` and `FileVectorStore` support the NodeJS vector-store
operations:

- `upsert`
- `query`
- `delete_ids` (`delete` in NodeJS; renamed because `delete` is a C++ keyword)
- `erase`
- `clear`
- `count`

Both stores also support config-object construction:

```cpp
agent::InMemoryVectorStore memory_vectors(agent::InMemoryVectorStoreConfig{
    .namespace_id = "docs",
});

agent::FileVectorStore file_vectors(agent::FileVectorStoreConfig{
    .file_path = "memory/vectors.json",
    .namespace_id = "docs",
});
```

Upserts without `namespace_id` use the store's configured namespace, matching
NodeJS `namespace` fallback behavior.

`FileVectorStore` persists normalized records as JSON and reloads lazily on
first access.
