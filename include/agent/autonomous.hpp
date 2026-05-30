#pragma once

#include "agent/core.hpp"
#include "agent/runtime.hpp"

#include <mutex>

namespace agent {

enum class AutonomousRunStatus {
  Queued,
  Running,
  Waiting,
  Completed,
  Failed,
  Cancelled,
  Interrupted,
};

enum class AutonomousStepStatus {
  Pending,
  Running,
  Waiting,
  Completed,
  Failed,
  Cancelled,
  Skipped,
};

std::string to_string(AutonomousRunStatus status);
std::string to_string(AutonomousStepStatus status);
AutonomousRunStatus autonomous_run_status_from_string(const std::string& value);
AutonomousStepStatus autonomous_step_status_from_string(const std::string& value);

struct AutonomousRun {
  std::string id;
  std::string goal;
  Value input;
  AutonomousRunStatus status = AutonomousRunStatus::Queued;
  Value output;
  std::string error;
  std::string created_at;
  std::string updated_at;
  std::string completed_at;
  Value metadata = Value::object({});
};

struct AutonomousPlanStep {
  std::string id;
  std::string title;
  std::string objective;
  Value input;
  std::vector<std::string> depends_on;
  Value metadata = Value::object({});
};

struct AutonomousPlan {
  std::string summary;
  std::vector<AutonomousPlanStep> steps;
};

struct AutonomousStep {
  std::string id;
  std::string run_id;
  std::size_t index = 0;
  std::string title;
  std::string objective;
  Value input;
  AutonomousStepStatus status = AutonomousStepStatus::Pending;
  int attempts = 0;
  Value output;
  std::string error;
  std::string wait_reason;
  std::vector<std::string> depends_on;
  std::string created_at;
  std::string updated_at;
  std::string started_at;
  std::string completed_at;
  Value metadata = Value::object({});
};

struct AutonomousEvent {
  std::string id;
  std::string run_id;
  std::string step_id;
  std::string type;
  Value payload;
  std::string created_at;
};

struct AutonomousCheckpoint {
  std::string id;
  std::string run_id;
  std::string step_id;
  std::string name;
  Value state;
  std::string created_at;
};

struct AutonomousRunSnapshot {
  AutonomousRun run;
  std::vector<AutonomousStep> steps;
  std::vector<AutonomousEvent> events;
  std::vector<AutonomousCheckpoint> checkpoints;
};

struct CreateAutonomousRunInput {
  std::string id;
  std::string goal;
  Value input;
  Value metadata = Value::object({});
};

struct AutonomousRunFilter {
  std::optional<AutonomousRunStatus> status;
  Value metadata = Value::object({});
};

struct AutonomousRunPatch {
  std::string goal;
  bool has_goal = false;
  Value input;
  bool has_input = false;
  std::optional<AutonomousRunStatus> status;
  Value output;
  bool has_output = false;
  std::string error;
  bool has_error = false;
  std::string completed_at;
  bool has_completed_at = false;
  Value metadata;
  bool has_metadata = false;
  std::string updated_at;
};

struct AutonomousStepPatch {
  std::string title;
  bool has_title = false;
  std::string objective;
  bool has_objective = false;
  Value input;
  bool has_input = false;
  std::optional<AutonomousStepStatus> status;
  Value output;
  bool has_output = false;
  std::string error;
  bool has_error = false;
  std::string wait_reason;
  bool has_wait_reason = false;
  std::vector<std::string> depends_on;
  bool has_depends_on = false;
  Value metadata;
  bool has_metadata = false;
  std::string started_at;
  bool has_started_at = false;
  std::string completed_at;
  bool has_completed_at = false;
  std::optional<int> attempts;
  std::string updated_at;
};

class AutonomousStore {
 public:
  virtual ~AutonomousStore() = default;
  virtual AutonomousRun create_run(CreateAutonomousRunInput input) = 0;
  virtual std::optional<AutonomousRunSnapshot> get_run(const std::string& run_id) = 0;
  virtual AutonomousRun update_run(const std::string& run_id, const AutonomousRunPatch& patch) = 0;
  virtual std::vector<AutonomousStep> replace_steps(const std::string& run_id,
                                                    const std::vector<AutonomousPlanStep>& steps) = 0;
  virtual std::vector<AutonomousStep> append_steps(const std::string& run_id,
                                                   const std::vector<AutonomousPlanStep>& steps) = 0;
  virtual AutonomousStep update_step(const std::string& run_id,
                                     const std::string& step_id,
                                     const AutonomousStepPatch& patch) = 0;
  virtual AutonomousEvent append_event(AutonomousEvent event) = 0;
  virtual AutonomousCheckpoint append_checkpoint(AutonomousCheckpoint checkpoint) = 0;
  virtual std::vector<AutonomousRun> list_runs(AutonomousRunFilter filter = {}) = 0;
};

class InMemoryAutonomousStore : public AutonomousStore {
 public:
  AutonomousRun create_run(CreateAutonomousRunInput input) override;
  std::optional<AutonomousRunSnapshot> get_run(const std::string& run_id) override;
  AutonomousRun update_run(const std::string& run_id, const AutonomousRunPatch& patch) override;
  std::vector<AutonomousStep> replace_steps(const std::string& run_id,
                                            const std::vector<AutonomousPlanStep>& steps) override;
  std::vector<AutonomousStep> append_steps(const std::string& run_id,
                                           const std::vector<AutonomousPlanStep>& steps) override;
  AutonomousStep update_step(const std::string& run_id,
                             const std::string& step_id,
                             const AutonomousStepPatch& patch) override;
  AutonomousEvent append_event(AutonomousEvent event) override;
  AutonomousCheckpoint append_checkpoint(AutonomousCheckpoint checkpoint) override;
  std::vector<AutonomousRun> list_runs(AutonomousRunFilter filter = {}) override;

 protected:
  AutonomousRun& require_run(const std::string& run_id);
  std::vector<AutonomousStep> normalize_steps(const std::string& run_id,
                                              std::size_t existing_count,
                                              const std::vector<AutonomousPlanStep>& steps) const;

  mutable std::recursive_mutex mutex_;
  std::map<std::string, AutonomousRun> runs_;
  std::map<std::string, std::vector<AutonomousStep>> steps_;
  std::map<std::string, std::vector<AutonomousEvent>> events_;
  std::map<std::string, std::vector<AutonomousCheckpoint>> checkpoints_;
};

class FileAutonomousStore : public InMemoryAutonomousStore {
 public:
  explicit FileAutonomousStore(std::filesystem::path file_path);

  AutonomousRun create_run(CreateAutonomousRunInput input) override;
  std::optional<AutonomousRunSnapshot> get_run(const std::string& run_id) override;
  AutonomousRun update_run(const std::string& run_id, const AutonomousRunPatch& patch) override;
  std::vector<AutonomousStep> replace_steps(const std::string& run_id,
                                            const std::vector<AutonomousPlanStep>& steps) override;
  std::vector<AutonomousStep> append_steps(const std::string& run_id,
                                           const std::vector<AutonomousPlanStep>& steps) override;
  AutonomousStep update_step(const std::string& run_id,
                             const std::string& step_id,
                             const AutonomousStepPatch& patch) override;
  AutonomousEvent append_event(AutonomousEvent event) override;
  AutonomousCheckpoint append_checkpoint(AutonomousCheckpoint checkpoint) override;
  std::vector<AutonomousRun> list_runs(AutonomousRunFilter filter = {}) override;

 private:
  void ensure_loaded();
  void persist() const;

  std::filesystem::path file_path_;
  bool loaded_ = false;
};

class PgAutonomousClient {
 public:
  virtual ~PgAutonomousClient() = default;
  virtual std::vector<Value> query(std::string sql, std::vector<Value> params = {}) = 0;
  virtual std::shared_ptr<PgAutonomousClient> connect() { return {}; }
  virtual void release() {}
  virtual void close() {}
};

struct PgAutonomousStoreConfig {
  std::shared_ptr<PgAutonomousClient> client;
  std::string schema_name = "public";
  std::string table_prefix = "node_agent";
  bool create_tables = false;
  bool close_client_on_close = false;
};

class PgAutonomousStore : public AutonomousStore {
 public:
  explicit PgAutonomousStore(PgAutonomousStoreConfig config);
  ~PgAutonomousStore() override;

  PgAutonomousStore(const PgAutonomousStore&) = delete;
  PgAutonomousStore& operator=(const PgAutonomousStore&) = delete;

  AutonomousRun create_run(CreateAutonomousRunInput input) override;
  std::optional<AutonomousRunSnapshot> get_run(const std::string& run_id) override;
  AutonomousRun update_run(const std::string& run_id, const AutonomousRunPatch& patch) override;
  std::vector<AutonomousStep> replace_steps(const std::string& run_id,
                                            const std::vector<AutonomousPlanStep>& steps) override;
  std::vector<AutonomousStep> append_steps(const std::string& run_id,
                                           const std::vector<AutonomousPlanStep>& steps) override;
  AutonomousStep update_step(const std::string& run_id,
                             const std::string& step_id,
                             const AutonomousStepPatch& patch) override;
  AutonomousEvent append_event(AutonomousEvent event) override;
  AutonomousCheckpoint append_checkpoint(AutonomousCheckpoint checkpoint) override;
  std::vector<AutonomousRun> list_runs(AutonomousRunFilter filter = {}) override;
  void close();

 private:
  [[nodiscard]] std::string table_name(const std::string& suffix) const;
  [[nodiscard]] std::string runs_table() const;
  [[nodiscard]] std::string steps_table() const;
  [[nodiscard]] std::string events_table() const;
  [[nodiscard]] std::string checkpoints_table() const;
  [[nodiscard]] std::vector<AutonomousStep> normalize_steps(const std::string& run_id,
                                                            std::size_t existing_count,
                                                            const std::vector<AutonomousPlanStep>& steps) const;
  std::vector<AutonomousStep> insert_steps(PgAutonomousClient& client,
                                           const std::string& run_id,
                                           const std::vector<AutonomousStep>& steps) const;
  void ensure_ready() const;

  std::shared_ptr<PgAutonomousClient> client_;
  std::string schema_name_;
  std::string table_prefix_;
  bool create_tables_;
  bool close_client_on_close_;
  mutable bool ready_ = false;
  mutable std::mutex ready_mutex_;
};

struct AutonomousPlanningInput {
  AutonomousRun run;
};

class AutonomousPlanner {
 public:
  virtual ~AutonomousPlanner() = default;
  virtual AutonomousPlan plan(const AutonomousPlanningInput& input) = 0;
};

using AutonomousPlannerHandler = std::function<AutonomousPlan(const AutonomousPlanningInput&)>;

class StaticAutonomousPlanner : public AutonomousPlanner {
 public:
  explicit StaticAutonomousPlanner(std::vector<AutonomousPlanStep> steps);
  explicit StaticAutonomousPlanner(AutonomousPlan plan);
  explicit StaticAutonomousPlanner(AutonomousPlannerHandler handler);
  AutonomousPlan plan(const AutonomousPlanningInput& input) override;

 private:
  AutonomousPlan plan_;
  AutonomousPlannerHandler handler_;
};

struct AgentAutonomousPlannerConfig {
  AgentRunner* runner = nullptr;
  std::string session_id;
  ModelSettings model_settings;
  Value context = Value::object({});
};

class AgentAutonomousPlanner : public AutonomousPlanner {
 public:
  explicit AgentAutonomousPlanner(AgentAutonomousPlannerConfig config);
  AutonomousPlan plan(const AutonomousPlanningInput& input) override;

 private:
  AgentRunner* runner_;
  std::string session_id_;
  ModelSettings model_settings_;
  Value context_;
};

struct AutonomousStepExecutionInput {
  AutonomousRun run;
  AutonomousStep step;
  std::vector<AutonomousStep> previous_steps;
  std::function<AutonomousCheckpoint(std::string, Value)> checkpoint;
};

struct AutonomousStepExecutionResult {
  AutonomousStepStatus status = AutonomousStepStatus::Completed;
  Value output;
  std::string wait_reason;
  std::vector<AutonomousPlanStep> next_steps;
  Value metadata = Value::object({});
};

class AutonomousStepExecutor {
 public:
  virtual ~AutonomousStepExecutor() = default;
  virtual AutonomousStepExecutionResult execute(const AutonomousStepExecutionInput& input) = 0;
};

using AutonomousStepExecutorHandler = std::function<AutonomousStepExecutionResult(const AutonomousStepExecutionInput&)>;

class CallbackAutonomousStepExecutor : public AutonomousStepExecutor {
 public:
  explicit CallbackAutonomousStepExecutor(AutonomousStepExecutorHandler handler);
  AutonomousStepExecutionResult execute(const AutonomousStepExecutionInput& input) override;

 private:
  AutonomousStepExecutorHandler handler_;
};

using AgentRunnerStepSessionResolver = std::function<std::string(const AutonomousStepExecutionInput&)>;

struct AgentRunnerStepExecutorConfig {
  AgentRunner* runner = nullptr;
  std::string session_id;
  AgentRunnerStepSessionResolver session_resolver;
  ModelSettings model_settings;
  Value context = Value::object({});
};

class AgentRunnerStepExecutor : public AutonomousStepExecutor {
 public:
  explicit AgentRunnerStepExecutor(AgentRunnerStepExecutorConfig config);
  AutonomousStepExecutionResult execute(const AutonomousStepExecutionInput& input) override;

 private:
  AgentRunner* runner_;
  std::string session_id_;
  AgentRunnerStepSessionResolver session_resolver_;
  ModelSettings model_settings_;
  Value context_;
};

struct AutonomousRunManagerConfig {
  AutonomousStore* store = nullptr;
  AutonomousPlanner* planner = nullptr;
  AutonomousStepExecutor* executor = nullptr;
  std::size_t max_steps_per_run = 64;
};

class AutonomousRunManager {
 public:
  explicit AutonomousRunManager(AutonomousRunManagerConfig config);

  AutonomousRun create_run(CreateAutonomousRunInput input);
  AutonomousRunSnapshot plan(const std::string& run_id);
  AutonomousRunSnapshot run(const std::string& run_id);
  AutonomousRunSnapshot resume(const std::string& run_id, bool retry_failed_step = true);
  AutonomousRunSnapshot complete_waiting_step(const std::string& run_id,
                                              const std::string& step_id,
                                              Value output);
  AutonomousRunSnapshot cancel(const std::string& run_id,
                               std::string reason = "Autonomous run cancelled.");

 private:
  AutonomousRunSnapshot require_snapshot(const std::string& run_id);
  AutonomousRunSnapshot prepare_run(const std::string& run_id);
  void requeue_running_steps(const AutonomousRunSnapshot& snapshot);
  AutonomousRunSnapshot execute_step(const AutonomousRunSnapshot& snapshot, const AutonomousStep& step);
  AutonomousRunSnapshot complete_run(const std::string& run_id, const AutonomousRunSnapshot& snapshot);

  AutonomousStore* store_;
  AutonomousPlanner* planner_;
  AutonomousStepExecutor* executor_;
  std::size_t max_steps_per_run_;
};

Value autonomous_run_to_value(const AutonomousRun& run);
Value autonomous_step_to_value(const AutonomousStep& step);
Value autonomous_event_to_value(const AutonomousEvent& event);
Value autonomous_checkpoint_to_value(const AutonomousCheckpoint& checkpoint);
Value autonomous_run_snapshot_to_value(const AutonomousRunSnapshot& snapshot);
AutonomousRun autonomous_run_from_value(const Value& value);
AutonomousStep autonomous_step_from_value(const Value& value);
AutonomousEvent autonomous_event_from_value(const Value& value);
AutonomousCheckpoint autonomous_checkpoint_from_value(const Value& value);

}  // namespace agent
