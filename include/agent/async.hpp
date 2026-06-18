#pragma once

#include "agent/run_transcript.hpp"
#include "agent/runtime.hpp"
#include "agent/tasks.hpp"

namespace agent {

inline constexpr const char* kAsyncAgentRunTaskType = "agent.run";
inline constexpr const char* kAsyncAgentRunKindAgent = "agent.run";
inline constexpr const char* kAsyncAgentRunKindSubagent = "subagent.run";

enum class AsyncAgentRunStatus {
  Queued,
  Running,
  Waiting,
  Completed,
  Failed,
  Cancelled,
};

std::string to_string(AsyncAgentRunStatus status);
AsyncAgentRunStatus async_agent_run_status_from_string(
    const std::string& value,
    AsyncAgentRunStatus fallback = AsyncAgentRunStatus::Queued);

struct AsyncRunActivity {
  std::string phase = "queued";
  std::string current_target;
  std::string current_tool_name;
  int current_iteration = -1;
  bool interruptible = true;
  std::string stall_reason;
  long long last_heartbeat_at_ms = 0;
  long long last_activity_at_ms = 0;
};

struct ResourceLedger {
  long long input_tokens = 0;
  long long output_tokens = 0;
  long long total_tokens = 0;
  std::string input_tokens_source;
  std::string output_tokens_source;
  std::string total_tokens_source;
  std::string token_usage_quality;
  long long cached_input_tokens = 0;
  std::string cached_input_tokens_source;
  long long reasoning_tokens = 0;
  std::string reasoning_source;
  std::string provider;
  long long iteration_count = 0;
  long long tool_call_count = 0;
  long long tool_success_count = 0;
  long long tool_error_count = 0;
  long long child_run_count = 0;
  Value details = Value::object({});
};

struct ChildAgentResult {
  int schema_version = 1;
  std::string summary;
  std::string output_text;
  Value evidence = Value::array({});
  Value modified_resources = Value::array({});
  std::vector<std::string> open_questions;
  std::vector<std::string> next_actions;
  std::string run_id;
  std::string kind = kAsyncAgentRunKindAgent;
  AsyncAgentRunStatus status = AsyncAgentRunStatus::Queued;
  std::string root_run_id;
  std::string parent_run_id;
  int depth = 0;
  std::string role = "leaf";
  std::string parent_child_relation;
  std::string spawned_by_tool_call_id;
  std::string text;
  Value output;
  std::string error;
  ResourceLedger resource_ledger;
  Value metadata = Value::object({});
};

struct ChildAgentPolicy {
  // Maximum active child runs across the current async runtime store.
  // Terminal child runs do not consume this capacity.
  std::optional<long long> max_global_child_runs;
  // Maximum cumulative child runs for one parent run.
  // Terminal child runs still count toward this per-parent total.
  std::optional<long long> max_child_runs_per_parent;
  std::optional<long long> max_spawn_depth;
  // Maximum active direct child runs for one parent run.
  std::optional<long long> max_children_per_run;
  std::optional<bool> allow_child_spawn;
  std::optional<std::string> cancel_propagation;
};

struct AsyncAgentRun {
  std::string id;
  std::string type = kAsyncAgentRunTaskType;
  std::string kind = kAsyncAgentRunKindAgent;
  AsyncAgentRunStatus status = AsyncAgentRunStatus::Queued;
  std::string session_id = "default";
  std::string root_run_id;
  std::string parent_run_id;
  int depth = 0;
  std::string role = "leaf";
  std::string parent_child_relation;
  std::string spawned_by_tool_call_id;
  AsyncRunActivity activity;
  ResourceLedger resource_ledger;
  ChildAgentResult result;
  Value input = Value::object({});
  Value output;
  std::string error;
  std::string owner_api_key_id;
  std::string tenant_id;
  std::string idempotency_key;
  long long created_at_ms = 0;
  long long updated_at_ms = 0;
  long long queued_at_ms = 0;
  long long started_at_ms = 0;
  long long completed_at_ms = 0;
  long long cancelled_at_ms = 0;
  Value metadata = Value::object({});
};

struct AsyncAgentRunAttempt {
  std::string id;
  std::string run_id;
  AsyncAgentRunStatus status = AsyncAgentRunStatus::Running;
  int attempt = 1;
  long long started_at_ms = 0;
  long long completed_at_ms = 0;
  long long lease_expires_at_ms = 0;
  long long heartbeat_at_ms = 0;
  Value output;
  std::string error;
};

struct AsyncAgentRunEvent {
  std::string id;
  std::string run_id;
  std::string attempt_id;
  std::string type;
  Value payload = Value::object({});
  long long created_at_ms = 0;
};

struct AsyncAgentRunCheckpoint {
  std::string id;
  std::string run_id;
  std::string attempt_id;
  std::string name;
  Value state = Value::object({});
  long long created_at_ms = 0;
};

struct AsyncAgentRunSnapshot {
  AsyncAgentRun run;
  std::vector<AsyncAgentRunAttempt> attempts;
  std::vector<AsyncAgentRunEvent> events;
  std::vector<AsyncAgentRunCheckpoint> checkpoints;
  std::vector<RunTranscriptEntry> transcript;
};

struct AsyncAgentRunStartInput {
  std::string id;
  std::string session_id = "default";
  Value input;
  ModelSettings model_settings;
  RunnerRetrievalOptions retrieval_options;
  RunnerWritebackOptions writeback_options;
  std::vector<SkillActivation> skill_activations;
  Value context = Value::object({});
  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt;
  bool enable_planning = true;
  std::string kind = kAsyncAgentRunKindAgent;
  std::string root_run_id;
  std::string parent_run_id;
  int depth = -1;
  std::string role;
  std::string parent_child_relation;
  std::string spawned_by_tool_call_id;
  ChildAgentPolicy child_agent_policy;
  std::string idempotency_key;
  std::string owner_api_key_id;
  std::string tenant_id;
  Value metadata = Value::object({});
};

struct AsyncAgentRunFilter {
  std::optional<AsyncAgentRunStatus> status;
  std::string owner_api_key_id;
  std::string tenant_id;
};

struct AsyncAgentRunResumeInput {
  std::string reason = "Async agent run resumed.";
  Value metadata = Value::object({});
  std::optional<AgentRunnerDurableState> resume_state = std::nullopt;
};

struct AsyncAgentRunCancelInput {
  std::string reason = "Async agent run cancelled.";
  Value metadata = Value::object({});
};

class AsyncAgentRunStore {
 public:
  virtual ~AsyncAgentRunStore() = default;
  virtual AsyncAgentRun create(AsyncAgentRunStartInput input) = 0;
  virtual std::optional<AsyncAgentRunSnapshot> get(const std::string& run_id) const = 0;
  virtual std::vector<AsyncAgentRun> list(AsyncAgentRunFilter filter = {}) const = 0;
  virtual AsyncAgentRun update_status(const std::string& run_id,
                                      AsyncAgentRunStatus status,
                                      Value output = {},
                                      std::string error = {}) = 0;
  virtual AsyncAgentRunEvent append_event(const std::string& run_id,
                                          const std::string& attempt_id,
                                          std::string type,
                                          Value payload = Value::object({})) = 0;
  virtual AsyncAgentRunCheckpoint append_checkpoint(const std::string& run_id,
                                                    const std::string& attempt_id,
                                                    std::string name,
                                                    Value state = Value::object({})) = 0;
};

struct TaskBackedAsyncAgentRunStoreConfig {
  InMemoryTaskStore* task_store = nullptr;
  RunTranscript* transcript = nullptr;
};

class TaskBackedAsyncAgentRunStore : public AsyncAgentRunStore {
 public:
  explicit TaskBackedAsyncAgentRunStore(TaskBackedAsyncAgentRunStoreConfig config);
  explicit TaskBackedAsyncAgentRunStore(InMemoryTaskStore& task_store, RunTranscript* transcript = nullptr);

  AsyncAgentRun create(AsyncAgentRunStartInput input) override;
  std::optional<AsyncAgentRunSnapshot> get(const std::string& run_id) const override;
  std::vector<AsyncAgentRun> list(AsyncAgentRunFilter filter = {}) const override;
  AsyncAgentRun update_status(const std::string& run_id,
                              AsyncAgentRunStatus status,
                              Value output = {},
                              std::string error = {}) override;
  AsyncAgentRunEvent append_event(const std::string& run_id,
                                  const std::string& attempt_id,
                                  std::string type,
                                  Value payload = Value::object({})) override;
  AsyncAgentRunCheckpoint append_checkpoint(const std::string& run_id,
                                            const std::string& attempt_id,
                                            std::string name,
                                            Value state = Value::object({})) override;

 private:
  InMemoryTaskStore& task_store_;
  RunTranscript* transcript_ = nullptr;
};

struct AsyncAgentRunWorkerContext {
  AsyncAgentRun run;
  AsyncAgentRunAttempt attempt;
  AsyncAgentRunStore* store = nullptr;
  RunTranscript* transcript = nullptr;
  CancellationToken* cancellation = nullptr;

  AsyncAgentRunEvent event(std::string type, Value payload = Value::object({})) const;
  AsyncAgentRunCheckpoint checkpoint(std::string name, Value state = Value::object({})) const;
  RunTranscriptEntry transcript_entry(std::string kind,
                                      Value payload = Value::object({}),
                                      Value metadata = Value::object({})) const;
};

using AsyncAgentRunnerResolver =
    std::function<std::shared_ptr<AgentRunner>(const AsyncAgentRun&, const Value& run_input)>;

using AsyncAgentRunWorkerStreamObserver =
    std::function<void(const AsyncAgentRunWorkerContext&,
                       const AgentRunnerStreamEvent&,
                       const Value& payload)>;

struct AsyncAgentRunWorkerConfig {
  InMemoryTaskStore* task_store = nullptr;
  InMemoryTaskQueue* queue = nullptr;
  AsyncAgentRunStore* store = nullptr;
  RunTranscript* transcript = nullptr;
  AgentRunner* runner = nullptr;
  AsyncAgentRunnerResolver resolve_runner;
  AsyncAgentRunWorkerStreamObserver on_stream_event;
  int lease_ms = 30000;
};

class AsyncAgentRunWorker {
 public:
  explicit AsyncAgentRunWorker(AsyncAgentRunWorkerConfig config);

  bool run_once();
  bool cancel(const std::string& run_id, std::string reason = "Async agent run cancelled.");

 private:
  [[nodiscard]] std::shared_ptr<AgentRunner> resolve_runner(const AsyncAgentRun& run,
                                                            const Value& input) const;
  [[nodiscard]] bool is_cancelled(const std::string& run_id) const;
  void mark_cancelled(const std::string& run_id, const std::string& reason);
  void clear_cancelled(const std::string& run_id);
  void register_active_token(const std::string& run_id, CancellationToken& token);
  void unregister_active_token(const std::string& run_id, CancellationToken& token);

  InMemoryTaskStore& task_store_;
  InMemoryTaskQueue& queue_;
  AsyncAgentRunStore& store_;
  RunTranscript* transcript_ = nullptr;
  AgentRunner* runner_ = nullptr;
  AsyncAgentRunnerResolver resolve_runner_;
  AsyncAgentRunWorkerStreamObserver on_stream_event_;
  int lease_ms_ = 30000;
  mutable std::mutex cancellation_mutex_;
  std::map<std::string, std::string> cancelled_run_reasons_;
  std::map<std::string, CancellationToken*> active_cancellations_;
};

struct AsyncAgentRunControllerConfig {
  AsyncAgentRunStore* store = nullptr;
  InMemoryTaskQueue* queue = nullptr;
  AsyncAgentRunWorker* worker = nullptr;
  RunTranscript* transcript = nullptr;
  ChildAgentPolicy child_agent_policy;
};

class AsyncAgentRunController {
 public:
  explicit AsyncAgentRunController(AsyncAgentRunControllerConfig config);

  AsyncAgentRunSnapshot start(AsyncAgentRunStartInput input);
  std::optional<AsyncAgentRunSnapshot> get(const std::string& run_id) const;
  std::vector<AsyncAgentRun> list(AsyncAgentRunFilter filter = {}) const;
  std::vector<AsyncAgentRunEvent> events(const std::string& run_id) const;
  std::vector<AsyncAgentRunCheckpoint> checkpoints(const std::string& run_id) const;
  std::vector<RunTranscriptEntry> transcript(const std::string& run_id) const;
  AsyncAgentRunSnapshot resume(const std::string& run_id, AsyncAgentRunResumeInput input = {});
  AsyncAgentRunSnapshot cancel(const std::string& run_id, AsyncAgentRunCancelInput input = {});
  bool run_once();

 private:
  [[nodiscard]] AsyncAgentRunSnapshot require_snapshot(const std::string& run_id) const;
  [[nodiscard]] std::optional<AgentRunnerDurableState> latest_runner_state(
      const AsyncAgentRunSnapshot& snapshot) const;
  void normalize_topology(AsyncAgentRunStartInput& input) const;
  void enforce_child_agent_policy(const AsyncAgentRunStartInput& input) const;

  AsyncAgentRunStore& store_;
  InMemoryTaskQueue* queue_ = nullptr;
  AsyncAgentRunWorker* worker_ = nullptr;
  RunTranscript* transcript_ = nullptr;
  ChildAgentPolicy child_agent_policy_;
};

AsyncAgentRunStartInput async_agent_run_start_input_from_value(const Value& value);
Value async_agent_run_start_input_to_value(const AsyncAgentRunStartInput& input);
AsyncAgentRunFilter async_agent_run_filter_from_value(const Value& value);
AsyncAgentRunResumeInput async_agent_run_resume_input_from_value(const Value& value);
AsyncAgentRunCancelInput async_agent_run_cancel_input_from_value(const Value& value);

Value async_run_activity_to_value(const AsyncRunActivity& activity);
AsyncRunActivity async_run_activity_from_value(const Value& value);
Value resource_ledger_to_value(const ResourceLedger& ledger);
ResourceLedger resource_ledger_from_value(const Value& value);
Value child_agent_result_to_value(const ChildAgentResult& result);
ChildAgentResult child_agent_result_from_value(const Value& value);
Value child_agent_policy_to_value(const ChildAgentPolicy& policy);
ChildAgentPolicy child_agent_policy_from_value(const Value& value);
Value async_agent_run_to_value(const AsyncAgentRun& run);
Value async_agent_run_attempt_to_value(const AsyncAgentRunAttempt& attempt);
Value async_agent_run_event_to_value(const AsyncAgentRunEvent& event);
Value async_agent_run_checkpoint_to_value(const AsyncAgentRunCheckpoint& checkpoint);
Value async_agent_run_snapshot_to_value(const AsyncAgentRunSnapshot& snapshot);
AsyncAgentRun async_agent_run_from_task(const AgentTask& task);
AsyncAgentRunAttempt async_agent_run_attempt_from_task_run(const AgentTaskRun& run);
AsyncAgentRunEvent async_agent_run_event_from_task_event(const TaskEvent& event);
AsyncAgentRunCheckpoint async_agent_run_checkpoint_from_task_checkpoint(const TaskCheckpoint& checkpoint);

AgentLoopDurablePhase agent_loop_durable_phase_from_string(
    const std::string& value,
    AgentLoopDurablePhase fallback = AgentLoopDurablePhase::BeforeModel);
AgentLoopTerminationReason agent_loop_termination_reason_from_string(
    const std::string& value,
    AgentLoopTerminationReason fallback = AgentLoopTerminationReason::Completed);
AgentRunnerDurableStatus agent_runner_durable_status_from_string(
    const std::string& value,
    AgentRunnerDurableStatus fallback = AgentRunnerDurableStatus::Running);
AgentLoopDurableState agent_loop_durable_state_from_value(const Value& value);
AgentRunnerDurableState agent_runner_durable_state_from_value(const Value& value);

}  // namespace agent
