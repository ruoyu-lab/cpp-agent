bool CompactionBudget::empty() const noexcept {
  return !max_messages.has_value() && !max_tokens.has_value();
}

std::string to_string(SessionCompactionEvent::Kind kind) {
  switch (kind) {
    case SessionCompactionEvent::Kind::Started:
      return "Started";
    case SessionCompactionEvent::Kind::Phase1Completed:
      return "Phase1Completed";
    case SessionCompactionEvent::Kind::RefinementStarted:
      return "RefinementStarted";
    case SessionCompactionEvent::Kind::RefinementCompleted:
      return "RefinementCompleted";
    case SessionCompactionEvent::Kind::Failed:
      return "Failed";
  }
  return "Unknown";
}

std::string to_string(LLMSessionSummarizerErrorMode mode) {
  switch (mode) {
    case LLMSessionSummarizerErrorMode::Throw:
      return "throw";
    case LLMSessionSummarizerErrorMode::Fallback:
      return "fallback";
  }
  return "throw";
}

std::string to_string(LLMSessionSummaryStyle style) {
  switch (style) {
    case LLMSessionSummaryStyle::AgentState:
      return "agent-state";
    case LLMSessionSummaryStyle::Conversation:
      return "conversation";
  }
  return "agent-state";
}

namespace {

constexpr std::size_t kDefaultLlmSummaryMaxInputChars = 24000;
constexpr std::size_t kDefaultLlmSummaryMaxArchivedMessages = 200;
constexpr std::size_t kDefaultLlmSummaryMaxSummaryChars = 8000;
constexpr int kDefaultLlmSummaryMaxOutputTokens = 2000;

std::string default_llm_session_summary_system_prompt() {
  return "You compact runtime session memory for an AI agent.\n"
         "Use only the previous summary and archived messages provided by the host.\n"
         "Preserve durable state that helps the next model call continue the task.\n"
         "Do not invent facts, tool results, preferences, file paths, or decisions.\n"
         "Prefer concise structured notes over a transcript rewrite.\n"
         "Return only the updated session summary.";
}

std::string default_llm_session_summary_instructions(LLMSessionSummaryStyle style) {
  if (style == LLMSessionSummaryStyle::Conversation) {
    return "Write a compact conversation summary.\n"
           "Preserve the user's intent, important assistant answers, decisions, and follow-up items.\n"
           "Merge with the previous summary; remove stale or superseded details.";
  }
  return "Write the updated summary with these sections when relevant:\n"
         "- Current goal\n"
         "- Durable facts\n"
         "- User preferences\n"
         "- Decisions made\n"
         "- Open tasks\n"
         "- Constraints and risks\n"
         "- Tool and environment state\n"
         "- Recent progress\n"
         "Keep exact identifiers, file paths, API names, error messages, and unresolved questions.\n"
         "Merge with the previous summary; remove stale or superseded details.";
}

std::string tail_by_chars(std::string value, std::size_t max_chars) {
  if (max_chars > 0 && value.size() > max_chars) {
    return value.substr(value.size() - max_chars);
  }
  return value;
}

struct BoundedArchivedTranscript {
  std::string transcript;
  std::size_t omitted_messages = 0;
  std::size_t omitted_characters = 0;
};

BoundedArchivedTranscript build_bounded_archived_transcript(
    const std::vector<AgentMessage>& archived_messages,
    std::size_t max_archived_messages,
    std::size_t max_input_chars,
    const LLMSessionMessageFormatter& formatter) {
  BoundedArchivedTranscript result;
  const std::size_t visible_count =
      std::min(max_archived_messages, archived_messages.size());
  result.omitted_messages = archived_messages.size() - visible_count;
  const std::size_t start = result.omitted_messages;

  std::ostringstream transcript;
  for (std::size_t index = start; index < archived_messages.size(); ++index) {
    if (index > start) {
      transcript << "\n\n";
    }
    transcript << formatter(archived_messages[index], index + 1);
  }
  std::string full = transcript.str();
  if (max_input_chars > 0 && full.size() > max_input_chars) {
    result.omitted_characters = full.size() - max_input_chars;
    result.transcript = full.substr(full.size() - max_input_chars);
  } else {
    result.transcript = std::move(full);
  }
  return result;
}

}  // namespace

std::string format_session_message_for_llm(const AgentMessage& message, std::size_t index) {
  std::ostringstream out;
  out << "#" << index << " role=" << message_role_label(message.role);
  if (!message.name.empty()) {
    out << " name=" << message.name;
  }
  if (!message.tool_call_id.empty()) {
    out << " toolCallId=" << message.tool_call_id;
  }
  out << "\n";

  const std::string text = trim_copy(extract_text_content(message.content));
  out << (text.empty() ? "(no text content)" : text);

  if (!message.tool_calls.empty()) {
    Value::Array calls;
    for (const auto& call : message.tool_calls) {
      calls.push_back(Value::object({
          {"id", call.id},
          {"name", call.name},
          {"arguments", call.arguments},
      }));
    }
    out << "\ntoolCalls=" << Value(std::move(calls)).stringify(2);
  }
  return out.str();
}

std::vector<AgentMessage> build_llm_session_summary_messages(
    const SessionMemorySummaryInput&,
    const LLMSessionSummarizerPromptContext& context) {
  std::string system = context.system_prompt;
  if (!context.instructions.empty()) {
    if (!system.empty()) {
      system += "\n\n";
    }
    system += context.instructions;
  }

  std::ostringstream user;
  user << "Previous summary:\n"
       << (context.previous_summary.empty() ? "(none)" : context.previous_summary)
       << "\n\nArchived messages omitted by count: " << context.omitted_archived_messages
       << "\n\nArchived transcript characters omitted from head: "
       << context.omitted_archived_characters
       << "\n\nArchived messages:\n"
       << (context.archived_transcript.empty() ? "(none)" : context.archived_transcript);

  return {
      create_message(MessageRole::System, std::move(system)),
      create_message(MessageRole::User, user.str()),
  };
}

std::string parse_llm_session_summary_response(const AgentOutput& response) {
  std::string text = trim_copy(response.text.empty()
                                   ? extract_text_content(response.content)
                                   : response.text);
  if (text.empty()) {
    throw ConfigurationError("LLM session summarizer returned an empty summary.");
  }
  return text;
}

std::string default_llm_session_summarizer_fallback(
    const SessionMemorySummaryInput& input,
    std::size_t max_summary_chars) {
  std::string output = trim_copy(input.previous_summary);
  const std::string archived = trim_copy(summarize_messages(input.archived_messages));
  if (!archived.empty()) {
    if (!output.empty()) {
      output += "\n";
    }
    output += archived;
  }
  return tail_by_chars(std::move(output), max_summary_chars);
}

SessionMemorySummarizer create_llm_session_summarizer(
    LLMSessionSummarizerOptions options) {
  if (!options.model) {
    throw ConfigurationError("create_llm_session_summarizer requires a model adapter.");
  }
  if (options.max_input_chars == 0) {
    options.max_input_chars = kDefaultLlmSummaryMaxInputChars;
  }
  if (options.max_archived_messages == 0) {
    options.max_archived_messages = kDefaultLlmSummaryMaxArchivedMessages;
  }
  if (options.max_summary_chars == 0) {
    options.max_summary_chars = kDefaultLlmSummaryMaxSummaryChars;
  }
  if (options.max_summary_tokens <= 0) {
    options.max_summary_tokens = kDefaultLlmSummaryMaxOutputTokens;
  }
  if (!options.settings.max_output_tokens) {
    options.settings.max_output_tokens = options.max_summary_tokens;
  }
  if (options.system_prompt.empty()) {
    options.system_prompt = default_llm_session_summary_system_prompt();
  }
  if (options.instructions.empty()) {
    options.instructions = default_llm_session_summary_instructions(options.style);
  }
  if (!options.format_message) {
    options.format_message = format_session_message_for_llm;
  }
  if (!options.build_messages) {
    options.build_messages = build_llm_session_summary_messages;
  }
  if (!options.parse_response) {
    options.parse_response = parse_llm_session_summary_response;
  }

  return [options = std::move(options)](const SessionMemorySummaryInput& input) mutable {
    const auto bounded = build_bounded_archived_transcript(
        input.archived_messages,
        options.max_archived_messages,
        options.max_input_chars,
        options.format_message);

    LLMSessionSummarizerPromptContext context;
    context.style = options.style;
    context.system_prompt = options.system_prompt;
    context.instructions = options.instructions;
    context.previous_summary =
        options.include_previous_summary ? input.previous_summary : std::string{};
    context.archived_transcript = bounded.transcript;
    context.omitted_archived_messages = bounded.omitted_messages;
    context.omitted_archived_characters = bounded.omitted_characters;

    try {
      GenerateParams params;
      params.messages = options.build_messages(input, context);
      params.settings = options.settings;
      params.cancellation = options.cancellation;
      auto response = options.model->generate(params);
      return tail_by_chars(options.parse_response(response), options.max_summary_chars);
    } catch (...) {
      if (options.on_error != LLMSessionSummarizerErrorMode::Fallback) {
        throw;
      }
      if (options.fallback_summarizer) {
        return tail_by_chars(options.fallback_summarizer(input), options.max_summary_chars);
      }
      return default_llm_session_summarizer_fallback(input, options.max_summary_chars);
    }
  };
}

SessionMemory::SessionMemory(SessionMemoryOptions options)
    : session_id_(std::move(options.storage.session_id)),
      compaction_budget_(std::move(options.compaction.compaction_budget)),
      summary_label_(std::move(options.storage.summary_label)),
      summary_(options.storage.summary),  // copy, baseline takes a copy below
      messages_(std::move(options.storage.messages)),
      // Two-state baseline: when restoring from a legacy snapshot the host
      // supplies only `summary` — treat it as the polished baseline and leave
      // the pending tail empty.
      polished_baseline_(std::move(options.storage.summary)),
      on_change_(std::move(options.on_change)),
      on_compaction_(std::move(options.on_compaction)),
      summarizer_(std::move(options.compaction.summarizer)),
      auto_compact_at_(options.compaction.auto_compact_at),
      token_budget_(options.compaction.token_budget),
      token_counter_(std::move(options.compaction.token_counter)),
      compact_on_append_(options.compaction.compact_on_append),
      summary_max_chars_(options.compaction.summary_max_chars),
      summarizer_mode_(options.compaction.summarizer_mode),
      background_executor_(std::move(options.compaction.background_executor)),
      truncate_oversized_message_(options.compaction.truncate_oversized_message),
      oversized_tail_chars_(options.compaction.oversized_tail_chars) {}

SessionMemory::~SessionMemory() {
  // Wait for any in-flight background refinement so its callback does not
  // outlive this object. await_refinements is safe to call on a quiescent
  // SessionMemory and is a no-op when nothing is pending.
  try {
    await_refinements();
  } catch (...) {
    // Destructors must not throw.
  }
}

Value session_memory_snapshot_to_value(const SessionMemorySnapshot& snapshot) {
  Value::Array messages;
  for (const auto& message : snapshot.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array overflow;
  for (const auto& message : snapshot.pending_refinement_overflow) {
    overflow.push_back(agent_message_to_value(message));
  }
  return Value::object({{"sessionId", snapshot.session_id},
                        {"summary", snapshot.summary},
                        {"messages", Value(messages)},
                        {"polishedBaseline", snapshot.polished_baseline},
                        {"pendingConcatTail", snapshot.pending_concat_tail},
                        {"pendingRefinementOverflow", Value(overflow)}});
}

SessionMemorySnapshot session_memory_snapshot_from_value(const Value& value) {
  SessionMemorySnapshot snapshot;
  if (!value.is_object()) {
    return snapshot;
  }
  snapshot.session_id = value.at("sessionId").as_string(value.at("session_id").as_string());
  snapshot.summary = value.at("summary").as_string();
  if (value.at("messages").is_array()) {
    for (const auto& item : value.at("messages").as_array()) {
      snapshot.messages.push_back(agent_message_from_value(item));
    }
  }
  snapshot.polished_baseline =
      value.at("polishedBaseline").as_string(value.at("polished_baseline").as_string());
  snapshot.pending_concat_tail =
      value.at("pendingConcatTail").as_string(value.at("pending_concat_tail").as_string());
  const auto& overflow = value.contains("pendingRefinementOverflow")
                             ? value.at("pendingRefinementOverflow")
                             : value.at("pending_refinement_overflow");
  if (overflow.is_array()) {
    for (const auto& item : overflow.as_array()) {
      snapshot.pending_refinement_overflow.push_back(agent_message_from_value(item));
    }
  }
  return snapshot;
}

namespace {

// Char-based token estimator shared by plan_compaction's fallback and
// SessionMemory::estimated_token_count_unlocked: one source of truth so
// CompactionBudget::max_tokens and should_auto_compact()'s threshold use
// the same unit when no SessionMemoryTokenCounter is configured.
std::size_t estimated_messages_tokens(std::span<const AgentMessage> messages) {
  std::size_t total_chars = 0;
  for (const auto& message : messages) {
    total_chars += extract_text_content(message.content).size();
  }
  return (total_chars + 3) / 4;  // rough char-based estimate
}

// Build the snapshot the trigger and the planner BOTH measure against:
// the summary projected as a virtual system message, followed by the
// configured suffix of messages. Single source of truth for "what does
// 'budget' mean" — fixes the counter-split and summary-asymmetry bugs.
std::vector<AgentMessage> build_budget_snapshot(
    std::span<const AgentMessage> messages,
    std::string_view summary,
    std::string_view summary_label) {
  std::vector<AgentMessage> snapshot;
  snapshot.reserve(messages.size() + 1);
  if (!summary.empty()) {
    snapshot.push_back(create_message(MessageRole::System,
                                      std::string(summary_label) + ":\n" + std::string(summary),
                                      Value::object({{"source", "session-memory"}})));
  }
  snapshot.insert(snapshot.end(), messages.begin(), messages.end());
  return snapshot;
}

// Compute remaining-token cost for a drop count, using the same ruler the
// trigger uses: the configured token_counter if set, otherwise the shared
// char/4 estimator. Both branches include the summary in the budget so
// that truncation and trigger agree on the unit.
std::size_t measure_remaining_tokens(std::span<const AgentMessage> messages,
                                     std::size_t drop,
                                     std::string_view summary,
                                     std::string_view summary_label,
                                     const SessionMemoryTokenCounter& token_counter) {
  const std::span<const AgentMessage> remaining = messages.subspan(drop);
  if (token_counter) {
    auto snapshot = build_budget_snapshot(remaining, summary, summary_label);
    return token_counter(snapshot);
  }
  const std::size_t summary_tokens = summary.empty() ? 0 : (summary.size() + 3) / 4;
  return summary_tokens + estimated_messages_tokens(remaining);
}

std::string normalize_session_file_extension(std::string extension) {
  if (extension.empty()) {
    return ".json";
  }
  if (extension.front() == '.') {
    return extension;
  }
  return "." + extension;
}

}  // namespace

CompactionPlan plan_compaction(const PlanCompactionInput& input) {
  CompactionPlan plan;
  if (input.budget.empty()) {
    return plan;  // caller decides whether empty budget is an error
  }
  // Compute how many oldest messages to drop so that ALL set caps hold
  // after the drop. Linear loop; messages_ rarely exceeds a few hundred.
  // Pure: never touches summarizer or any lock.
  std::size_t drop = 0;
  while (drop < input.messages.size()) {
    const std::size_t remaining_count = input.messages.size() - drop;
    if (input.budget.max_messages && remaining_count > *input.budget.max_messages) {
      ++drop;
      continue;
    }
    if (input.budget.max_tokens) {
      const std::size_t remaining_tokens =
          measure_remaining_tokens(input.messages, drop, input.summary,
                                   input.summary_label, input.token_counter);
      if (remaining_tokens > *input.budget.max_tokens) {
        // Single-message overflow handling: when only the tail remains and
        // it alone exceeds max_tokens, we cannot drop it without leaving
        // the conversation empty. If the policy opts in, emit a truncated
        // copy of the tail; otherwise stop and let the caller surface a
        // soft over-budget warning.
        if (remaining_count <= 1) {
          if (input.truncate_oversized_message && remaining_count == 1) {
            const AgentMessage& tail =
                *(input.messages.begin() + static_cast<std::ptrdiff_t>(drop));
            AgentMessage truncated = tail;
            // Replace the message's text payload with the trailing slice.
            const std::string original_text = extract_text_content(truncated.content);
            std::string new_text;
            if (input.oversized_tail_chars > 0 &&
                original_text.size() > input.oversized_tail_chars) {
              new_text = original_text.substr(original_text.size() -
                                              input.oversized_tail_chars);
            } else {
              new_text = original_text;
            }
            // Replace all content parts with a single text part carrying the
            // truncated payload; non-text parts (images, etc.) are dropped
            // because they're not text-measured and the cap is a text cap.
            truncated.content.clear();
            truncated.content.push_back(MessageContentPart{
                .type = ContentPartType::Text,
                .text = std::move(new_text),
            });
            plan.truncated_tail_message = std::move(truncated);
          }
          break;
        }
        ++drop;
        continue;
      }
    }
    break;  // all set caps satisfied
  }
  if (drop == 0 && !plan.truncated_tail_message) {
    return plan;
  }
  plan.drop_count = drop;
  if (drop > 0) {
    plan.overflow.assign(input.messages.begin(),
                         input.messages.begin() + static_cast<std::ptrdiff_t>(drop));
  }
  return plan;
}

const std::string& SessionMemory::session_id() const noexcept {
  return session_id_;
}

SessionMemorySnapshot SessionMemory::snapshot_unlocked() const {
  // We project polished/pending state into the snapshot so persistence
  // round-trips can resume an in-flight refinement. Reading the refinement
  // bookkeeping fields here without taking refinement_mutex_ is acceptable
  // because mutex_ is already held by the caller and Phase 2 commits to
  // pending_concat_tail_/polished_baseline_ are themselves serialized through
  // mutex_ in the write-back step (see run_refinement_body).
  return SessionMemorySnapshot{session_id_, summary_, messages_,
                               polished_baseline_, pending_concat_tail_,
                               pending_refinement_overflow_};
}

void SessionMemory::notify_change(const SessionMemorySnapshot& snapshot) const {
  if (on_change_) {
    on_change_(snapshot);
  }
}

void SessionMemory::notify_compaction(const SessionCompactionEvent& event) const {
  if (on_compaction_) {
    on_compaction_(event);
  }
}

namespace {

// Build the cheap concat tail line for a single overflow message, matching
// the deterministic summarize_messages format so post-refinement state and
// pre-refinement projections render identically.
std::string format_concat_line(const AgentMessage& message) {
  std::string line = "[" + message_role_label(message.role);
  if (!message.name.empty()) {
    line += ":" + message.name;
  }
  line += "] " + extract_text_content(message.content) + "\n";
  return line;
}

// Build the new polished baseline outside the mutex. Either runs the user-
// provided summarizer (which may block on an LLM call for seconds and may
// re-enter SessionMemory via reads) or falls back to the deterministic
// summarize_messages helper. Pure with respect to SessionMemory state.
std::string compute_new_polished_baseline(const SessionMemorySummarizer& summarizer,
                                          const std::string& session_id,
                                          std::string previous_baseline,
                                          const std::vector<AgentMessage>& overflow) {
  if (summarizer) {
    return summarizer(
        SessionMemorySummaryInput{session_id, std::move(previous_baseline), overflow});
  }
  std::string updated = std::move(previous_baseline);
  if (!updated.empty()) {
    updated += "\n";
  }
  updated += summarize_messages(overflow);
  return updated;
}

// Apply summary_max_chars cap to a denormalized join, preferring to keep
// the trailing slice (the most recent context).
std::string apply_summary_cap(std::string value, std::size_t cap) {
  if (cap > 0 && value.size() > cap) {
    return value.substr(value.size() - cap);
  }
  return value;
}

}  // namespace

void SessionMemory::recompute_summary_unlocked() {
  std::string joined = polished_baseline_;
  if (!pending_concat_tail_.empty()) {
    if (!joined.empty() && joined.back() != '\n') {
      joined += "\n";
    }
    joined += pending_concat_tail_;
  }
  summary_ = apply_summary_cap(std::move(joined), summary_max_chars_);
}

// ---------------------------------------------------------------------------
// Phase 1 + Phase 2 dispatch + Phase 2 body.
// ---------------------------------------------------------------------------
//
// Phase 1 (run_phase1_locked):
//   - Caller holds compaction_pipeline_mutex_.
//   - Acquire mutex_, compute plan_compaction, erase oldest messages, apply
//     truncated_tail_message when present, append concat lines to
//     pending_concat_tail_, append AgentMessages to pending_refinement_overflow_,
//     recompute denormalized summary_.
//   - Fires on_change once for the Phase 1 mutation.
//
// Phase 2 dispatch (dispatch_refinement):
//   - Synchronous mode: call run_refinement_body() on the current thread.
//   - Background mode: under refinement_mutex_, if no in-flight refinement
//     exists, set refinement_in_flight_ = true and submit run_refinement_body
//     to the executor (or std::thread fallback). Otherwise just set
//     refinement_pending_ = true and return — the in-flight body will pick
//     up the accumulated tail/overflow on its trailing dispatch.
//
// Phase 2 body (run_refinement_body):
//   - Snapshot polished_baseline_ + pending_refinement_overflow_ + session_id
//     under mutex_; the snapshot becomes the summarizer input.
//   - Fire RefinementStarted.
//   - Run summarizer outside any lock. On exception → Failed event, clear
//     refinement_in_flight_ + refinement_pending_, notify_all, return without
//     mutating polished_baseline_ / pending_*.
//   - On success: under mutex_, commit polished_baseline_ = result, clear the
//     consumed prefix of pending_concat_tail_ + pending_refinement_overflow_,
//     recompute summary_, capture snapshot for on_change + RefinementCompleted.
//   - Under refinement_mutex_: clear in_flight; if pending was set during the
//     refinement, set in_flight = true and dispatch trailing refinement.
//     Otherwise notify_all (await_refinements consumers).

SessionMemory& SessionMemory::add(AgentMessage message) {
  bool should_compact = false;
  SessionMemorySnapshot changed;

  // Append phase under mutex_ only. compaction_pipeline_mutex_ NOT held —
  // the only contract on append is that it does not race with itself.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.push_back(std::move(message));
    should_compact = compact_on_append_ && !compaction_budget_.empty();
    changed = snapshot_unlocked();
  }

  notify_change(changed);

  if (!should_compact) {
    return *this;
  }
  // Fall through to the same pipeline used by explicit compact(). This
  // unifies Phase 1 + Phase 2 dispatch semantics across append-driven and
  // explicit paths.
  compact();
  return *this;
}

SessionMemory& SessionMemory::add(const Value& message) {
  return add(agent_message_from_value(message));
}

SessionMemory& SessionMemory::add_many(const std::vector<AgentMessage>& messages) {
  bool should_compact = false;
  SessionMemorySnapshot changed;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_.insert(messages_.end(), messages.begin(), messages.end());
    should_compact = compact_on_append_ && !compaction_budget_.empty();
    changed = snapshot_unlocked();
  }

  notify_change(changed);

  if (!should_compact) {
    return *this;
  }
  compact();
  return *this;
}

SessionMemory& SessionMemory::add_many(const std::vector<Value>& messages) {
  std::vector<AgentMessage> normalized;
  normalized.reserve(messages.size());
  for (const auto& message : messages) {
    normalized.push_back(agent_message_from_value(message));
  }
  return add_many(normalized);
}

void SessionMemory::compact() {
  // Hold compaction_pipeline_mutex_ across Phase 1 so two callers cannot
  // erase the message vector concurrently. In Synchronous mode it is also
  // held across Phase 2 (preserving the historical "one summarizer at a
  // time" property exercised by the R1 race repro). In Background mode the
  // dispatcher exits immediately, releasing the pipeline lock; the in-flight
  // Phase 2 is then serialized purely by refinement_in_flight_.
  std::unique_lock<std::mutex> pipeline_lock(compaction_pipeline_mutex_);

  CompactionPlan plan;
  std::size_t pre_token_count = 0;
  std::string snapshot_session_id;
  bool single_message_truncated = false;
  std::string warning;
  const bool phase1_did_work = run_phase1_locked(plan, pre_token_count,
                                                  snapshot_session_id,
                                                  single_message_truncated,
                                                  warning);

  // Even when Phase 1 is a no-op, we may still need to dispatch a
  // refinement: the previous attempt may have failed and left pending
  // overflow behind. This is the explicit "host retries after Failed" path
  // documented in the brief.
  bool has_pending_overflow = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    has_pending_overflow = !pending_refinement_overflow_.empty();
    if (snapshot_session_id.empty()) snapshot_session_id = session_id_;
  }
  if (!phase1_did_work && !has_pending_overflow) {
    return;  // truly nothing to do
  }

  if (phase1_did_work) {
    // Phase1Completed event — messages_ is bounded; summary_ contains the
    // updated denormalization including any new concat-tail lines.
    SessionCompactionEvent phase1;
    phase1.kind = SessionCompactionEvent::Kind::Phase1Completed;
    phase1.session_id = snapshot_session_id;
    phase1.dropped_count = plan.drop_count;
    phase1.pre_token_count = pre_token_count;
    phase1.single_message_truncated = single_message_truncated;
    phase1.warning = warning;
    notify_compaction(phase1);
  }

  // In Synchronous mode, fall through to Phase 2 on the calling thread
  // while still holding the pipeline lock — Synchronous mode === historical
  // "one compact in flight" semantics.
  if (summarizer_mode_ == SummarizerMode::Synchronous) {
    {
      std::lock_guard<std::mutex> rlock(refinement_mutex_);
      refinement_in_flight_ = true;
    }
    run_refinement_body();
    return;
  }

  // Background mode: release pipeline lock and hand Phase 2 to the executor.
  pipeline_lock.unlock();
  dispatch_refinement();
}

bool SessionMemory::run_phase1_locked(CompactionPlan& plan,
                                      std::size_t& pre_token_count,
                                      std::string& snapshot_session_id,
                                      bool& single_message_truncated_out,
                                      std::string& warning_out) {
  bool budget_empty = false;
  std::string snapshot_session_id_for_fail;
  SessionCompactionListener listener_copy_for_fail;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (compaction_budget_.empty()) {
      budget_empty = true;
      snapshot_session_id_for_fail = session_id_;
      listener_copy_for_fail = on_compaction_;
    } else {
      pre_token_count = estimated_token_count_unlocked();
    }
  }
  if (budget_empty) {
    SessionCompactionEvent failed_event;
    failed_event.kind = SessionCompactionEvent::Kind::Failed;
    failed_event.session_id = snapshot_session_id_for_fail;
    failed_event.error =
        "SessionMemory compaction triggered but no CompactionBudget set "
        "(neither max_messages nor max_tokens). Set at least one cap on "
        "SessionMemoryOptions::compaction.compaction_budget.";
    if (listener_copy_for_fail) listener_copy_for_fail(failed_event);
    throw ConfigurationError(
        failed_event.error,
        "{\"code\":\"no_compaction_budget\",\"field\":\"compaction.compaction_budget\"}");
  }

  // Plan first (under mutex_) to discover whether there is real work. Then
  // emit Started outside the lock (so listeners that re-enter the session
  // do not self-deadlock), then re-acquire mutex_ to apply the mutation.
  {
    std::lock_guard<std::mutex> lock(mutex_);
    plan = plan_compaction(PlanCompactionInput{
        .messages = std::span<const AgentMessage>(messages_),
        .summary = summary_,
        .summary_label = summary_label_,
        .budget = compaction_budget_,
        .token_counter = token_counter_,
        .truncate_oversized_message = truncate_oversized_message_,
        .oversized_tail_chars = oversized_tail_chars_,
    });
    snapshot_session_id = session_id_;
  }
  // Detect single-message soft-over-budget BEFORE early-returning. Even
  // when the planner is a no-op (single message, no truncate opt-in), we
  // still want to surface the warning so observability hosts can react.
  bool soft_over_budget = false;
  if (plan.drop_count == 0 && !plan.truncated_tail_message) {
    if (!truncate_oversized_message_ && compaction_budget_.max_tokens) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (messages_.size() == 1) {
        const std::size_t lone_tokens = measure_remaining_tokens(
            std::span<const AgentMessage>(messages_), /*drop=*/0, summary_,
            summary_label_, token_counter_);
        if (lone_tokens > *compaction_budget_.max_tokens) {
          soft_over_budget = true;
        }
      }
    }
    if (!soft_over_budget) {
      return false;  // truly no work
    }
    // Soft over-budget: emit a Phase1Completed-like event so the host sees
    // the warning. We do not fire Started/RefinementStarted because no
    // actual mutation or summarizer call happens — just a configuration
    // hint. Return false to short-circuit Phase 2 dispatch in compact().
    SessionCompactionEvent warn;
    warn.kind = SessionCompactionEvent::Kind::Phase1Completed;
    warn.session_id = snapshot_session_id;
    warn.warning =
        "single message exceeds max_tokens; enable "
        "SessionCompactionPolicy.truncate_oversized_message to opt in to "
        "tail truncation";
    notify_compaction(warn);
    warning_out = warn.warning;
    return false;
  }

  // Started fires once per compact() invocation that has real Phase 1 work.
  {
    SessionCompactionEvent started;
    started.kind = SessionCompactionEvent::Kind::Started;
    started.session_id = snapshot_session_id;
    started.pre_token_count = pre_token_count;
    started.dropped_count = plan.drop_count;
    notify_compaction(started);
  }

  // Phase 1 mutation critical section.
  SessionMemorySnapshot changed;
  bool had_work = false;
  bool truncated = false;
  bool over_budget_no_truncate = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);

    if (plan.drop_count > 0 && plan.drop_count <= messages_.size()) {
      // Append concat-tail lines BEFORE erasing so we don't have to hold a
      // reference into the moved-from prefix.
      for (const auto& msg : plan.overflow) {
        pending_concat_tail_ += format_concat_line(msg);
      }
      // Accumulate raw overflow for the eventual summarizer call.
      pending_refinement_overflow_.insert(pending_refinement_overflow_.end(),
                                          plan.overflow.begin(),
                                          plan.overflow.end());
      messages_.erase(messages_.begin(),
                      messages_.begin() + static_cast<std::ptrdiff_t>(plan.drop_count));
      had_work = true;
    }
    if (plan.truncated_tail_message) {
      // Replace the still-too-large tail with its truncated copy. Surface
      // the post-truncation text on the concat tail so the summary reflects
      // the new state of the conversation.
      if (!messages_.empty()) {
        messages_.back() = *plan.truncated_tail_message;
      }
      pending_concat_tail_ += format_concat_line(*plan.truncated_tail_message);
      truncated = true;
      had_work = true;
    }
    // Detect single-message soft-over-budget condition (no truncate opt-in):
    // post-plan, remaining count == 1 and that one message still exceeds the
    // token cap.
    if (!truncated && compaction_budget_.max_tokens && messages_.size() == 1) {
      const std::size_t lone_tokens = measure_remaining_tokens(
          std::span<const AgentMessage>(messages_), /*drop=*/0, summary_,
          summary_label_, token_counter_);
      if (lone_tokens > *compaction_budget_.max_tokens) {
        over_budget_no_truncate = true;
      }
    }

    if (had_work) {
      recompute_summary_unlocked();
      changed = snapshot_unlocked();
    }
  }

  single_message_truncated_out = truncated;
  if (over_budget_no_truncate) {
    warning_out =
        "single message exceeds max_tokens; enable "
        "SessionCompactionPolicy.truncate_oversized_message to opt in to "
        "tail truncation";
  } else {
    warning_out.clear();
  }

  if (had_work) {
    notify_change(changed);
  }
  return had_work;
}

void SessionMemory::dispatch_refinement() {
  std::function<void()> body;
  {
    std::lock_guard<std::mutex> rlock(refinement_mutex_);
    if (refinement_in_flight_) {
      // Mark trailing dispatch — the in-flight body will requeue itself
      // when it finishes.
      refinement_pending_ = true;
      return;
    }
    refinement_in_flight_ = true;
  }

  body = [this]() { this->run_refinement_body(); };

  if (background_executor_) {
    try {
      background_executor_(body);
    } catch (...) {
      // Executor refused the work. Reset bookkeeping and rethrow into a
      // detached failure event so we don't leave refinement_in_flight_ stuck.
      {
        std::lock_guard<std::mutex> rlock(refinement_mutex_);
        refinement_in_flight_ = false;
        refinement_pending_ = false;
      }
      refinement_cv_.notify_all();
      SessionCompactionEvent failed;
      failed.kind = SessionCompactionEvent::Kind::Failed;
      {
        std::lock_guard<std::mutex> dlock(mutex_);
        failed.session_id = session_id_;
      }
      failed.error = "background_executor rejected refinement work";
      notify_compaction(failed);
      throw;
    }
  } else {
    // Default fallback: detached std::thread.
    std::thread(body).detach();
  }
}

void SessionMemory::run_refinement_body() {
  for (;;) {
    // Snapshot the inputs under mutex_.
    SessionMemorySummarizer summarizer_copy;
    std::string previous_baseline;
    std::vector<AgentMessage> overflow_snapshot;
    std::string snapshot_session_id;
    std::size_t consumed_tail_size = 0;
    std::size_t consumed_overflow_size = 0;
    std::size_t pre_token_count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      summarizer_copy = summarizer_;
      previous_baseline = polished_baseline_;
      overflow_snapshot = pending_refinement_overflow_;
      snapshot_session_id = session_id_;
      consumed_tail_size = pending_concat_tail_.size();
      consumed_overflow_size = pending_refinement_overflow_.size();
      pre_token_count = estimated_token_count_unlocked();
    }

    // If nothing to refine, exit the loop cleanly without firing events.
    if (overflow_snapshot.empty()) {
      // Clear in-flight / pending and notify waiters.
      bool requeue = false;
      {
        std::lock_guard<std::mutex> rlock(refinement_mutex_);
        if (refinement_pending_) {
          // Pending without overflow shouldn't really happen, but treat it
          // as already-satisfied: clear and exit.
          refinement_pending_ = false;
        }
        refinement_in_flight_ = false;
        (void)requeue;
      }
      refinement_cv_.notify_all();
      return;
    }

    // RefinementStarted event.
    {
      SessionCompactionEvent started;
      started.kind = SessionCompactionEvent::Kind::RefinementStarted;
      started.session_id = snapshot_session_id;
      started.dropped_count = overflow_snapshot.size();
      started.pre_token_count = pre_token_count;
      notify_compaction(started);
    }

    const auto summarizer_start = std::chrono::steady_clock::now();
    std::string new_baseline;
    bool failed = false;
    std::string failure_msg;
    try {
      new_baseline = compute_new_polished_baseline(
          summarizer_copy, snapshot_session_id, std::move(previous_baseline),
          overflow_snapshot);
    } catch (const std::exception& ex) {
      failed = true;
      failure_msg = ex.what();
    } catch (...) {
      failed = true;
      failure_msg = "unknown summarizer exception";
    }
    const auto summarizer_duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - summarizer_start);

    if (failed) {
      // Preserve pending_* state so the host can retry by triggering another
      // compact(). Clear both refinement_in_flight_ and refinement_pending_
      // — refinement_pending_ is a 1-bit dispatch hint, NOT a retry queue.
      SessionCompactionEvent failed_event;
      failed_event.kind = SessionCompactionEvent::Kind::Failed;
      failed_event.session_id = snapshot_session_id;
      failed_event.dropped_count = overflow_snapshot.size();
      failed_event.pre_token_count = pre_token_count;
      failed_event.summarizer_duration = summarizer_duration;
      failed_event.error = failure_msg;
      notify_compaction(failed_event);

      {
        std::lock_guard<std::mutex> rlock(refinement_mutex_);
        refinement_in_flight_ = false;
        refinement_pending_ = false;
      }
      refinement_cv_.notify_all();
      return;
    }

    // Commit phase. We only consume up to `consumed_*` because callers may
    // have appended more to pending_concat_tail_ / pending_refinement_overflow_
    // while the summarizer was running.
    SessionMemorySnapshot changed;
    std::size_t post_token_count = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      polished_baseline_ = std::move(new_baseline);
      if (consumed_overflow_size <= pending_refinement_overflow_.size()) {
        pending_refinement_overflow_.erase(
            pending_refinement_overflow_.begin(),
            pending_refinement_overflow_.begin() +
                static_cast<std::ptrdiff_t>(consumed_overflow_size));
      } else {
        pending_refinement_overflow_.clear();
      }
      if (consumed_tail_size <= pending_concat_tail_.size()) {
        pending_concat_tail_.erase(0, consumed_tail_size);
      } else {
        pending_concat_tail_.clear();
      }
      recompute_summary_unlocked();
      changed = snapshot_unlocked();
      post_token_count = estimated_token_count_unlocked();
    }
    notify_change(changed);

    SessionCompactionEvent completed;
    completed.kind = SessionCompactionEvent::Kind::RefinementCompleted;
    completed.session_id = snapshot_session_id;
    completed.dropped_count = overflow_snapshot.size();
    completed.pre_token_count = pre_token_count;
    completed.post_token_count = post_token_count;
    completed.summarizer_duration = summarizer_duration;
    notify_compaction(completed);

    // Decide whether to do a trailing refinement. Read overflow size under
    // mutex_ (the field that owns it) THEN take the dispatch decision under
    // refinement_mutex_. The two locks are taken in order data → refinement
    // here, which is the only allowed ordering — but we release mutex_
    // immediately, so this does not violate the "refinement_mutex_ never
    // held with data mutex" invariant for the rest of the codebase.
    bool overflow_empty = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      overflow_empty = pending_refinement_overflow_.empty();
    }
    bool exit_loop = false;
    {
      std::lock_guard<std::mutex> rlock(refinement_mutex_);
      if (refinement_pending_ && !overflow_empty) {
        refinement_pending_ = false;
        // refinement_in_flight_ stays true — we continue the loop.
      } else {
        refinement_pending_ = false;
        refinement_in_flight_ = false;
        exit_loop = true;
      }
    }
    if (exit_loop) {
      refinement_cv_.notify_all();
      return;
    }
    // else continue the loop — trailing refinement swallows accumulated tail.
  }
}

void SessionMemory::await_refinements() const {
  std::unique_lock<std::mutex> lock(refinement_mutex_);
  refinement_cv_.wait(
      lock, [this] { return !refinement_in_flight_ && !refinement_pending_; });
}

std::size_t SessionMemory::estimated_token_count_unlocked() const {
  // Delegate to the shared measurement helper so the trigger
  // (should_auto_compact) and the planner (plan_compaction) use exactly
  // the same ruler — fixes the counter-split (Bug 1) and summary-
  // asymmetry (Bug 3) regressions.
  return measure_remaining_tokens(messages_, /*drop=*/0, summary_, summary_label_,
                                  token_counter_);
}

std::size_t SessionMemory::estimated_token_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return estimated_token_count_unlocked();
}

std::size_t SessionMemory::token_budget() const noexcept {
  return token_budget_;
}

SessionMemoryTokenCounter SessionMemory::token_counter() const {
  return token_counter_;
}

bool SessionMemory::should_auto_compact() const noexcept {
  if (token_budget_ == 0) {
    return false;
  }
  if (auto_compact_at_ <= 0.0) {
    return false;
  }
  std::size_t tokens = 0;
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens = estimated_token_count_unlocked();
  } catch (...) {
    return false;
  }
  const double threshold = auto_compact_at_ * static_cast<double>(token_budget_);
  return static_cast<double>(tokens) >= threshold;
}

std::vector<AgentMessage> SessionMemory::get_messages(bool include_summary) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<AgentMessage> messages;
  if (include_summary && !summary_.empty()) {
    messages.push_back(create_message(MessageRole::System, summary_label_ + ":\n" + summary_,
                                      Value::object({{"source", "session-memory"}})));
  }
  messages.insert(messages.end(), messages_.begin(), messages_.end());
  return messages;
}

std::vector<AgentMessage> SessionMemory::get_messages(const SessionMemoryGetMessagesOptions& options) const {
  return get_messages(options.include_summary);
}

SessionMemorySnapshot SessionMemory::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_unlocked();
}

SessionMemory& SessionMemory::restore(const SessionMemorySnapshot& snapshot) {
  SessionMemorySnapshot changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // Two-state restore: prefer the explicitly persisted polished + pending
    // fields when present. Fall back to the legacy single `summary` field
    // (treat it as the polished baseline with no pending work) so older
    // session files keep loading.
    messages_ = snapshot.messages;
    if (!snapshot.polished_baseline.empty() ||
        !snapshot.pending_concat_tail.empty() ||
        !snapshot.pending_refinement_overflow.empty()) {
      polished_baseline_ = snapshot.polished_baseline;
      pending_concat_tail_ = snapshot.pending_concat_tail;
      pending_refinement_overflow_ = snapshot.pending_refinement_overflow;
    } else {
      polished_baseline_ = snapshot.summary;
      pending_concat_tail_.clear();
      pending_refinement_overflow_.clear();
    }
    recompute_summary_unlocked();
    changed = snapshot_unlocked();
  }
  notify_change(changed);
  return *this;
}

SessionMemory& SessionMemory::restore(const SessionMemoryRestoreInput& snapshot) {
  SessionMemorySnapshot changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    messages_ = snapshot.messages.value_or(std::vector<AgentMessage>{});
    const bool has_two_state =
        snapshot.polished_baseline.has_value() ||
        snapshot.pending_concat_tail.has_value() ||
        snapshot.pending_refinement_overflow.has_value();
    if (has_two_state) {
      polished_baseline_ = snapshot.polished_baseline.value_or("");
      pending_concat_tail_ = snapshot.pending_concat_tail.value_or("");
      pending_refinement_overflow_ =
          snapshot.pending_refinement_overflow.value_or(std::vector<AgentMessage>{});
    } else {
      polished_baseline_ = snapshot.summary.value_or("");
      pending_concat_tail_.clear();
      pending_refinement_overflow_.clear();
    }
    recompute_summary_unlocked();
    changed = snapshot_unlocked();
  }
  notify_change(changed);
  return *this;
}

void SessionMemory::clear() {
  SessionMemorySnapshot changed;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    summary_.clear();
    messages_.clear();
    polished_baseline_.clear();
    pending_concat_tail_.clear();
    pending_refinement_overflow_.clear();
    changed = snapshot_unlocked();
  }
  notify_change(changed);
}

InMemorySessionStore::InMemorySessionStore(SessionMemoryOptions session_options)
    : session_options_(std::move(session_options)) {}

InMemorySessionStore::InMemorySessionStore(InMemorySessionStoreConfig config)
    : InMemorySessionStore(std::move(config.session_options)) {}

std::shared_ptr<SessionMemory> InMemorySessionStore::get(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = session_id.empty() ? "default" : session_id;
  auto found = sessions_.find(id);
  if (found != sessions_.end()) {
    return found->second;
  }
  auto options = session_options_;
  options.storage.session_id = id;
  auto session = std::make_shared<SessionMemory>(std::move(options));
  sessions_[id] = session;
  return session;
}

void InMemorySessionStore::clear(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_id.empty()) {
    sessions_.clear();
    return;
  }
  sessions_.erase(session_id);
}

std::vector<std::string> InMemorySessionStore::list_session_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> ids;
  ids.reserve(sessions_.size());
  for (const auto& [id, _] : sessions_) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

FileSessionStore::FileSessionStore(std::filesystem::path base_dir, std::string file_extension,
                                   CompactionBudget compaction_budget)
    : base_dir_(std::move(base_dir)),
      file_extension_(normalize_session_file_extension(std::move(file_extension))),
      session_options_() {
  session_options_.compaction.compaction_budget = std::move(compaction_budget);
}

FileSessionStore::FileSessionStore(std::filesystem::path base_dir, SessionMemoryOptions session_options)
    : FileSessionStore(std::move(base_dir), ".json", std::move(session_options)) {}

FileSessionStore::FileSessionStore(std::filesystem::path base_dir, std::string file_extension,
                                   SessionMemoryOptions session_options)
    : base_dir_(std::move(base_dir)),
      file_extension_(normalize_session_file_extension(std::move(file_extension))),
      session_options_(std::move(session_options)) {}

FileSessionStore::FileSessionStore(FileSessionStoreConfig config)
    : FileSessionStore(std::move(config.base_dir),
                       std::move(config.file_extension),
                       std::move(config.session_options)) {}

FileSessionStore::~FileSessionStore() {
  flush();
}

void write_session_snapshot_to_file(const std::filesystem::path& path,
                                    const SessionMemorySnapshot& snapshot);

std::shared_ptr<SessionMemory> FileSessionStore::get(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string id = session_id.empty() ? "default" : session_id;
  if (sessions_.contains(id)) {
    return sessions_.at(id);
  }
  const auto snapshot = read_snapshot(id);
  const auto path = file_path(id);
  auto options = session_options_;
  options.storage.session_id = id;
  if (snapshot) {
    // Construct the SessionMemory empty-of-storage, then restore so the
    // two-state pending fields are populated. (Storage::summary maps only
    // to the legacy single-string baseline; the polished + pending fields
    // are not part of SessionStorage.)
    options.storage.summary.clear();
    options.storage.messages.clear();
  }
  options.on_change = [path](const SessionMemorySnapshot& changed) {
    write_session_snapshot_to_file(path, changed);
  };
  auto session = std::make_shared<SessionMemory>(std::move(options));
  if (snapshot) {
    session->restore(*snapshot);
  }
  sessions_[id] = session;
  return session;
}

void FileSessionStore::clear(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_id.empty()) {
    sessions_.clear();
    std::filesystem::remove_all(base_dir_);
    return;
  }
  sessions_.erase(session_id);
  std::filesystem::remove(file_path(session_id));
}

void FileSessionStore::flush(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (session_id.empty()) {
    for (const auto& [_, session] : sessions_) {
      write_snapshot(session->snapshot());
    }
    return;
  }
  const auto found = sessions_.find(session_id);
  if (found != sessions_.end()) {
    write_snapshot(found->second->snapshot());
  }
}

std::vector<std::string> FileSessionStore::list_session_ids() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::set<std::string> ids;
  for (const auto& [id, _] : sessions_) {
    ids.insert(id);
  }
  if (std::filesystem::exists(base_dir_)) {
    for (const auto& entry : std::filesystem::directory_iterator(base_dir_)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto filename = entry.path().filename().string();
      if (filename.size() < file_extension_.size()) {
        continue;
      }
      if (filename.compare(filename.size() - file_extension_.size(), file_extension_.size(), file_extension_) == 0) {
        const auto decoded = decode_uri_component(filename.substr(0, filename.size() - file_extension_.size()));
        if (decoded) {
          ids.insert(*decoded);
        }
      }
    }
  }
  return std::vector<std::string>(ids.begin(), ids.end());
}

std::filesystem::path FileSessionStore::file_path(const std::string& session_id) const {
  return base_dir_ / (encode_uri_component(session_id) + file_extension_);
}

std::optional<SessionMemorySnapshot> FileSessionStore::read_snapshot(const std::string& session_id) const {
  const auto path = file_path(session_id);
  if (!std::filesystem::exists(path)) {
    return std::nullopt;
  }
  const auto raw = read_json_file(path);
  SessionMemorySnapshot snapshot;
  snapshot.session_id = raw.at("sessionId").as_string(session_id);
  snapshot.summary = raw.at("summary").as_string();
  for (const auto& item : raw.at("messages").as_array()) {
    snapshot.messages.push_back(agent_message_from_value(item));
  }
  // Two-state fields are OPTIONAL — legacy snapshots written before the
  // background-summarizer upgrade do not have them. as_string() / as_array()
  // on a missing key returns the default-constructed value so reads stay
  // forward/backward compatible.
  snapshot.polished_baseline = raw.at("polishedBaseline").as_string();
  snapshot.pending_concat_tail = raw.at("pendingConcatTail").as_string();
  for (const auto& item : raw.at("pendingRefinementOverflow").as_array()) {
    snapshot.pending_refinement_overflow.push_back(agent_message_from_value(item));
  }
  return snapshot;
}

void write_session_snapshot_to_file(const std::filesystem::path& path,
                                    const SessionMemorySnapshot& snapshot) {
  Value::Array messages;
  for (const auto& message : snapshot.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array overflow;
  for (const auto& message : snapshot.pending_refinement_overflow) {
    overflow.push_back(agent_message_to_value(message));
  }
  write_json_file(path,
                  Value::object({{"sessionId", snapshot.session_id},
                                 {"summary", snapshot.summary},
                                 {"messages", Value(messages)},
                                 {"polishedBaseline", snapshot.polished_baseline},
                                 {"pendingConcatTail", snapshot.pending_concat_tail},
                                 {"pendingRefinementOverflow", Value(overflow)}}));
}

void FileSessionStore::write_snapshot(const SessionMemorySnapshot& snapshot) const {
  write_session_snapshot_to_file(file_path(snapshot.session_id), snapshot);
}

std::shared_ptr<SessionMemory> rehydrate_session_memory(const SessionMemorySnapshot& snapshot,
                                                        SessionMemoryOptions options) {
  options.storage.session_id = snapshot.session_id;
  options.storage.summary.clear();
  options.storage.messages.clear();
  auto session = std::make_shared<SessionMemory>(std::move(options));
  session->restore(snapshot);
  return session;
}
