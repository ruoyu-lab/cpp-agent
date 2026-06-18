#pragma once

#include "agent/model.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>

namespace agent {

class SessionMemory;

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
SessionMemorySnapshot session_memory_snapshot_from_value(const Value& value);

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
// of compact() / add() -- the body may block freely but blocks the caller.
//
// In SummarizerMode::Background the summarizer body runs on whatever thread
// the SessionCompactionExecutor dispatches it onto. The summarizer body
// itself remains synchronous (still a `std::string` return) -- only the
// dispatch is asynchronous. Implementations may therefore block on an LLM
// call without bouncing through a future/coroutine; the framework guarantees
// the body never runs on the agent loop thread when Background mode is set.
using SessionMemorySummarizer = std::function<std::string(const SessionMemorySummaryInput&)>;

enum class LLMSessionSummarizerErrorMode {
  Throw,
  Fallback,
};

std::string to_string(LLMSessionSummarizerErrorMode mode);

enum class LLMSessionSummaryStyle {
  AgentState,
  Conversation,
};

std::string to_string(LLMSessionSummaryStyle style);

struct LLMSessionSummarizerPromptContext {
  LLMSessionSummaryStyle style = LLMSessionSummaryStyle::AgentState;
  std::string system_prompt;
  std::string instructions;
  std::string previous_summary;
  std::string archived_transcript;
  std::size_t omitted_archived_messages = 0;
  std::size_t omitted_archived_characters = 0;
};

using LLMSessionSummaryPromptBuilder =
    std::function<std::vector<AgentMessage>(const SessionMemorySummaryInput&,
                                            const LLMSessionSummarizerPromptContext&)>;
using LLMSessionSummaryResponseParser = std::function<std::string(const AgentOutput&)>;
using LLMSessionMessageFormatter =
    std::function<std::string(const AgentMessage&, std::size_t index)>;

struct LLMSessionSummarizerOptions {
  std::shared_ptr<ChatModelAdapter> model;
  ModelSettings settings;
  CancellationToken* cancellation = nullptr;
  LLMSessionSummaryStyle style = LLMSessionSummaryStyle::AgentState;
  std::string system_prompt;
  std::string instructions;
  std::size_t max_input_chars = 24000;
  std::size_t max_archived_messages = 200;
  std::size_t max_summary_chars = 8000;
  int max_summary_tokens = 2000;
  bool include_previous_summary = true;
  LLMSessionSummarizerErrorMode on_error = LLMSessionSummarizerErrorMode::Throw;
  SessionMemorySummarizer fallback_summarizer;
  LLMSessionMessageFormatter format_message;
  LLMSessionSummaryPromptBuilder build_messages;
  LLMSessionSummaryResponseParser parse_response;
};

std::string format_session_message_for_llm(const AgentMessage& message, std::size_t index);
std::vector<AgentMessage> build_llm_session_summary_messages(
    const SessionMemorySummaryInput& input,
    const LLMSessionSummarizerPromptContext& context);
std::string parse_llm_session_summary_response(const AgentOutput& response);
std::string default_llm_session_summarizer_fallback(
    const SessionMemorySummaryInput& input,
    std::size_t max_summary_chars = 8000);
SessionMemorySummarizer create_llm_session_summarizer(
    LLMSessionSummarizerOptions options);

// Selects how Phase 2 (the LLM-summarizer refinement) is dispatched.
//   Synchronous  - Phase 2 runs on the calling thread.
//   Background   - Phase 1 returns immediately; Phase 2 is dispatched via the
//                  policy's SessionCompactionExecutor.
enum class SummarizerMode {
  Synchronous,
  Background,
};

// Executor that runs Phase 2 summarizer work off the agent loop thread.
using SessionCompactionExecutor = std::function<void(std::function<void()>)>;

// Host-supplied token counter. The framework calls this with the same
// summary-inclusive message projection used by compaction triggers/planning.
using SessionMemoryTokenCounter =
    std::function<std::size_t(const std::vector<AgentMessage>&)>;

struct CompactionBudget {
  std::optional<std::size_t> max_messages;
  std::optional<std::size_t> max_tokens;
  [[nodiscard]] bool empty() const noexcept;
};

struct CompactionPlan {
  std::size_t drop_count = 0;
  std::vector<AgentMessage> overflow;
  std::optional<AgentMessage> truncated_tail_message;
};

struct PlanCompactionInput {
  std::span<const AgentMessage> messages;
  std::string_view summary;
  std::string_view summary_label;
  CompactionBudget budget;
  SessionMemoryTokenCounter token_counter;
  bool truncate_oversized_message = false;
  std::size_t oversized_tail_chars = 4000;
};

CompactionPlan plan_compaction(const PlanCompactionInput& input);

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
  bool single_message_truncated = false;
  std::string warning;
  std::string error;
};
std::string to_string(SessionCompactionEvent::Kind);
using SessionCompactionListener = std::function<void(const SessionCompactionEvent&)>;

struct SessionStorage {
  std::string session_id = "default";
  std::vector<AgentMessage> messages;
  std::string summary;
  std::string summary_label = "Conversation summary";
};

struct SessionCompactionPolicy {
  CompactionBudget compaction_budget;
  double auto_compact_at = 0.8;
  std::size_t token_budget = 0;
  bool compact_on_append = false;
  SessionMemorySummarizer summarizer;
  SessionMemoryTokenCounter token_counter;
  std::size_t summary_max_chars = 8000;
  SummarizerMode summarizer_mode = SummarizerMode::Synchronous;
  SessionCompactionExecutor background_executor;
  bool truncate_oversized_message = false;
  std::size_t oversized_tail_chars = 4000;
};

struct SessionMemoryOptions {
  SessionStorage storage;
  SessionCompactionPolicy compaction;
  SessionMemoryChangeListener on_change;
  SessionCompactionListener on_compaction;
};

std::shared_ptr<SessionMemory> rehydrate_session_memory(const SessionMemorySnapshot& snapshot,
                                                        SessionMemoryOptions options = {});

struct InMemorySessionStoreConfig {
  SessionMemoryOptions session_options = {};
};

struct FileSessionStoreConfig {
  std::filesystem::path base_dir;
  std::string file_extension = ".json";
  SessionMemoryOptions session_options = {};
};

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
  [[nodiscard]] SessionMemoryTokenCounter token_counter() const;
  [[nodiscard]] std::vector<AgentMessage> get_messages(bool include_summary = true) const;
  [[nodiscard]] std::vector<AgentMessage> get_messages(const SessionMemoryGetMessagesOptions& options) const;
  [[nodiscard]] SessionMemorySnapshot snapshot() const;
  SessionMemory& restore(const SessionMemorySnapshot& snapshot);
  SessionMemory& restore(const SessionMemoryRestoreInput& snapshot);
  void clear();
  void await_refinements() const;

 private:
  [[nodiscard]] SessionMemorySnapshot snapshot_unlocked() const;
  void notify_change(const SessionMemorySnapshot& snapshot) const;
  void notify_compaction(const SessionCompactionEvent& event) const;
  bool run_phase1_locked(CompactionPlan& plan,
                         std::size_t& pre_token_count,
                         std::string& snapshot_session_id,
                         bool& single_message_truncated_out,
                         std::string& warning_out);
  void run_refinement_body();
  void dispatch_refinement();
  void recompute_summary_unlocked();
  [[nodiscard]] std::size_t estimated_token_count_unlocked() const;

  mutable std::mutex compaction_pipeline_mutex_;
  mutable std::mutex mutex_;
  mutable std::mutex refinement_mutex_;
  mutable std::condition_variable refinement_cv_;
  std::string session_id_;
  CompactionBudget compaction_budget_;
  std::string summary_label_;
  std::string summary_;
  std::vector<AgentMessage> messages_;
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

}  // namespace agent
