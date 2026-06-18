#pragma once

#include "agent/core.hpp"
#include "agent/execution.hpp"

#include <mutex>

namespace agent {

enum class TaskStatus {
  Queued,
  Running,
  Completed,
  Failed,
  Cancelled,
  Interrupted,
  Waiting,
};

std::string to_string(TaskStatus status);

struct AgentTask {
  std::string id;
  std::string type;
  Value input;
  std::string idempotency_key;
  std::string owner_api_key_id;
  std::string tenant_id;
  TaskStatus status = TaskStatus::Queued;
  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point updated_at;
  std::optional<std::chrono::system_clock::time_point> queued_at;
  std::optional<std::chrono::system_clock::time_point> started_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
  std::optional<std::chrono::system_clock::time_point> cancelled_at;
  Value output;
  std::string error;
  Value metadata = Value::object({});
};

struct AgentTaskRun {
  std::string id;
  std::string task_id;
  TaskStatus status = TaskStatus::Running;
  std::chrono::system_clock::time_point started_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
  Value output;
  std::string error;
  std::chrono::system_clock::time_point lease_expires_at;
  std::chrono::system_clock::time_point heartbeat_at;
  int attempt = 1;
};

struct AgentTaskStep {
  std::string id;
  std::string task_id;
  std::string run_id;
  std::string name;
  TaskStatus status = TaskStatus::Running;
  Value input;
  Value output;
  std::string error;
  std::chrono::system_clock::time_point started_at;
  std::optional<std::chrono::system_clock::time_point> completed_at;
};

struct TaskEvent {
  std::string id;
  std::string task_id;
  std::string run_id;
  std::string type;
  Value payload;
  std::chrono::system_clock::time_point created_at;
};

struct TaskCheckpoint {
  std::string id;
  std::string task_id;
  std::string run_id;
  std::string name;
  Value state;
  std::chrono::system_clock::time_point created_at;
};

struct TaskStoreSnapshot {
  AgentTask task;
  std::vector<AgentTaskRun> runs;
  std::vector<AgentTaskStep> steps;
  std::vector<TaskEvent> events;
  std::vector<TaskCheckpoint> checkpoints;
};

struct CreateTaskInput {
  std::string id;
  std::string type;
  Value input;
  std::string idempotency_key;
  std::string owner_api_key_id;
  std::string tenant_id;
  Value metadata = Value::object({});
};

struct TaskScopeFilter {
  std::string type;
  std::optional<TaskStatus> status;
  std::optional<std::string> owner_api_key_id;
  std::optional<std::string> tenant_id;
};

struct TaskQueueClaim {
  AgentTask task;
  AgentTaskRun run;
};

struct FileTaskStoreConfig {
  std::filesystem::path file_path;
};

class InMemoryTaskStore {
 public:
  virtual ~InMemoryTaskStore() = default;
  virtual AgentTask create_task(CreateTaskInput input);
  [[nodiscard]] virtual std::optional<TaskStoreSnapshot> get_task(const std::string& task_id) const;
  [[nodiscard]] virtual std::optional<TaskStoreSnapshot> find_task_by_idempotency_key(
      const std::string& idempotency_key, const TaskScopeFilter& filter = {}) const;
  virtual AgentTask update_task_status(const std::string& task_id, TaskStatus status, Value output = {},
                                       std::string error = {});
  virtual AgentTaskRun append_run(const std::string& task_id, int lease_ms = 30000);
  virtual AgentTaskRun update_run(const std::string& task_id, const std::string& run_id, TaskStatus status,
                                  Value output = {}, std::string error = {}, int lease_ms = 0);
  virtual AgentTaskStep append_step(AgentTaskStep step);
  virtual TaskEvent append_event(std::string task_id, std::string run_id, std::string type, Value payload = {});
  virtual TaskCheckpoint append_checkpoint(std::string task_id, std::string run_id, std::string name, Value state = {});
  [[nodiscard]] virtual std::vector<AgentTask> list_tasks(const TaskScopeFilter& filter = {}) const;
  virtual std::optional<TaskQueueClaim> claim_task(const TaskScopeFilter& filter = {}, int lease_ms = 30000);

 protected:
  AgentTask& require_task(const std::string& task_id);
  [[nodiscard]] const AgentTask& require_task(const std::string& task_id) const;
  void requeue_expired_runs(std::chrono::system_clock::time_point now, const TaskScopeFilter& filter);
  [[nodiscard]] bool task_matches_filter(const AgentTask& task, const TaskScopeFilter& filter,
                                         bool include_status = true) const;

  mutable std::recursive_mutex mutex_;
  std::map<std::string, AgentTask> tasks_;
  std::map<std::string, std::vector<AgentTaskRun>> runs_;
  std::map<std::string, std::vector<AgentTaskStep>> steps_;
  std::map<std::string, std::vector<TaskEvent>> events_;
  std::map<std::string, std::vector<TaskCheckpoint>> checkpoints_;
};

class FileTaskStore : public InMemoryTaskStore {
 public:
  explicit FileTaskStore(std::filesystem::path file_path);
  explicit FileTaskStore(FileTaskStoreConfig config);
  AgentTask create_task(CreateTaskInput input) override;
  [[nodiscard]] std::optional<TaskStoreSnapshot> get_task(const std::string& task_id) const override;
  [[nodiscard]] std::optional<TaskStoreSnapshot> find_task_by_idempotency_key(
      const std::string& idempotency_key, const TaskScopeFilter& filter = {}) const override;
  [[nodiscard]] std::vector<AgentTask> list_tasks(const TaskScopeFilter& filter = {}) const override;
  std::optional<TaskQueueClaim> claim_task(const TaskScopeFilter& filter = {}, int lease_ms = 30000) override;
  AgentTask update_task_status(const std::string& task_id, TaskStatus status, Value output = {},
                               std::string error = {}) override;
  AgentTaskRun append_run(const std::string& task_id, int lease_ms = 30000) override;
  AgentTaskRun update_run(const std::string& task_id, const std::string& run_id, TaskStatus status,
                          Value output = {}, std::string error = {}, int lease_ms = 0) override;
  AgentTaskStep append_step(AgentTaskStep step) override;
  TaskEvent append_event(std::string task_id, std::string run_id, std::string type, Value payload = {}) override;
  TaskCheckpoint append_checkpoint(std::string task_id, std::string run_id, std::string name, Value state = {}) override;

 private:
  void ensure_loaded();
  void persist() const;

  std::filesystem::path file_path_;
  bool loaded_ = false;
};

class PgTaskClient {
 public:
  virtual ~PgTaskClient() = default;
  virtual std::vector<Value> query(std::string sql, std::vector<Value> params = {}) = 0;
  virtual std::shared_ptr<PgTaskClient> connect() { return {}; }
  virtual void release() {}
  virtual void close() {}
};

struct PgTaskStoreConfig {
  std::shared_ptr<PgTaskClient> client;
  std::string schema_name = "public";
  std::string table_prefix = "node_agent";
  bool create_tables = false;
  bool close_client_on_close = false;
};

class PgTaskStore : public InMemoryTaskStore {
 public:
  explicit PgTaskStore(PgTaskStoreConfig config);
  ~PgTaskStore() override;

  PgTaskStore(const PgTaskStore&) = delete;
  PgTaskStore& operator=(const PgTaskStore&) = delete;

  AgentTask create_task(CreateTaskInput input) override;
  [[nodiscard]] std::optional<TaskStoreSnapshot> get_task(const std::string& task_id) const override;
  [[nodiscard]] std::optional<TaskStoreSnapshot> find_task_by_idempotency_key(
      const std::string& idempotency_key, const TaskScopeFilter& filter = {}) const override;
  [[nodiscard]] std::vector<AgentTask> list_tasks(const TaskScopeFilter& filter = {}) const override;
  std::optional<TaskQueueClaim> claim_task(const TaskScopeFilter& filter = {}, int lease_ms = 30000) override;
  AgentTask update_task_status(const std::string& task_id, TaskStatus status, Value output = {},
                               std::string error = {}) override;
  AgentTaskRun append_run(const std::string& task_id, int lease_ms = 30000) override;
  AgentTaskRun update_run(const std::string& task_id, const std::string& run_id, TaskStatus status,
                          Value output = {}, std::string error = {}, int lease_ms = 0) override;
  AgentTaskStep append_step(AgentTaskStep step) override;
  TaskEvent append_event(std::string task_id, std::string run_id, std::string type, Value payload = {}) override;
  TaskCheckpoint append_checkpoint(std::string task_id, std::string run_id, std::string name, Value state = {}) override;
  void close();

 private:
  [[nodiscard]] std::string table_name(const std::string& suffix) const;
  [[nodiscard]] std::string tasks_table() const;
  [[nodiscard]] std::string runs_table() const;
  [[nodiscard]] std::string steps_table() const;
  [[nodiscard]] std::string events_table() const;
  [[nodiscard]] std::string checkpoints_table() const;
  [[nodiscard]] std::vector<std::string> scope_conditions(const TaskScopeFilter& filter,
                                                          std::vector<Value>& params,
                                                          const std::string& alias = {},
                                                          bool include_status = false) const;
  void ensure_ready() const;
  void requeue_expired_runs(PgTaskClient& client,
                            std::chrono::system_clock::time_point now,
                            const TaskScopeFilter& filter) const;

  std::shared_ptr<PgTaskClient> client_;
  std::string schema_name_;
  std::string table_prefix_;
  bool create_tables_;
  bool close_client_on_close_;
  mutable std::mutex lifecycle_mutex_;
  mutable bool ready_ = false;
};

struct InMemoryTaskQueueOptions {
  InMemoryTaskStore* store = nullptr;
  int lease_ms = 30000;
};

class InMemoryTaskQueue {
 public:
  explicit InMemoryTaskQueue(InMemoryTaskStore& store, int lease_ms = 30000);
  explicit InMemoryTaskQueue(InMemoryTaskQueueOptions options);
  void enqueue(const std::string& task_id);
  std::optional<TaskQueueClaim> claim(int lease_ms = 0);
  AgentTaskRun heartbeat(const std::string& task_id, const std::string& run_id, int lease_ms = 0);
  void complete(const std::string& task_id, const std::string& run_id, Value output = {});
  void fail(const std::string& task_id, const std::string& run_id, std::string error);
  void cancel(const std::string& task_id);

 private:
  void refresh_queued_task_ids();
  void remove_queued(const std::string& task_id);

  InMemoryTaskStore& store_;
  int lease_ms_;
  mutable std::mutex mutex_;
  std::deque<std::string> queued_task_ids_;
};

struct TaskHandlerContext {
  AgentTaskRun run;
  InMemoryTaskStore* store = nullptr;
  CancellationToken* cancellation = nullptr;

  TaskEvent event(std::string type, Value payload = {}) const;
  TaskCheckpoint checkpoint(std::string name, Value state = {}) const;
};

using TaskHandler = std::function<Value(const AgentTask&, TaskHandlerContext&)>;

struct BullMQTaskJobData {
  std::string task_id;
};

struct BullMQJobOptions {
  Value values = Value::object({});
};

struct BullMQJobLike {
  std::string id;
  std::string name;
  Value data = Value::object({});
};

class BullMQQueueClient {
 public:
  virtual ~BullMQQueueClient() = default;
  virtual void add(std::string name, BullMQTaskJobData data, BullMQJobOptions options = {}) = 0;
  virtual void remove_job(const std::string& job_id) = 0;
  virtual void close() {}
};

struct BullMQTaskQueueOptions {
  InMemoryTaskStore* store = nullptr;
  std::shared_ptr<BullMQQueueClient> queue;
  std::string queue_name = "node-agent-tasks";
  std::string job_name = "agent-task";
  BullMQJobOptions job_options;
  int lease_ms = 30000;
  bool close_queue_on_close = false;
};

class BullMQTaskQueue {
 public:
  explicit BullMQTaskQueue(BullMQTaskQueueOptions options);
  ~BullMQTaskQueue();

  BullMQTaskQueue(const BullMQTaskQueue&) = delete;
  BullMQTaskQueue& operator=(const BullMQTaskQueue&) = delete;

  void enqueue(const std::string& task_id);
  std::optional<TaskQueueClaim> claim(int lease_ms = 0);
  AgentTaskRun heartbeat(const std::string& task_id, const std::string& run_id, int lease_ms = 0);
  void complete(const std::string& task_id, const std::string& run_id, Value output = {});
  void fail(const std::string& task_id, const std::string& run_id, std::string error);
  void cancel(const std::string& task_id);
  void close();

 private:
  [[nodiscard]] TaskStoreSnapshot require_snapshot(const std::string& task_id) const;

  InMemoryTaskStore* store_;
  std::shared_ptr<BullMQQueueClient> queue_;
  std::string queue_name_;
  std::string job_name_;
  BullMQJobOptions job_options_;
  int lease_ms_;
  bool close_queue_on_close_;
  mutable std::mutex queue_mutex_;
  bool closed_ = false;
};

struct BullMQTaskWorkerOptions {
  InMemoryTaskStore* store = nullptr;
  BullMQTaskQueue* task_queue = nullptr;
  TaskHandler handler;
  int lease_ms = 30000;
  bool throw_on_handler_error = false;
};

class BullMQTaskWorker {
 public:
  explicit BullMQTaskWorker(BullMQTaskWorkerOptions options);
  bool run_once();
  void start();
  void stop();
  bool cancel(const std::string& task_id);
  void close(bool force = false);
  Value process(const BullMQJobLike& job);

 private:
  [[nodiscard]] std::optional<TaskQueueClaim> claim_task(const std::string& task_id);
  [[nodiscard]] bool is_task_cancelled(const std::string& task_id) const;
  [[nodiscard]] bool is_cancelled(const std::string& task_id) const;
  void mark_cancelled(const std::string& task_id);
  void clear_cancelled(const std::string& task_id);
  void register_active_token(const std::string& task_id, CancellationToken& token);
  void unregister_active_token(const std::string& task_id, CancellationToken& token);

  InMemoryTaskStore* store_;
  BullMQTaskQueue* task_queue_;
  TaskHandler handler_;
  int lease_ms_;
  bool throw_on_handler_error_;
  mutable std::mutex cancellation_mutex_;
  std::set<std::string> cancelled_task_ids_;
  std::map<std::string, CancellationToken*> active_cancellations_;
};

struct AgentTaskWorkerOptions {
  InMemoryTaskStore* store = nullptr;
  InMemoryTaskQueue* queue = nullptr;
  TaskHandler handler;
  int lease_ms = 30000;
};

class AgentTaskWorker {
 public:
  AgentTaskWorker(InMemoryTaskStore& store, InMemoryTaskQueue& queue, TaskHandler handler,
                  int lease_ms = 30000);
  explicit AgentTaskWorker(AgentTaskWorkerOptions options);
  bool run_once();
  bool cancel(const std::string& task_id);

 private:
  [[nodiscard]] bool is_cancelled(const std::string& task_id) const;
  void mark_cancelled(const std::string& task_id);
  void clear_cancelled(const std::string& task_id);
  void register_active_token(const std::string& task_id, CancellationToken& token);
  void unregister_active_token(const std::string& task_id, CancellationToken& token);

  InMemoryTaskStore& store_;
  InMemoryTaskQueue& queue_;
  TaskHandler handler_;
  int lease_ms_;
  mutable std::mutex cancellation_mutex_;
  std::set<std::string> cancelled_task_ids_;
  std::map<std::string, CancellationToken*> active_cancellations_;
};

Value agent_task_to_value(const AgentTask& task);
Value agent_task_run_to_value(const AgentTaskRun& run);
Value agent_task_step_to_value(const AgentTaskStep& step);
Value task_event_to_value(const TaskEvent& event);
Value task_checkpoint_to_value(const TaskCheckpoint& checkpoint);
Value task_store_snapshot_to_value(const TaskStoreSnapshot& snapshot);

}  // namespace agent
