#pragma once

#include "agent/model.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace agent {

class NativeWebCrawler;
class NativeWebPageFetcher;
class BrowserRenderer;

// Snapshot of a SessionMemory's externally observable state.
//
// `summary` is the denormalized join of the LLM-refined `polished_baseline`
// with the cheap concat `pending_concat_tail` produced by Phase 1 compactions
// that have not yet been refined by Phase 2. Hosts that only render the
// summary as a single string should read `summary`. Hosts that persist the
// session and need to resume an in-flight refinement after restart should
// also persist `polished_baseline`, `pending_concat_tail`, and
// `pending_refinement_overflow` and restore them via SessionMemoryRestoreInput.
struct SessionMemorySnapshot {
  std::string session_id;
  std::string summary;
  std::vector<AgentMessage> messages;
  std::string polished_baseline;
  std::string pending_concat_tail;
  std::vector<AgentMessage> pending_refinement_overflow;
};

Value session_memory_snapshot_to_value(const SessionMemorySnapshot& snapshot);

struct SessionMemoryRestoreInput {
  std::optional<std::string> session_id;
  std::optional<std::string> summary;
  std::optional<std::vector<AgentMessage>> messages;
  std::optional<std::string> polished_baseline;
  std::optional<std::string> pending_concat_tail;
  std::optional<std::vector<AgentMessage>> pending_refinement_overflow;
};

struct SessionMemoryGetMessagesOptions {
  bool include_summary = true;
};

struct SessionMemorySummaryInput {
  std::string session_id;
  std::string previous_summary;
  std::vector<AgentMessage> archived_messages;
};

using SessionMemoryChangeListener = std::function<void(const SessionMemorySnapshot&)>;

// SessionMemorySummarizer: synchronous string return.
//
// In SummarizerMode::Synchronous the summarizer runs on the calling thread
// of compact() / add() — the body may block freely but blocks the caller.
//
// In SummarizerMode::Background the summarizer body runs on whatever thread
// the SessionCompactionExecutor dispatches it onto. The summarizer body
// itself remains synchronous (still a `std::string` return) — only the
// dispatch is asynchronous. Implementations may therefore block on an LLM
// call without bouncing through a future/coroutine; the framework guarantees
// the body never runs on the agent loop thread when Background mode is set.
using SessionMemorySummarizer = std::function<std::string(const SessionMemorySummaryInput&)>;

// Selects how Phase 2 (the LLM-summarizer refinement) is dispatched.
//   Synchronous  - Phase 2 runs on the calling thread (default; preserves
//                  the historical behavior so existing tests do not regress).
//   Background   - Phase 1 (drop + concat) runs on the calling thread and
//                  returns immediately; Phase 2 is dispatched via the policy's
//                  SessionCompactionExecutor. While a refinement is in flight,
//                  additional Phase 1 compactions accumulate into the pending
//                  concat tail; a single trailing refinement is queued to
//                  swallow that accumulation when the in-flight one finishes.
enum class SummarizerMode {
  Synchronous,
  Background,
};

// Executor that runs Phase 2 summarizer work off the agent loop thread.
//
// Hosts plug in std::thread, std::async, Qt's QThreadPool::start, a custom
// pool, etc. When SummarizerMode::Background is set and no executor is
// supplied, the framework falls back to `std::thread([w]{ w(); }).detach()`.
//
// The work callable runs at most once; implementations MUST NOT discard it
// (drop-on-overflow is the host's responsibility — write a wrapping executor
// if you want bounded behavior).
using SessionCompactionExecutor = std::function<void(std::function<void()>)>;

// Host-supplied token counter. The framework calls this with a snapshot
// equivalent to:
//   [system: "<summary_label>:\n<summary>"]   ← prepended only when summary
//                                                is non-empty
//   <messages...>
// i.e. exactly the projection used both to TRIGGER auto-compaction
// (estimated_token_count_unlocked) and to PLAN truncation (plan_compaction).
// Implementations should sum tokens across the whole snapshot in whatever
// model-specific way they prefer (tiktoken, provider usage API, …).
// The counter MUST be deterministic — the planner runs it once per drop
// candidate during compaction loops.
using SessionMemoryTokenCounter =
    std::function<std::size_t(const std::vector<AgentMessage>&)>;

// Composable per-side compaction caps. Truncation drops oldest messages
// until every cap that is set evaluates true. At least one cap must be
// set when a SessionMemory uses auto-compaction — otherwise compaction
// would be a silent no-op (the bug this struct exists to prevent).
struct CompactionBudget {
  std::optional<std::size_t> max_messages;   // post-compaction message count
  std::optional<std::size_t> max_tokens;     // post-compaction estimated tokens
  // True when neither cap is set — used by SessionMemory to throw at
  // compaction time rather than silently doing nothing.
  [[nodiscard]] bool empty() const noexcept;
};

// Result of the pure planning phase of compaction. Computed under-lock from
// a snapshot of state; consumed outside the lock by the summarizer; the
// resulting summary is written back under a short lock. drop_count is the
// number of oldest messages to remove; overflow is the snapshot of those
// dropped messages, used as the summarizer input.
//
// `truncated_tail_message` handles the single-message overflow edge case:
// when the message list collapses to a single message that, on its own,
// exceeds CompactionBudget::max_tokens (so plan_compaction cannot drop it
// without leaving the conversation empty), and the policy opts in via
// SessionCompactionPolicy::truncate_oversized_message=true, the planner
// fills `truncated_tail_message` with a copy of that tail message whose
// text content has been truncated to the last `oversized_tail_chars`
// characters. The applier then replaces messages_.back() with it under
// the data lock. When the policy does NOT opt in, the field stays empty
// and the SessionCompactionEvent reports the soft over-budget condition
// via its `warning` field.
struct CompactionPlan {
  std::size_t drop_count = 0;
  std::vector<AgentMessage> overflow;
  std::optional<AgentMessage> truncated_tail_message;
};

// Inputs to the pure planning phase of compaction. `messages` is a
// non-owning view over the live message vector; `summary`/`summary_label`
// describe the summary projected as a virtual system message that the
// trigger and the planner both measure against. `token_counter` may be
// empty — in that case the planner falls back to the shared char/4
// estimator (still summary-inclusive).
struct PlanCompactionInput {
  std::span<const AgentMessage> messages;
  std::string_view summary;
  std::string_view summary_label;
  CompactionBudget budget;
  SessionMemoryTokenCounter token_counter;
  // When true and the planner would leave a single message exceeding
  // budget.max_tokens, populate CompactionPlan::truncated_tail_message
  // with a copy whose text payload is tail-truncated to `oversized_tail_chars`.
  bool truncate_oversized_message = false;
  std::size_t oversized_tail_chars = 4000;
};

// Pure function. Takes a snapshot of state and decides what to drop.
// Does NOT mutate inputs. Does NOT call summarizer. Does NOT acquire any
// lock. Safe to call with or without the SessionMemory mutex held.
CompactionPlan plan_compaction(const PlanCompactionInput& input);

// SessionCompactionEvent is published on the SessionMemory observer
// channel as well as bridged to the runner EventBus (topic
// "session.compaction") so hosts can observe compaction latency and
// drop counts independently of the per-snapshot on_change listener.
//
// Event taxonomy (Synchronous mode collapses Started+Phase1Completed and
// RefinementStarted+RefinementCompleted onto the calling thread):
//   Started             - Phase 1 begin (about to drop overflow)
//   Phase1Completed     - Phase 1 done; messages_ is bounded; summary_
//                         denormalization includes the new concat tail
//   RefinementStarted   - Phase 2 about to run (Synchronous: same thread;
//                         Background: just dispatched onto the executor)
//   RefinementCompleted - Phase 2 success; polished_baseline_ replaced;
//                         pending_concat_tail_ + pending_refinement_overflow_
//                         cleared
//   Failed              - any phase failed; pending_* state preserved so
//                         the host can retry via the next compact()
struct SessionCompactionEvent {
  enum class Kind {
    Started,
    Phase1Completed,
    RefinementStarted,
    RefinementCompleted,
    Failed,
  };
  Kind kind;
  std::string session_id;
  std::size_t dropped_count = 0;
  std::size_t pre_token_count = 0;
  std::size_t post_token_count = 0;
  std::chrono::milliseconds summarizer_duration{0};
  bool single_message_truncated = false;  // set on Phase1Completed/RefinementCompleted
  std::string warning;                    // soft over-budget / non-fatal hints
  std::string error;                      // only set on Failed
};
std::string to_string(SessionCompactionEvent::Kind);
using SessionCompactionListener = std::function<void(const SessionCompactionEvent&)>;

// Persistent state slice — what a SessionMemory STORES.
struct SessionStorage {
  std::string session_id = "default";
  std::vector<AgentMessage> messages;
  std::string summary;
  std::string summary_label = "Conversation summary";
};

// Compaction policy slice — what a SessionMemory DOES when it compacts.
// `compact_on_append` defaults to false: eager per-append compaction
// makes every add() pay summarizer latency; most hosts want compaction
// only at iteration boundaries (driven by maybe_auto_compact_session).
// Opt in explicitly to restore the per-append behavior.
struct SessionCompactionPolicy {
  CompactionBudget compaction_budget;
  double auto_compact_at = 0.8;
  std::size_t token_budget = 0;
  bool compact_on_append = false;
  SessionMemorySummarizer summarizer;
  SessionMemoryTokenCounter token_counter;
  // Cap on the post-summarize summary string length, applied to the
  // denormalized join `polished_baseline + pending_concat_tail` as well
  // as the built-in (no host summarizer) summarize_messages fallback.
  std::size_t summary_max_chars = 8000;

  // === Background summarizer plumbing ===
  // Default Synchronous so existing call sites behave as before.
  SummarizerMode summarizer_mode = SummarizerMode::Synchronous;
  // Optional. When null and summarizer_mode == Background, the framework
  // dispatches Phase 2 onto a detached std::thread.
  SessionCompactionExecutor background_executor;

  // === Single-message overflow handling ===
  // Opt in to tail-truncate the last remaining message when it alone
  // exceeds compaction_budget.max_tokens. Off by default to preserve the
  // historical "leave conversation intact, surface a warning" semantics.
  bool truncate_oversized_message = false;
  std::size_t oversized_tail_chars = 4000;
};

struct SessionMemoryOptions {
  SessionStorage storage;
  SessionCompactionPolicy compaction;
  SessionMemoryChangeListener on_change;
  SessionCompactionListener on_compaction;
};

struct InMemorySessionStoreConfig {
  SessionMemoryOptions session_options = {};
};

struct FileSessionStoreConfig {
  std::filesystem::path base_dir;
  std::string file_extension = ".json";
  SessionMemoryOptions session_options = {};
};

// SessionMemory — synchronized chat history with budget-driven compaction.
//
// Lock ordering discipline (MUST be respected):
//   compaction_pipeline_mutex_  →  mutex_      (Phase 1)
//   refinement_mutex_           (independent — NEVER held with mutex_)
//
// `compaction_pipeline_mutex_` serializes Phase 1 (plan + erase + concat-tail
// recompute) so two callers cannot drop messages concurrently and clobber
// each other. In Synchronous mode it ALSO covers Phase 2 (the LLM call)
// to preserve the historical "one compact in flight" behavior. In Background
// mode it is released before Phase 2 is dispatched onto the executor.
//
// `refinement_mutex_` guards the refinement bookkeeping (in-flight flag,
// pending flag, pending_concat_tail_, pending_refinement_overflow_,
// polished_baseline_). It is acquired only across short critical sections
// and NEVER held with `mutex_` to avoid lock-order inversion.
//
// `mutex_` is acquired only across short critical sections (append, planner,
// erase, summary denormalization, snapshot construction). It MUST NEVER be
// acquired BEFORE compaction_pipeline_mutex_ — reads (snapshot,
// estimated_token_count, get_messages) take only mutex_ and therefore never
// block a refinement.
class SessionMemory {
 public:
  explicit SessionMemory(SessionMemoryOptions options);
  ~SessionMemory();
  SessionMemory(const SessionMemory&) = delete;
  SessionMemory& operator=(const SessionMemory&) = delete;
  SessionMemory(SessionMemory&&) = delete;
  SessionMemory& operator=(SessionMemory&&) = delete;

  [[nodiscard]] const std::string& session_id() const noexcept;
  SessionMemory& add(AgentMessage message);
  SessionMemory& add(const Value& message);
  SessionMemory& add_many(const std::vector<AgentMessage>& messages);
  SessionMemory& add_many(const std::vector<Value>& messages);
  void compact();
  [[nodiscard]] bool should_auto_compact() const noexcept;
  [[nodiscard]] std::size_t estimated_token_count() const;
  [[nodiscard]] std::size_t token_budget() const noexcept;
  [[nodiscard]] std::vector<AgentMessage> get_messages(bool include_summary = true) const;
  [[nodiscard]] std::vector<AgentMessage> get_messages(const SessionMemoryGetMessagesOptions& options) const;
  [[nodiscard]] SessionMemorySnapshot snapshot() const;
  SessionMemory& restore(const SessionMemorySnapshot& snapshot);
  SessionMemory& restore(const SessionMemoryRestoreInput& snapshot);
  void clear();

  // Blocks until no Phase 2 refinement is in flight and no trailing refinement
  // is pending. Safe to call from any thread except the executor thread that
  // runs the refinement itself (would deadlock). Hosts use this for tests,
  // session persistence checkpoints, and graceful shutdown.
  void await_refinements() const;

 private:
  [[nodiscard]] SessionMemorySnapshot snapshot_unlocked() const;
  void notify_change(const SessionMemorySnapshot& snapshot) const;
  void notify_compaction(const SessionCompactionEvent& event) const;

  // Phase 1: plan + erase + concat-tail update + summary denormalization.
  // Returns true when there was real work, false when the plan was a no-op.
  // pre_token_count/snapshot_session_id are out-params populated when work
  // is done. Held under compaction_pipeline_mutex_ in the caller; this fn
  // acquires mutex_ and refinement_mutex_ internally per the lock order.
  bool run_phase1_locked(CompactionPlan& plan,
                         std::size_t& pre_token_count,
                         std::string& snapshot_session_id,
                         bool& single_message_truncated_out,
                         std::string& warning_out);

  // Phase 2 execution body. Reads the current pending tail + overflow under
  // refinement_mutex_ at start, runs the summarizer outside any lock, then
  // commits polished_baseline_ + clears the consumed tail/overflow under
  // refinement_mutex_. Fires RefinementStarted/RefinementCompleted/Failed.
  // Returns the duration of the summarizer call.
  void run_refinement_body();

  // Dispatch Phase 2 according to summarizer_mode_. Synchronous: run on the
  // calling thread immediately. Background: hand the work to background_executor_
  // (or std::thread fallback) and return without blocking. Both update the
  // refinement_in_flight_ / refinement_pending_ flags atomically.
  void dispatch_refinement();

  // Recompute denormalized summary_ from polished_baseline_ + pending_concat_tail_
  // applying summary_max_chars_ cap. mutex_ MUST be held.
  void recompute_summary_unlocked();

  [[nodiscard]] std::size_t estimated_token_count_unlocked() const;

  mutable std::mutex compaction_pipeline_mutex_;  // serializes Phase 1; covers Phase 2 in Synchronous mode
  mutable std::mutex mutex_;                      // guards messages_/summary_/etc — short critical sections only
  mutable std::mutex refinement_mutex_;           // guards refinement bookkeeping + pending tail/overflow
  mutable std::condition_variable refinement_cv_;
  std::string session_id_;
  CompactionBudget compaction_budget_;
  std::string summary_label_;
  std::string summary_;                            // denormalized: polished_baseline_ + pending_concat_tail_
  std::vector<AgentMessage> messages_;
  // Two-state summary model — see Snapshot doc.
  std::string polished_baseline_;
  std::string pending_concat_tail_;
  std::vector<AgentMessage> pending_refinement_overflow_;
  bool refinement_in_flight_ = false;
  bool refinement_pending_ = false;
  SessionMemoryChangeListener on_change_;
  SessionCompactionListener on_compaction_;
  SessionMemorySummarizer summarizer_;
  double auto_compact_at_ = 0.8;
  std::size_t token_budget_ = 0;
  SessionMemoryTokenCounter token_counter_;
  bool compact_on_append_ = false;
  std::size_t summary_max_chars_ = 8000;
  SummarizerMode summarizer_mode_ = SummarizerMode::Synchronous;
  SessionCompactionExecutor background_executor_;
  bool truncate_oversized_message_ = false;
  std::size_t oversized_tail_chars_ = 4000;
};

class SessionStore {
 public:
  virtual ~SessionStore() = default;
  virtual std::shared_ptr<SessionMemory> get(const std::string& session_id = "default") = 0;
  virtual void clear(const std::string& session_id = {}) = 0;
  virtual void flush(const std::string& session_id = {}) { (void)session_id; }
  [[nodiscard]] virtual std::vector<std::string> list_session_ids() const { return {}; }
};

class InMemorySessionStore : public SessionStore {
 public:
  explicit InMemorySessionStore(SessionMemoryOptions session_options = {});
  explicit InMemorySessionStore(InMemorySessionStoreConfig config);
  std::shared_ptr<SessionMemory> get(const std::string& session_id = "default") override;
  void clear(const std::string& session_id = {}) override;
  [[nodiscard]] std::vector<std::string> list_session_ids() const override;

 private:
  mutable std::mutex mutex_;
  SessionMemoryOptions session_options_;
  std::unordered_map<std::string, std::shared_ptr<SessionMemory>> sessions_;
};

class FileSessionStore : public SessionStore {
 public:
  FileSessionStore(std::filesystem::path base_dir, std::string file_extension = ".json",
                   CompactionBudget compaction_budget = {});
  FileSessionStore(std::filesystem::path base_dir, SessionMemoryOptions session_options);
  FileSessionStore(std::filesystem::path base_dir, std::string file_extension,
                   SessionMemoryOptions session_options);
  explicit FileSessionStore(FileSessionStoreConfig config);
  ~FileSessionStore() override;
  std::shared_ptr<SessionMemory> get(const std::string& session_id = "default") override;
  void clear(const std::string& session_id = {}) override;
  void flush(const std::string& session_id = {}) override;
  [[nodiscard]] std::vector<std::string> list_session_ids() const override;

 private:
  [[nodiscard]] std::filesystem::path file_path(const std::string& session_id) const;
  [[nodiscard]] std::optional<SessionMemorySnapshot> read_snapshot(const std::string& session_id) const;
  void write_snapshot(const SessionMemorySnapshot& snapshot) const;

  mutable std::mutex mutex_;
  std::filesystem::path base_dir_;
  std::string file_extension_;
  SessionMemoryOptions session_options_;
  std::unordered_map<std::string, std::shared_ptr<SessionMemory>> sessions_;
};

struct RetrievedMemory {
  std::string id;
  std::string content;
  double score = 0;
  Value metadata = Value::object({});
  std::string namespace_id = "default";
};

struct VectorMemoryRecord {
  std::string id;
  std::string content;
  EmbeddingVector embedding;
  Value metadata = Value::object({});
  std::string namespace_id = "default";
  std::string created_at;
  std::string updated_at;
};

struct VectorMemoryUpsertInput {
  std::string id;
  std::string content;
  EmbeddingVector embedding;
  Value metadata = Value::object({});
  std::string namespace_id;
};

struct VectorMemoryQuery {
  EmbeddingVector embedding;
  std::size_t top_k = 4;
  double min_score = 0.2;
  std::string namespace_id;
};

struct InMemoryVectorStoreConfig {
  std::string namespace_id = "default";
};

struct FileVectorStoreConfig {
  std::filesystem::path file_path;
  std::string namespace_id = "default";
};

struct RememberMemoryInput {
  std::string id;
  std::string content;
  Value metadata = Value::object({});
  std::string namespace_id;
};

struct RememberConversationPlanStep {
  std::string title;
};

struct RememberConversationPlan {
  std::vector<RememberConversationPlanStep> steps;
};

struct RememberConversationTurnInput {
  std::string session_id;
  std::string input;
  std::string output;
  std::string namespace_id;
  Value metadata = Value::object({});
  std::optional<RememberConversationPlan> plan;
  std::vector<std::string> plan_steps;
};

struct SearchMemoryOptions {
  std::optional<std::size_t> top_k;
  std::optional<double> min_score;
  std::string namespace_id;
};

struct LongTermMemoryContextResult {
  std::vector<RetrievedMemory> hits;
  std::optional<AgentMessage> message;
};

Value retrieved_memory_to_value(const RetrievedMemory& memory);
Value vector_memory_record_to_value(const VectorMemoryRecord& record);
Value long_term_memory_context_result_to_value(const LongTermMemoryContextResult& result);

class VectorStore {
 public:
  virtual ~VectorStore() = default;
  virtual std::vector<VectorMemoryRecord> upsert(const std::vector<VectorMemoryUpsertInput>& items) = 0;
  virtual std::vector<RetrievedMemory> query(const VectorMemoryQuery& query) = 0;
  virtual std::size_t erase(const std::vector<std::string>& ids) = 0;
  virtual std::size_t delete_ids(const std::vector<std::string>& ids) { return erase(ids); }
  virtual void clear(const std::string& namespace_id = {}) = 0;
  virtual std::size_t count(const std::string& namespace_id = {}) const = 0;
};

class InMemoryVectorStore : public VectorStore {
 public:
  explicit InMemoryVectorStore(std::string namespace_id = "default");
  explicit InMemoryVectorStore(InMemoryVectorStoreConfig config);

  std::vector<VectorMemoryRecord> upsert(const std::vector<VectorMemoryUpsertInput>& items) override;
  std::vector<RetrievedMemory> query(const VectorMemoryQuery& query) override;
  std::size_t erase(const std::vector<std::string>& ids) override;
  void clear(const std::string& namespace_id = {}) override;
  std::size_t count(const std::string& namespace_id = {}) const override;

 private:
  mutable std::mutex mutex_;
  std::string namespace_id_;
  std::unordered_map<std::string, VectorMemoryRecord> records_;
};

class FileVectorStore : public VectorStore {
 public:
  FileVectorStore(std::filesystem::path file_path, std::string namespace_id = "default");
  explicit FileVectorStore(FileVectorStoreConfig config);
  std::vector<VectorMemoryRecord> upsert(const std::vector<VectorMemoryUpsertInput>& items) override;
  std::vector<RetrievedMemory> query(const VectorMemoryQuery& query) override;
  std::size_t erase(const std::vector<std::string>& ids) override;
  void clear(const std::string& namespace_id = {}) override;
  std::size_t count(const std::string& namespace_id = {}) const override;

 private:
  void ensure_loaded() const;
  void persist() const;

  std::filesystem::path file_path_;
  std::string namespace_id_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::unordered_map<std::string, VectorMemoryRecord> records_;
};

struct LongTermMemoryConfig {
  std::shared_ptr<TextEmbeddingAdapter> embedder = std::make_shared<HashEmbeddingAdapter>();
  std::shared_ptr<VectorStore> store = std::make_shared<InMemoryVectorStore>();
  std::string namespace_id = "default";
  std::size_t top_k = 4;
  double min_score = 0.2;
  bool auto_remember = true;
  std::string context_title = "Long-term memory retrieval";
};

class LongTermMemory {
 public:
  LongTermMemory(std::shared_ptr<TextEmbeddingAdapter> embedder = std::make_shared<HashEmbeddingAdapter>(),
                 std::shared_ptr<VectorStore> store = std::make_shared<InMemoryVectorStore>(),
                 std::string namespace_id = "default", std::size_t top_k = 4, double min_score = 0.2,
                 bool auto_remember = true, std::string context_title = "Long-term memory retrieval");
  explicit LongTermMemory(LongTermMemoryConfig config);

  VectorMemoryRecord remember(std::string content, Value metadata = Value::object({}),
                              std::string namespace_id = {},
                              CancellationToken* cancellation = nullptr);
  VectorMemoryRecord remember(const RememberMemoryInput& input,
                              CancellationToken* cancellation = nullptr);
  std::vector<VectorMemoryRecord> remember_many(const std::vector<RememberMemoryInput>& inputs,
                                                CancellationToken* cancellation = nullptr);
  std::vector<RetrievedMemory> search(const std::string& query,
                                      std::optional<std::size_t> top_k = std::nullopt,
                                      std::optional<double> min_score = std::nullopt,
                                      std::string namespace_id = {},
                                      CancellationToken* cancellation = nullptr);
  std::vector<RetrievedMemory> search(const std::string& query,
                                      const SearchMemoryOptions& options,
                                      CancellationToken* cancellation = nullptr);
  std::optional<AgentMessage> create_context_message(const std::vector<RetrievedMemory>& hits) const;
  LongTermMemoryContextResult build_context_message(const std::string& query,
                                                    std::optional<std::size_t> top_k = std::nullopt,
                                                    std::optional<double> min_score = std::nullopt,
                                                    std::string namespace_id = {},
                                                    CancellationToken* cancellation = nullptr);
  LongTermMemoryContextResult build_context_message(const std::string& query,
                                                    const SearchMemoryOptions& options,
                                                    CancellationToken* cancellation = nullptr);
  [[nodiscard]] bool auto_remember() const noexcept;
  VectorMemoryRecord remember_conversation_turn(const std::string& session_id, const std::string& input,
                                                const std::string& output, Value metadata = Value::object({}),
                                                std::string namespace_id = {},
                                                std::vector<std::string> plan_steps = {});
  VectorMemoryRecord remember_conversation_turn(const RememberConversationTurnInput& input);

 private:
  std::shared_ptr<TextEmbeddingAdapter> embedder_;
  std::shared_ptr<VectorStore> store_;
  std::string namespace_id_;
  std::size_t top_k_;
  double min_score_;
  bool auto_remember_;
  std::string context_title_;
};

enum class KnowledgeAssetType {
  Text,
  Image,
};

struct LoadedKnowledgeDocument {
  std::string uri;
  std::string title;
  std::string content;
  std::string source_type = "manual";
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::optional<MediaSource> media;
  std::string text_hint;
  std::string alt_text;
  std::string ocr_text;
  std::string caption;
  Value metadata = Value::object({});
};

class KnowledgeSourceLoader {
 public:
  virtual ~KnowledgeSourceLoader() = default;
  [[nodiscard]] virtual bool supports(const Value& source) const = 0;
  [[nodiscard]] virtual std::vector<LoadedKnowledgeDocument> load(const Value& source) const = 0;
};

struct KnowledgeProviderMetadata {
  std::string name;
  std::string tier = "core-safe";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
};

class KnowledgeLoaderRegistry;

struct KnowledgeLoaderProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeSourceLoader>(
      const Value& options,
      const KnowledgeLoaderRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeLoaderRegistry {
 public:
  explicit KnowledgeLoaderRegistry(std::vector<KnowledgeLoaderProvider> providers = {});
  KnowledgeLoaderRegistry(const KnowledgeLoaderRegistry& other);
  KnowledgeLoaderRegistry& operator=(const KnowledgeLoaderRegistry& other);
  KnowledgeLoaderRegistry(KnowledgeLoaderRegistry&& other) noexcept;
  KnowledgeLoaderRegistry& operator=(KnowledgeLoaderRegistry&& other) noexcept;
  KnowledgeLoaderProvider& register_provider(KnowledgeLoaderProvider provider);
  [[nodiscard]] const KnowledgeLoaderProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeSourceLoader> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeLoaderProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeLoaderProvider> providers_;
  std::vector<std::string> provider_order_;
};

class TextKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;
};

class FileKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;
};

class DirectoryKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit DirectoryKnowledgeSourceLoader(std::shared_ptr<FileKnowledgeSourceLoader> file_loader =
                                              std::make_shared<FileKnowledgeSourceLoader>());
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  std::shared_ptr<FileKnowledgeSourceLoader> file_loader_;
};

class MarkdownKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit MarkdownKnowledgeSourceLoader(bool recursive = true);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  bool recursive_;
};

class RepositoryKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit RepositoryKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher = nullptr,
                                           std::set<std::string> extensions = {},
                                           std::set<std::string> exclude_directories = {});
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  std::vector<LoadedKnowledgeDocument> load_local_repository(const Value& source) const;
  std::vector<LoadedKnowledgeDocument> load_github_repository(const Value& source) const;

  const NativeWebPageFetcher* fetcher_;
  std::set<std::string> extensions_;
  std::set<std::string> exclude_directories_;
};

class WebKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit WebKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher = nullptr);
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  const NativeWebPageFetcher* fetcher_;
};

class WebsiteKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit WebsiteKnowledgeSourceLoader(const NativeWebCrawler* crawler = nullptr,
                                        BrowserRenderer* browser = nullptr);
  void set_crawler(const NativeWebCrawler* crawler);
  void set_browser(BrowserRenderer* browser);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  const NativeWebCrawler* crawler_;
  BrowserRenderer* browser_;
};

class SitemapKnowledgeSourceLoader : public KnowledgeSourceLoader {
 public:
  explicit SitemapKnowledgeSourceLoader(const NativeWebPageFetcher* fetcher = nullptr,
                                        std::optional<std::size_t> default_limit = std::nullopt);
  void set_fetcher(const NativeWebPageFetcher* fetcher);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  const NativeWebPageFetcher* fetcher_;
  std::optional<std::size_t> default_limit_;
};

class CompositeKnowledgeLoader : public KnowledgeSourceLoader {
 public:
  explicit CompositeKnowledgeLoader(std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders = {},
                                    bool use_default_loaders_when_empty = true);
  void add_loader(std::shared_ptr<KnowledgeSourceLoader> loader);
  [[nodiscard]] bool supports(const Value& source) const override;
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> load(const Value& source) const override;

 private:
  std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders_;
};

std::vector<LoadedKnowledgeDocument> load_knowledge_sources(
    const std::vector<Value>& sources,
    const KnowledgeSourceLoader& loader);

std::shared_ptr<CompositeKnowledgeLoader> create_default_knowledge_loader(
    const NativeWebPageFetcher* fetcher = nullptr,
    const NativeWebCrawler* crawler = nullptr,
    BrowserRenderer* browser = nullptr);

KnowledgeLoaderRegistry create_default_knowledge_loader_registry(
    const NativeWebPageFetcher* fetcher = nullptr,
    const NativeWebCrawler* crawler = nullptr,
    BrowserRenderer* browser = nullptr);

struct ChunkDraft {
  std::string content;
  std::size_t chunk_index = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  Value metadata = Value::object({});
};

class KnowledgeChunker {
 public:
  virtual ~KnowledgeChunker() = default;
  [[nodiscard]] virtual std::vector<ChunkDraft> chunk(const LoadedKnowledgeDocument& document) const = 0;
};

class RecursiveTextChunker : public KnowledgeChunker {
 public:
  RecursiveTextChunker(std::size_t chunk_size = 1200, std::size_t chunk_overlap = 180,
                       std::size_t code_chunk_lines = 80, std::size_t code_chunk_overlap_lines = 12);
  [[nodiscard]] std::vector<ChunkDraft> chunk(const LoadedKnowledgeDocument& document) const override;

 private:
  std::size_t chunk_size_;
  std::size_t chunk_overlap_;
  std::size_t code_chunk_lines_;
  std::size_t code_chunk_overlap_lines_;
};

class KnowledgeChunkerRegistry;

struct KnowledgeChunkerProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeChunker>(
      const Value& options,
      const KnowledgeChunkerRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeChunkerRegistry {
 public:
  explicit KnowledgeChunkerRegistry(std::vector<KnowledgeChunkerProvider> providers = {});
  KnowledgeChunkerRegistry(const KnowledgeChunkerRegistry& other);
  KnowledgeChunkerRegistry& operator=(const KnowledgeChunkerRegistry& other);
  KnowledgeChunkerRegistry(KnowledgeChunkerRegistry&& other) noexcept;
  KnowledgeChunkerRegistry& operator=(KnowledgeChunkerRegistry&& other) noexcept;
  KnowledgeChunkerProvider& register_provider(KnowledgeChunkerProvider provider);
  [[nodiscard]] const KnowledgeChunkerProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeChunker> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeChunkerProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeChunkerProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeChunkerRegistry create_default_knowledge_chunker_registry();

struct KnowledgeDocumentRecord {
  std::string id;
  std::string knowledge_base_id;
  std::string tenant_id;
  std::string source_type;
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::string uri;
  std::string title;
  std::string content;
  std::optional<MediaSource> media;
  std::string text_hint;
  Value metadata = Value::object({});
  std::string created_at;
  std::string updated_at;
};

struct KnowledgeChunkRecord {
  std::string id;
  std::string document_id;
  std::string knowledge_base_id;
  std::string tenant_id;
  std::string source_type;
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::string uri;
  std::string title;
  std::string content;
  std::size_t chunk_index = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  std::optional<MediaSource> media;
  std::string embedding_space_id;
  Value metadata = Value::object({});
  EmbeddingVector embedding;
  std::string created_at;
  std::string updated_at;
};

struct KnowledgeCitation {
  std::string knowledge_base_id;
  std::string knowledge_base_title;
  std::string tenant_id;
  std::string document_id;
  std::string chunk_id;
  std::string source_type;
  KnowledgeAssetType asset_type = KnowledgeAssetType::Text;
  std::string uri;
  std::string title;
  std::size_t chunk_index = 0;
  std::size_t start_offset = 0;
  std::size_t end_offset = 0;
  std::size_t line_start = 0;
  std::size_t line_end = 0;
  double score = 0;
  double vector_score = 0;
  double lexical_score = 0;
  std::optional<MediaSource> media;
  std::string embedding_space_id;
  Value metadata = Value::object({});
  std::string snippet;
};

struct KnowledgeSearchHit {
  KnowledgeDocumentRecord document;
  KnowledgeChunkRecord chunk;
  double score = 0;
  double vector_score = 0;
  double lexical_score = 0;
  std::optional<double> rerank_score;
  KnowledgeCitation citation;
};

struct KnowledgeSearchResult {
  std::vector<KnowledgeSearchHit> hits;
  Value debug = Value::object({});
};

struct KnowledgeContextResult {
  std::vector<KnowledgeSearchHit> hits;
  std::optional<AgentMessage> message;
  Value debug = Value::object({});
};

Value knowledge_citation_to_value(const KnowledgeCitation& citation);
Value knowledge_search_hit_to_value(const KnowledgeSearchHit& hit);

struct KnowledgeSearchOptions {
  std::size_t top_k = 0;
  std::size_t vector_top_k = 0;
  std::size_t lexical_top_k = 0;
  double min_score = std::numeric_limits<double>::quiet_NaN();
  double hybrid_alpha = std::numeric_limits<double>::quiet_NaN();
  std::size_t rerank_top_k = 0;
  std::string retrieval_mode;
  double oversample_factor = 0.0;
  std::string fusion;
  std::map<KnowledgeAssetType, double> modality_weights;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<KnowledgeAssetType> asset_types;
  std::string space_id;
  std::vector<std::string> source_types;
  std::vector<std::string> chunk_ids;
  Value metadata = Value::object({});
  CancellationToken* cancellation = nullptr;
};

KnowledgeSearchOptions default_knowledge_search_options();
KnowledgeSearchOptions merge_knowledge_search_options(const KnowledgeSearchOptions& base,
                                                      const KnowledgeSearchOptions& override);
KnowledgeSearchOptions effective_knowledge_search_options(const KnowledgeSearchOptions& search_defaults,
                                                          const KnowledgeSearchOptions& options);

struct KnowledgeDeleteOptions {
  std::vector<std::string> document_ids;
  std::vector<std::string> uris;
  std::string uri_prefix;
  std::vector<std::string> source_types;
  Value metadata = Value::object({});
};

struct KnowledgeDeleteResult {
  std::string knowledge_base_id;
  std::string tenant_id;
  std::size_t document_count = 0;
  std::size_t chunk_count = 0;
  std::vector<std::string> document_ids;
  std::vector<std::string> chunk_ids;
};

struct KnowledgeTextMatch {
  std::string chunk_id;
  double score = 0;
};

struct KnowledgeVectorMatch {
  std::string chunk_id;
  double score = 0;
};

struct KnowledgeTextSearchOptions {
  std::string knowledge_base_id;
  std::string tenant_id = "default";
  std::string query;
  std::size_t top_k = 12;
  std::vector<std::string> source_types;
  std::vector<KnowledgeAssetType> asset_types;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<std::string> chunk_ids;
  std::string space_id;
  Value metadata = Value::object({});
};

struct KnowledgeTextIndexClearOptions {
  std::string knowledge_base_id;
  std::string tenant_id;
};

struct KnowledgeTextIndexStats {
  std::size_t chunk_count = 0;
};

struct KnowledgeVectorSearchOptions {
  std::string knowledge_base_id;
  std::string tenant_id = "default";
  EmbeddingVector embedding;
  std::size_t top_k = 12;
  double min_score = 0.0;
  std::vector<std::string> source_types;
  std::vector<KnowledgeAssetType> asset_types;
  std::string uri_prefix;
  std::vector<std::string> document_ids;
  std::vector<std::string> chunk_ids;
  std::string space_id;
  Value metadata = Value::object({});
};

struct KnowledgeVectorIndexClearOptions {
  std::string knowledge_base_id;
  std::string tenant_id;
};

struct KnowledgeVectorIndexStats {
  std::size_t chunk_count = 0;
  std::string namespace_id;
  std::map<std::string, std::size_t> asset_type_counts;
};

class KnowledgeTextIndex {
 public:
  virtual ~KnowledgeTextIndex() = default;
  virtual void upsert(const std::vector<KnowledgeChunkRecord>& chunks) = 0;
  [[nodiscard]] virtual std::vector<KnowledgeTextMatch> search(const KnowledgeTextSearchOptions& options) const = 0;
  virtual void delete_chunks(const std::vector<std::string>& chunk_ids) = 0;
  virtual void clear(const KnowledgeTextIndexClearOptions& options = {}) = 0;
  [[nodiscard]] virtual KnowledgeTextIndexStats stats() const = 0;
};

class InMemoryKnowledgeTextIndex : public KnowledgeTextIndex {
 public:
  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeTextMatch> search(const KnowledgeTextSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeTextIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeTextIndexStats stats() const override;

 protected:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeChunkRecord> chunks_;
};

class MiniSearchKnowledgeTextIndex : public InMemoryKnowledgeTextIndex {
 public:
  [[nodiscard]] std::vector<KnowledgeTextMatch> search(const KnowledgeTextSearchOptions& options) const override;
};

class KnowledgeTextIndexRegistry;

struct KnowledgeTextIndexProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeTextIndex>(
      const Value& options,
      const KnowledgeTextIndexRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeTextIndexRegistry {
 public:
  using Factory = std::function<std::shared_ptr<KnowledgeTextIndex>()>;

  explicit KnowledgeTextIndexRegistry(std::map<std::string, Factory> factories = {});
  explicit KnowledgeTextIndexRegistry(std::vector<KnowledgeTextIndexProvider> providers);
  KnowledgeTextIndexRegistry(const KnowledgeTextIndexRegistry& other);
  KnowledgeTextIndexRegistry& operator=(const KnowledgeTextIndexRegistry& other);
  KnowledgeTextIndexRegistry(KnowledgeTextIndexRegistry&& other) noexcept;
  KnowledgeTextIndexRegistry& operator=(KnowledgeTextIndexRegistry&& other) noexcept;
  KnowledgeTextIndexProvider& register_provider(KnowledgeTextIndexProvider provider);
  void register_index(std::string name, Factory factory);
  [[nodiscard]] const KnowledgeTextIndexProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeTextIndex> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeTextIndexProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeTextIndexProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeTextIndexRegistry create_default_knowledge_text_index_registry();

class KnowledgeVectorIndex {
 public:
  virtual ~KnowledgeVectorIndex() = default;
  virtual void upsert(const std::vector<KnowledgeChunkRecord>& chunks) = 0;
  [[nodiscard]] virtual std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const = 0;
  virtual void delete_chunks(const std::vector<std::string>& chunk_ids) = 0;
  virtual void clear(const KnowledgeVectorIndexClearOptions& options = {}) = 0;
  [[nodiscard]] virtual KnowledgeVectorIndexStats stats() const = 0;
};

class InMemoryKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 protected:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeChunkRecord> chunks_;
};

struct SqliteKnowledgeVectorIndexConfig {
  std::filesystem::path file_path;
  int dimensions = 0;
  std::string table_name = "knowledge_vectors";
  bool create_table = true;
  std::string namespace_id;
  std::size_t batch_size = 200;
};

class SqliteKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  explicit SqliteKnowledgeVectorIndex(SqliteKnowledgeVectorIndexConfig config);
  ~SqliteKnowledgeVectorIndex() override = default;

  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 private:
  struct StoredChunk {
    std::string namespace_id;
    KnowledgeChunkRecord chunk;
  };

  void load() const;
  void save() const;

  SqliteKnowledgeVectorIndexConfig config_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::map<std::string, StoredChunk> chunks_;
};

struct QdrantKnowledgeVectorIndexConfig {
  std::string base_url;
  std::string collection;
  std::string api_key;
  std::map<std::string, std::string> headers;
  std::string vector_name;
  bool create_collection = false;
  std::string distance = "Cosine";
  int dimensions = 0;
  bool wait = true;
  std::size_t oversample_factor = 4;
  HttpTransport transport;
};

class QdrantKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  explicit QdrantKnowledgeVectorIndex(QdrantKnowledgeVectorIndexConfig config);

  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 private:
  [[nodiscard]] std::string path(std::string suffix) const;
  [[nodiscard]] std::map<std::string, std::string> headers() const;
  void ensure_collection(int dimensions = 0) const;
  [[nodiscard]] Value vector_payload(const EmbeddingVector& embedding) const;

  QdrantKnowledgeVectorIndexConfig config_;
  mutable std::mutex collection_mutex_;
  mutable bool collection_ready_ = false;
};

struct PgVectorQuery {
  std::string sql;
  std::vector<Value> params;
};

struct PgVectorQueryResult {
  std::vector<std::map<std::string, Value>> rows;
};

using PgVectorQueryClient = std::function<PgVectorQueryResult(const PgVectorQuery&)>;

struct PgVectorKnowledgeVectorIndexConfig {
  PgVectorQueryClient client;
  std::string schema_name = "public";
  std::string table_name = "node_agent_knowledge_chunks";
  bool create_table = false;
  int dimensions = 0;
};

class PgVectorKnowledgeVectorIndex : public KnowledgeVectorIndex {
 public:
  explicit PgVectorKnowledgeVectorIndex(PgVectorKnowledgeVectorIndexConfig config);

  void upsert(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeVectorMatch> search(
      const KnowledgeVectorSearchOptions& options) const override;
  void delete_chunks(const std::vector<std::string>& chunk_ids) override;
  void clear(const KnowledgeVectorIndexClearOptions& options = {}) override;
  [[nodiscard]] KnowledgeVectorIndexStats stats() const override;

 private:
  [[nodiscard]] std::string qualified_table() const;
  void ensure_ready(int dimensions = 0) const;
  [[nodiscard]] PgVectorQueryResult query(std::string sql, std::vector<Value> params = {}) const;

  PgVectorKnowledgeVectorIndexConfig config_;
  mutable std::mutex ready_mutex_;
  mutable bool ready_ = false;
};

class KnowledgeVectorIndexRegistry;

struct KnowledgeVectorIndexProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeVectorIndex>(
      const Value& options,
      const KnowledgeVectorIndexRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeVectorIndexRegistry {
 public:
  using Factory = std::function<std::shared_ptr<KnowledgeVectorIndex>()>;

  explicit KnowledgeVectorIndexRegistry(std::map<std::string, Factory> factories = {});
  explicit KnowledgeVectorIndexRegistry(std::vector<KnowledgeVectorIndexProvider> providers);
  KnowledgeVectorIndexRegistry(const KnowledgeVectorIndexRegistry& other);
  KnowledgeVectorIndexRegistry& operator=(const KnowledgeVectorIndexRegistry& other);
  KnowledgeVectorIndexRegistry(KnowledgeVectorIndexRegistry&& other) noexcept;
  KnowledgeVectorIndexRegistry& operator=(KnowledgeVectorIndexRegistry&& other) noexcept;
  KnowledgeVectorIndexProvider& register_provider(KnowledgeVectorIndexProvider provider);
  void register_index(std::string name, Factory factory);
  [[nodiscard]] const KnowledgeVectorIndexProvider* get(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeVectorIndex> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::vector<KnowledgeVectorIndexProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeVectorIndexProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeVectorIndexRegistry create_default_knowledge_vector_index_registry();

class KnowledgeReranker {
 public:
  virtual ~KnowledgeReranker() = default;
  [[nodiscard]] virtual std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const = 0;
};

class HeuristicKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

class BasicKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

class OverlapKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

struct HybridKnowledgeRerankerConfig {
  double vector_weight = 0.55;
  double lexical_weight = 0.35;
  double image_asset_bias = 0.05;
  double ocr_boost = 0.05;
};

class HybridKnowledgeReranker : public KnowledgeReranker {
 public:
  explicit HybridKnowledgeReranker(HybridKnowledgeRerankerConfig config = HybridKnowledgeRerankerConfig{});
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;

 private:
  HybridKnowledgeRerankerConfig config_;
};

class RecencyKnowledgeReranker : public KnowledgeReranker {
 public:
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;
};

class MmrKnowledgeReranker : public KnowledgeReranker {
 public:
  explicit MmrKnowledgeReranker(double lambda = 0.75);
  [[nodiscard]] std::vector<KnowledgeSearchHit> rerank(
      const std::string& query,
      std::vector<KnowledgeSearchHit> hits,
      std::size_t top_k) const override;

 private:
  double lambda_ = 0.75;
};

class KnowledgeRerankerRegistry;

struct KnowledgeRerankerProvider {
  using Factory = std::function<std::shared_ptr<KnowledgeReranker>(
      const Value& options,
      const KnowledgeRerankerRegistry& registry)>;

  KnowledgeProviderMetadata metadata;
  Factory create;
};

class KnowledgeRerankerRegistry {
 public:
  explicit KnowledgeRerankerRegistry(
      std::map<std::string, std::shared_ptr<KnowledgeReranker>> rerankers = {});
  explicit KnowledgeRerankerRegistry(std::vector<KnowledgeRerankerProvider> providers);
  KnowledgeRerankerRegistry(const KnowledgeRerankerRegistry& other);
  KnowledgeRerankerRegistry& operator=(const KnowledgeRerankerRegistry& other);
  KnowledgeRerankerRegistry(KnowledgeRerankerRegistry&& other) noexcept;
  KnowledgeRerankerRegistry& operator=(KnowledgeRerankerRegistry&& other) noexcept;
  KnowledgeRerankerProvider& register_provider(KnowledgeRerankerProvider provider);
  void register_reranker(std::string name, std::shared_ptr<KnowledgeReranker> reranker);
  [[nodiscard]] const KnowledgeRerankerProvider* provider(const std::string& name) const;
  [[nodiscard]] std::shared_ptr<KnowledgeReranker> create(
      const std::string& name,
      const Value& options = Value::object({})) const;
  [[nodiscard]] std::shared_ptr<KnowledgeReranker> get(const std::string& name) const;
  [[nodiscard]] std::vector<KnowledgeRerankerProvider> list() const;
  [[nodiscard]] std::vector<std::string> list_names() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, KnowledgeRerankerProvider> providers_;
  std::vector<std::string> provider_order_;
};

KnowledgeRerankerRegistry create_default_knowledge_reranker_registry();

struct KnowledgeIngestionResult {
  std::string knowledge_base_id;
  std::string tenant_id;
  std::size_t document_count = 0;
  std::size_t chunk_count = 0;
  std::vector<KnowledgeDocumentRecord> documents;
  std::vector<KnowledgeChunkRecord> chunks;
};

struct KnowledgeIngestOptions {
  bool replace_existing = true;
  bool skip_if_unchanged = true;
  std::size_t embedding_batch_size = 0;
  CancellationToken* cancellation = nullptr;
};

class KnowledgeBase;

struct KnowledgeIngestionStrategyContext {
  KnowledgeBase& knowledge_base;
  const std::vector<Value>& sources;
  const KnowledgeSourceLoader& loader;
  KnowledgeIngestOptions options;
};

class KnowledgeIngestionStrategy {
 public:
  virtual ~KnowledgeIngestionStrategy() = default;
  [[nodiscard]] virtual KnowledgeIngestionResult ingest(
      const KnowledgeIngestionStrategyContext& context) const = 0;
};

class DefaultKnowledgeIngestionStrategy : public KnowledgeIngestionStrategy {
 public:
  [[nodiscard]] KnowledgeIngestionResult ingest(
      const KnowledgeIngestionStrategyContext& context) const override;
};

class KnowledgeRetrievalStrategy {
 public:
  virtual ~KnowledgeRetrievalStrategy() = default;
  [[nodiscard]] virtual KnowledgeSearchResult search(
      const KnowledgeBase& knowledge_base,
      const std::string& query,
      KnowledgeSearchOptions options) const = 0;
  [[nodiscard]] virtual KnowledgeSearchResult search(
      const KnowledgeBase& knowledge_base,
      const ImageEmbeddingInput& query,
      KnowledgeSearchOptions options) const = 0;
};

struct KnowledgeContextRenderContext {
  const KnowledgeBase& knowledge_base;
  const std::vector<KnowledgeSearchHit>& hits;
};

class KnowledgeContextRenderer {
 public:
  virtual ~KnowledgeContextRenderer() = default;
  [[nodiscard]] virtual std::optional<AgentMessage> render(
      const KnowledgeContextRenderContext& context) const = 0;
};

class DefaultKnowledgeContextRenderer : public KnowledgeContextRenderer {
 public:
  [[nodiscard]] std::optional<AgentMessage> render(
      const KnowledgeContextRenderContext& context) const override;
};

std::string hash_knowledge_content(const std::string& content);
std::vector<LoadedKnowledgeDocument> dedupe_loaded_knowledge_documents(
    const std::vector<LoadedKnowledgeDocument>& documents,
    bool replace_existing = true);

struct KnowledgeStoreStats {
  std::size_t document_count = 0;
  std::size_t chunk_count = 0;
};

class KnowledgeStore {
 public:
  virtual ~KnowledgeStore() = default;
  virtual std::vector<KnowledgeDocumentRecord> upsert_documents(
      const std::vector<KnowledgeDocumentRecord>& documents) = 0;
  virtual std::vector<KnowledgeChunkRecord> upsert_chunks(
      const std::vector<KnowledgeChunkRecord>& chunks) = 0;
  [[nodiscard]] virtual std::vector<KnowledgeDocumentRecord> list_documents() const = 0;
  [[nodiscard]] virtual std::vector<KnowledgeChunkRecord> list_chunks() const = 0;
  virtual std::size_t delete_documents(const std::vector<std::string>& ids) = 0;
  virtual std::size_t delete_chunks(const std::vector<std::string>& ids) = 0;
  virtual void clear() = 0;
  [[nodiscard]] virtual KnowledgeStoreStats stats() const = 0;
};

class InMemoryKnowledgeStore : public KnowledgeStore {
 public:
  std::vector<KnowledgeDocumentRecord> upsert_documents(
      const std::vector<KnowledgeDocumentRecord>& documents) override;
  std::vector<KnowledgeChunkRecord> upsert_chunks(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeDocumentRecord> list_documents() const override;
  [[nodiscard]] std::vector<KnowledgeChunkRecord> list_chunks() const override;
  std::size_t delete_documents(const std::vector<std::string>& ids) override;
  std::size_t delete_chunks(const std::vector<std::string>& ids) override;
  void clear() override;
  [[nodiscard]] KnowledgeStoreStats stats() const override;

 protected:
  mutable std::recursive_mutex mutex_;
  mutable std::map<std::string, KnowledgeDocumentRecord> documents_;
  mutable std::map<std::string, KnowledgeChunkRecord> chunks_;
};

class FileKnowledgeStore : public InMemoryKnowledgeStore {
 public:
  explicit FileKnowledgeStore(std::filesystem::path file_path);

  std::vector<KnowledgeDocumentRecord> upsert_documents(
      const std::vector<KnowledgeDocumentRecord>& documents) override;
  std::vector<KnowledgeChunkRecord> upsert_chunks(const std::vector<KnowledgeChunkRecord>& chunks) override;
  [[nodiscard]] std::vector<KnowledgeDocumentRecord> list_documents() const override;
  [[nodiscard]] std::vector<KnowledgeChunkRecord> list_chunks() const override;
  std::size_t delete_documents(const std::vector<std::string>& ids) override;
  std::size_t delete_chunks(const std::vector<std::string>& ids) override;
  void clear() override;
  [[nodiscard]] KnowledgeStoreStats stats() const override;
  [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

 private:
  void ensure_loaded() const;
  void persist() const;

  std::filesystem::path file_path_;
  mutable bool loaded_ = false;
};

class KnowledgeBase {
 public:
  KnowledgeBase(std::string id, std::string tenant_id = "default", std::string title = {},
                std::shared_ptr<TextEmbeddingAdapter> embedder = std::make_shared<HashEmbeddingAdapter>(),
                RecursiveTextChunker chunker = {},
                std::shared_ptr<KnowledgeReranker> reranker = std::make_shared<HeuristicKnowledgeReranker>(),
                std::shared_ptr<KnowledgeStore> store = std::make_shared<InMemoryKnowledgeStore>(),
                std::shared_ptr<ImageEmbeddingAdapter> image_embedder =
                    std::make_shared<HashImageEmbeddingAdapter>(),
                std::shared_ptr<KnowledgeTextIndex> text_index = std::make_shared<InMemoryKnowledgeTextIndex>(),
                std::shared_ptr<KnowledgeVectorIndex> vector_index = {},
                KnowledgeSearchOptions search_defaults = {},
                std::string description = {},
                std::string context_title = {},
                std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy = {},
                std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy = {},
                std::shared_ptr<KnowledgeContextRenderer> context_renderer = {});

  KnowledgeIngestionResult ingest_loaded_documents(const std::vector<LoadedKnowledgeDocument>& documents,
                                                   KnowledgeIngestOptions options = {});
  KnowledgeIngestionResult ingest_loaded_documents(const std::vector<LoadedKnowledgeDocument>& documents,
                                                   bool replace_existing);
  KnowledgeIngestionResult ingest(const std::vector<Value>& sources, const KnowledgeSourceLoader& loader,
                                  KnowledgeIngestOptions options = {});
  KnowledgeDeleteResult delete_documents(const KnowledgeDeleteOptions& options);
  std::vector<KnowledgeSearchHit> search(const std::string& query, KnowledgeSearchOptions options = {});
  std::vector<KnowledgeSearchHit> search(const ImageEmbeddingInput& query, KnowledgeSearchOptions options = {});
  KnowledgeSearchResult search_with_debug(const std::string& query, KnowledgeSearchOptions options = {});
  KnowledgeSearchResult search_with_debug(const ImageEmbeddingInput& query, KnowledgeSearchOptions options = {});
  std::optional<AgentMessage> create_context_message(const std::vector<KnowledgeSearchHit>& hits) const;
  KnowledgeContextResult build_context_message(const std::string& query, KnowledgeSearchOptions options = {});
  KnowledgeContextResult build_context_message(const ImageEmbeddingInput& query, KnowledgeSearchOptions options = {});
  [[nodiscard]] KnowledgeStoreStats stats() const;
  [[nodiscard]] std::shared_ptr<KnowledgeStore> store() const noexcept;

  [[nodiscard]] const std::string& id() const noexcept;
  [[nodiscard]] const std::string& tenant_id() const noexcept;
  [[nodiscard]] const std::string& title() const noexcept;
  [[nodiscard]] const std::string& description() const noexcept;
 [[nodiscard]] const std::string& context_title() const noexcept;

 private:
  mutable std::recursive_mutex mutex_;
  std::string id_;
  std::string tenant_id_;
  std::string title_;
  std::string description_;
  std::string context_title_;
  std::shared_ptr<TextEmbeddingAdapter> embedder_;
  std::shared_ptr<ImageEmbeddingAdapter> image_embedder_;
  RecursiveTextChunker chunker_;
  std::shared_ptr<KnowledgeReranker> reranker_;
  std::shared_ptr<KnowledgeStore> store_;
  std::shared_ptr<KnowledgeTextIndex> text_index_;
  std::shared_ptr<KnowledgeVectorIndex> vector_index_;
  std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy_;
  std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy_;
  std::shared_ptr<KnowledgeContextRenderer> context_renderer_;
  KnowledgeSearchOptions search_defaults_;
  bool text_index_ready_ = false;
  bool vector_index_ready_ = false;

  std::vector<KnowledgeSearchHit> search_with_embedding(const std::string& query_text,
                                                        const EmbeddingVector& query_vector,
                                                        KnowledgeSearchOptions options,
                                                        Value* debug);
  void rebuild_text_index();
  void ensure_text_index_ready();
  void rebuild_vector_index();
  void ensure_vector_index_ready();
};

struct KnowledgeIngestionProgress {
  std::string phase;
  std::size_t source_count = 0;
  std::size_t loaded_document_count = 0;
  std::size_t processed_document_count = 0;
  std::size_t processed_chunk_count = 0;
  std::size_t batch_index = 0;
  std::size_t total_batches = 0;
};

using KnowledgeIngestionProgressCallback = std::function<void(const KnowledgeIngestionProgress&)>;

struct KnowledgeDedupeContext {
  const KnowledgeBase& knowledge_base;
  const std::vector<Value>& sources;
  const std::vector<LoadedKnowledgeDocument>& loaded_documents;
  bool replace_existing = true;
};

struct KnowledgeIncrementalContext {
  const KnowledgeBase& knowledge_base;
  const std::vector<Value>& sources;
  const std::vector<LoadedKnowledgeDocument>& loaded_documents;
  const std::vector<LoadedKnowledgeDocument>& documents;
  bool replace_existing = true;
  bool skip_if_unchanged = true;
};

struct KnowledgeIncrementalResult {
  std::vector<LoadedKnowledgeDocument> documents;
  bool replace_existing = true;
  bool skip_if_unchanged = true;
};

class KnowledgeDedupeStrategy {
 public:
  virtual ~KnowledgeDedupeStrategy() = default;
  [[nodiscard]] virtual std::vector<LoadedKnowledgeDocument> dedupe(
      const KnowledgeDedupeContext& context) const = 0;
};

class UriContentHashDedupeStrategy : public KnowledgeDedupeStrategy {
 public:
  [[nodiscard]] std::vector<LoadedKnowledgeDocument> dedupe(
      const KnowledgeDedupeContext& context) const override;
};

class KnowledgeIncrementalStrategy {
 public:
  virtual ~KnowledgeIncrementalStrategy() = default;
  [[nodiscard]] virtual KnowledgeIncrementalResult prepare(
      const KnowledgeIncrementalContext& context) const = 0;
};

class SkipUnchangedKnowledgeIncrementalStrategy : public KnowledgeIncrementalStrategy {
 public:
  [[nodiscard]] KnowledgeIncrementalResult prepare(
      const KnowledgeIncrementalContext& context) const override;
};

struct KnowledgeRetryContext {
  const KnowledgeBase& knowledge_base;
  std::string phase;
  std::size_t batch_index = 0;
  std::size_t total_batches = 0;
};

class KnowledgeRetryStrategy {
 public:
  virtual ~KnowledgeRetryStrategy() = default;
  [[nodiscard]] virtual std::size_t max_attempts(const KnowledgeRetryContext& context) const = 0;
  [[nodiscard]] virtual bool should_retry(const std::exception& error,
                                          const KnowledgeRetryContext& context,
                                          std::size_t attempt,
                                          std::size_t max_attempts) const = 0;
  [[nodiscard]] virtual int retry_delay_ms(const std::exception& error,
                                           const KnowledgeRetryContext& context,
                                           std::size_t attempt) const = 0;
};

class FixedKnowledgeRetryStrategy : public KnowledgeRetryStrategy {
 public:
  explicit FixedKnowledgeRetryStrategy(std::size_t max_attempts = 2, int delay_ms = 0);
  [[nodiscard]] std::size_t max_attempts(const KnowledgeRetryContext& context) const override;
  [[nodiscard]] bool should_retry(const std::exception& error,
                                  const KnowledgeRetryContext& context,
                                  std::size_t attempt,
                                  std::size_t max_attempts) const override;
  [[nodiscard]] int retry_delay_ms(const std::exception& error,
                                   const KnowledgeRetryContext& context,
                                   std::size_t attempt) const override;

 private:
  std::size_t max_attempts_;
  int delay_ms_;
};

struct KnowledgeIngestionPipelineOptions {
  std::size_t document_batch_size = 16;
  std::size_t embedding_batch_size = 0;
  bool replace_existing = true;
  bool skip_if_unchanged = true;
  std::size_t max_attempts = 2;
  int retry_delay_ms = 0;
  std::shared_ptr<KnowledgeDedupeStrategy> dedupe_strategy;
  std::shared_ptr<KnowledgeIncrementalStrategy> incremental_strategy;
  std::shared_ptr<KnowledgeRetryStrategy> retry_strategy;
  KnowledgeIngestionProgressCallback on_progress;
  CancellationToken* cancellation = nullptr;
};

class KnowledgeIngestionPipeline {
 public:
  KnowledgeIngestionPipeline(KnowledgeBase& knowledge_base, const KnowledgeSourceLoader& loader);
  KnowledgeIngestionResult ingest(const std::vector<Value>& sources,
                                  KnowledgeIngestionPipelineOptions options = {}) const;

 private:
  KnowledgeBase& knowledge_base_;
  const KnowledgeSourceLoader& loader_;
};

struct KnowledgeSyncState {
  std::map<std::string, std::string> document_hashes;
  std::string synced_at;
};

class KnowledgeSyncStateStore {
 public:
  virtual ~KnowledgeSyncStateStore() = default;
  [[nodiscard]] virtual KnowledgeSyncState load() const = 0;
  virtual void save(const KnowledgeSyncState& state) = 0;
};

class InMemoryKnowledgeSyncStateStore : public KnowledgeSyncStateStore {
 public:
  [[nodiscard]] KnowledgeSyncState load() const override;
  void save(const KnowledgeSyncState& state) override;

 private:
  mutable std::mutex mutex_;
  KnowledgeSyncState state_;
};

class FileKnowledgeSyncStateStore : public KnowledgeSyncStateStore {
 public:
  explicit FileKnowledgeSyncStateStore(std::filesystem::path file_path);
  [[nodiscard]] KnowledgeSyncState load() const override;
  void save(const KnowledgeSyncState& state) override;
  [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

 private:
  std::filesystem::path file_path_;
  mutable std::mutex mutex_;
};

struct KnowledgeSyncOptions : KnowledgeIngestionPipelineOptions {
  bool delete_missing = false;
};

struct KnowledgeSyncResult : KnowledgeIngestionResult {
  std::size_t loaded_document_count = 0;
  std::size_t changed_document_count = 0;
  std::size_t skipped_document_count = 0;
  std::size_t deleted_document_count = 0;
  std::vector<std::string> deleted_uris;
  std::string synced_at;
};

class KnowledgeSyncJob {
 public:
  KnowledgeSyncJob(KnowledgeBase& knowledge_base,
                   const KnowledgeSourceLoader& loader,
                   KnowledgeSyncStateStore& state_store);
  KnowledgeSyncResult sync(const std::vector<Value>& sources, KnowledgeSyncOptions options = {}) const;

 private:
  KnowledgeBase& knowledge_base_;
  const KnowledgeSourceLoader& loader_;
  KnowledgeSyncStateStore& state_store_;
};

struct KnowledgeBaseDefinition {
  std::string id;
  std::string tenant_id = "default";
  std::string title;
  std::string description;
  std::string store_file_path;
};

struct ManagedKnowledgeBaseConfig {
  std::string id;
  std::string tenant_id = "default";
  std::string title;
  std::string description;
  std::optional<bool> persistent;
  std::shared_ptr<KnowledgeStore> store;
  std::shared_ptr<TextEmbeddingAdapter> embedder;
  std::shared_ptr<ImageEmbeddingAdapter> image_embedder;
  RecursiveTextChunker chunker = {};
  std::shared_ptr<KnowledgeReranker> reranker;
  std::shared_ptr<KnowledgeTextIndex> text_index;
  std::shared_ptr<KnowledgeVectorIndex> vector_index;
  std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy;
  std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy;
  std::shared_ptr<KnowledgeContextRenderer> context_renderer;
  KnowledgeSearchOptions search_defaults;
  KnowledgeSearchOptions retrieval_config;
  std::string context_title;
};

struct ManagerSearchOptions : KnowledgeSearchOptions {
  std::string tenant_id;
  std::vector<std::string> knowledge_base_ids;
  bool enabled = true;
};

struct KnowledgeManagerSearchResult {
  std::vector<KnowledgeSearchHit> hits;
  Value debug = Value::object({});
};

struct KnowledgeManagerContextResult : KnowledgeContextResult {};

class KnowledgeBaseManager {
 public:
  explicit KnowledgeBaseManager(std::filesystem::path base_dir = {},
                                std::shared_ptr<TextEmbeddingAdapter> embedder =
                                    std::make_shared<HashEmbeddingAdapter>(),
                                std::shared_ptr<ImageEmbeddingAdapter> image_embedder =
                                    std::make_shared<HashImageEmbeddingAdapter>(),
                                std::shared_ptr<KnowledgeSourceLoader> loader =
                                    std::make_shared<CompositeKnowledgeLoader>(),
                                std::shared_ptr<KnowledgeReranker> reranker =
                                    std::make_shared<HeuristicKnowledgeReranker>(),
                                std::shared_ptr<KnowledgeTextIndex> text_index =
                                    std::make_shared<InMemoryKnowledgeTextIndex>(),
                                std::shared_ptr<KnowledgeVectorIndex> vector_index = {},
                                RecursiveTextChunker chunker = {});

  [[nodiscard]] std::filesystem::path manifest_path() const;
  [[nodiscard]] std::vector<KnowledgeBaseDefinition> load_definitions();
  [[nodiscard]] std::vector<KnowledgeBaseDefinition> list_knowledge_bases(
      std::string tenant_id = {});
  std::shared_ptr<KnowledgeBase> create_knowledge_base(ManagedKnowledgeBaseConfig config);
  std::shared_ptr<KnowledgeBase> register_knowledge_base(std::shared_ptr<KnowledgeBase> base,
                                                         std::string description = {});
  [[nodiscard]] std::shared_ptr<KnowledgeBase> get_knowledge_base(const std::string& id,
                                                                  std::string tenant_id = "default");
  bool delete_knowledge_base(const std::string& id, std::string tenant_id = "default");
  KnowledgeIngestionResult ingest(const std::string& knowledge_base_id,
                                  const std::vector<Value>& sources,
                                  std::string tenant_id = "default",
                                  KnowledgeIngestionPipelineOptions options = {});
  KnowledgeDeleteResult delete_documents(const std::string& knowledge_base_id,
                                         const KnowledgeDeleteOptions& options,
                                         std::string tenant_id = "default");
  KnowledgeManagerSearchResult search_with_debug(const std::string& query,
                                                 ManagerSearchOptions options = {});
  KnowledgeManagerSearchResult search_with_debug(const ImageEmbeddingInput& query,
                                                 ManagerSearchOptions options = {});
  std::vector<KnowledgeSearchHit> search(const std::string& query,
                                         ManagerSearchOptions options = {});
  std::vector<KnowledgeSearchHit> search(const ImageEmbeddingInput& query,
                                         ManagerSearchOptions options = {});
  std::optional<AgentMessage> create_context_message(const std::vector<KnowledgeSearchHit>& hits) const;
  KnowledgeManagerContextResult build_context_message(const std::string& query,
                                                      ManagerSearchOptions options = {});
  KnowledgeManagerContextResult build_context_message(const ImageEmbeddingInput& query,
                                                      ManagerSearchOptions options = {});

 private:
  [[nodiscard]] std::string key(const std::string& tenant_id, const std::string& id) const;
  [[nodiscard]] std::filesystem::path store_file_path(const std::string& tenant_id,
                                                      const std::string& id) const;
  void persist_definitions() const;

  std::filesystem::path base_dir_;
  std::shared_ptr<TextEmbeddingAdapter> embedder_;
  std::shared_ptr<ImageEmbeddingAdapter> image_embedder_;
  std::shared_ptr<KnowledgeSourceLoader> loader_;
  std::shared_ptr<KnowledgeReranker> reranker_;
  std::shared_ptr<KnowledgeTextIndex> text_index_;
  std::shared_ptr<KnowledgeVectorIndex> vector_index_;
  RecursiveTextChunker chunker_;
  mutable std::recursive_mutex mutex_;
  std::map<std::string, std::shared_ptr<KnowledgeBase>> bases_;
  std::map<std::string, KnowledgeBaseDefinition> definitions_;
};

}  // namespace agent
