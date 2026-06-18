#include "agent/tasks.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace agent {

std::string to_string(TaskStatus status) {
  switch (status) {
    case TaskStatus::Queued:
      return "queued";
    case TaskStatus::Running:
      return "running";
    case TaskStatus::Completed:
      return "completed";
    case TaskStatus::Failed:
      return "failed";
    case TaskStatus::Cancelled:
      return "cancelled";
    case TaskStatus::Interrupted:
      return "interrupted";
    case TaskStatus::Waiting:
      return "waiting";
  }
  return "queued";
}

InMemoryTaskStore& require_task_queue_store(InMemoryTaskStore* store) {
  if (!store) {
    throw ConfigurationError("InMemoryTaskQueue requires a task store.");
  }
  return *store;
}

InMemoryTaskStore& require_agent_task_worker_store(InMemoryTaskStore* store) {
  if (!store) {
    throw ConfigurationError("AgentTaskWorker requires a task store.");
  }
  return *store;
}

InMemoryTaskQueue& require_agent_task_worker_queue(InMemoryTaskQueue* queue) {
  if (!queue) {
    throw ConfigurationError("AgentTaskWorker requires a task queue.");
  }
  return *queue;
}

AgentTask InMemoryTaskStore::create_task(CreateTaskInput input) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const std::string id = input.id.empty() ? generate_uuid() : input.id;
  if (tasks_.find(id) != tasks_.end()) {
    throw AgentFrameworkError("Task already exists: " + id);
  }
  if (!input.idempotency_key.empty()) {
    for (const auto& [_, task] : tasks_) {
      if (task.idempotency_key == input.idempotency_key && task.type == input.type &&
          task.owner_api_key_id == input.owner_api_key_id && task.tenant_id == input.tenant_id) {
        throw AgentFrameworkError("Task idempotency key already exists: " + input.idempotency_key);
      }
    }
  }
  const auto now = std::chrono::system_clock::now();
  AgentTask task;
  task.id = id;
  task.type = std::move(input.type);
  task.input = std::move(input.input);
  task.idempotency_key = std::move(input.idempotency_key);
  task.owner_api_key_id = std::move(input.owner_api_key_id);
  task.tenant_id = std::move(input.tenant_id);
  task.status = TaskStatus::Queued;
  task.created_at = now;
  task.updated_at = now;
  task.queued_at = now;
  task.metadata = std::move(input.metadata);
  tasks_[id] = task;
  return task;
}

std::optional<TaskStoreSnapshot> InMemoryTaskStore::get_task(const std::string& task_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto found = tasks_.find(task_id);
  if (found == tasks_.end()) {
    return std::nullopt;
  }
  return TaskStoreSnapshot{found->second,
                           runs_.contains(task_id) ? runs_.at(task_id) : std::vector<AgentTaskRun>{},
                           steps_.contains(task_id) ? steps_.at(task_id) : std::vector<AgentTaskStep>{},
                           events_.contains(task_id) ? events_.at(task_id) : std::vector<TaskEvent>{},
                           checkpoints_.contains(task_id) ? checkpoints_.at(task_id)
                                                          : std::vector<TaskCheckpoint>{}};
}

std::optional<TaskStoreSnapshot> InMemoryTaskStore::find_task_by_idempotency_key(
    const std::string& idempotency_key, const TaskScopeFilter& filter) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& [_, task] : tasks_) {
    if (task.idempotency_key == idempotency_key && task_matches_filter(task, filter, false)) {
      return get_task(task.id);
    }
  }
  return std::nullopt;
}

AgentTask InMemoryTaskStore::update_task_status(const std::string& task_id, TaskStatus status,
                                                Value output, std::string error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto& task = require_task(task_id);
  const auto now = std::chrono::system_clock::now();
  task.status = status;
  task.updated_at = now;
  if (status == TaskStatus::Queued) {
    task.queued_at = now;
    task.completed_at.reset();
    task.cancelled_at.reset();
    task.error.clear();
  } else if (status == TaskStatus::Running) {
    if (!task.started_at) {
      task.started_at = now;
    }
  } else if (status == TaskStatus::Completed) {
    task.completed_at = now;
    task.output = std::move(output);
    task.error.clear();
  } else if (status == TaskStatus::Failed) {
    task.completed_at = now;
    task.error = std::move(error);
  } else if (status == TaskStatus::Cancelled) {
    task.cancelled_at = now;
    task.completed_at = now;
  } else if (status == TaskStatus::Interrupted) {
    task.error = std::move(error);
  }
  return task;
}

AgentTaskRun InMemoryTaskStore::append_run(const std::string& task_id, int lease_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  require_task(task_id);
  const auto now = std::chrono::system_clock::now();
  auto& runs = runs_[task_id];
  AgentTaskRun run;
  run.id = generate_uuid();
  run.task_id = task_id;
  run.status = TaskStatus::Running;
  run.started_at = now;
  run.heartbeat_at = now;
  run.lease_expires_at = now + std::chrono::milliseconds(lease_ms);
  run.attempt = static_cast<int>(runs.size()) + 1;
  runs.push_back(run);
  return run;
}

AgentTaskRun InMemoryTaskStore::update_run(const std::string& task_id, const std::string& run_id,
                                           TaskStatus status, Value output, std::string error, int lease_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto& runs = runs_[task_id];
  auto found = std::find_if(runs.begin(), runs.end(), [&](const auto& run) { return run.id == run_id; });
  if (found == runs.end()) {
    throw AgentFrameworkError("Task run not found: " + run_id);
  }
  const auto now = std::chrono::system_clock::now();
  found->status = status;
  found->heartbeat_at = now;
  if (lease_ms > 0) {
    found->lease_expires_at = now + std::chrono::milliseconds(lease_ms);
  }
  if (status == TaskStatus::Completed) {
    found->completed_at = now;
    found->output = std::move(output);
  } else if (status == TaskStatus::Failed || status == TaskStatus::Interrupted ||
             status == TaskStatus::Cancelled) {
    found->completed_at = now;
    found->error = std::move(error);
  }
  return *found;
}

AgentTaskStep InMemoryTaskStore::append_step(AgentTaskStep step) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  require_task(step.task_id);
  if (step.id.empty()) {
    step.id = generate_uuid();
  }
  if (step.started_at.time_since_epoch().count() == 0) {
    step.started_at = std::chrono::system_clock::now();
  }
  steps_[step.task_id].push_back(step);
  return step;
}

TaskEvent InMemoryTaskStore::append_event(std::string task_id, std::string run_id, std::string type, Value payload) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  require_task(task_id);
  TaskEvent event{generate_uuid(), std::move(task_id), std::move(run_id), std::move(type), std::move(payload),
                  std::chrono::system_clock::now()};
  events_[event.task_id].push_back(event);
  return event;
}

TaskCheckpoint InMemoryTaskStore::append_checkpoint(std::string task_id, std::string run_id, std::string name,
                                                    Value state) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  require_task(task_id);
  TaskCheckpoint checkpoint{generate_uuid(), std::move(task_id), std::move(run_id), std::move(name),
                            std::move(state), std::chrono::system_clock::now()};
  checkpoints_[checkpoint.task_id].push_back(checkpoint);
  return checkpoint;
}

std::vector<AgentTask> InMemoryTaskStore::list_tasks(const TaskScopeFilter& filter) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<AgentTask> tasks;
  for (const auto& [_, task] : tasks_) {
    if (task_matches_filter(task, filter)) {
      tasks.push_back(task);
    }
  }
  std::sort(tasks.begin(), tasks.end(), [](const auto& left, const auto& right) {
    return left.created_at < right.created_at;
  });
  return tasks;
}

std::optional<TaskQueueClaim> InMemoryTaskStore::claim_task(const TaskScopeFilter& filter, int lease_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto now = std::chrono::system_clock::now();
  requeue_expired_runs(now, filter);
  auto queued = list_tasks(TaskScopeFilter{filter.type, TaskStatus::Queued,
                                           filter.owner_api_key_id, filter.tenant_id});
  if (queued.empty()) {
    return std::nullopt;
  }
  auto task = update_task_status(queued.front().id, TaskStatus::Running);
  auto run = append_run(task.id, lease_ms);
  return TaskQueueClaim{task, run};
}

AgentTask& InMemoryTaskStore::require_task(const std::string& task_id) {
  auto found = tasks_.find(task_id);
  if (found == tasks_.end()) {
    throw AgentFrameworkError("Task not found: " + task_id);
  }
  return found->second;
}

const AgentTask& InMemoryTaskStore::require_task(const std::string& task_id) const {
  auto found = tasks_.find(task_id);
  if (found == tasks_.end()) {
    throw AgentFrameworkError("Task not found: " + task_id);
  }
  return found->second;
}

void InMemoryTaskStore::requeue_expired_runs(std::chrono::system_clock::time_point now,
                                             const TaskScopeFilter& filter) {
  for (auto& [task_id, task] : tasks_) {
    if (task.status != TaskStatus::Running || !task_matches_filter(task, filter, false)) {
      continue;
    }
    auto& runs = runs_[task_id];
    auto active = std::find_if(runs.rbegin(), runs.rend(), [](const auto& run) {
      return run.status == TaskStatus::Running;
    });
    if (active == runs.rend() || active->lease_expires_at > now) {
      continue;
    }
    active->status = TaskStatus::Interrupted;
    active->completed_at = now;
    active->error = "Task run lease expired.";
    task.status = TaskStatus::Queued;
    task.queued_at = now;
    task.updated_at = now;
  }
}

bool InMemoryTaskStore::task_matches_filter(const AgentTask& task, const TaskScopeFilter& filter,
                                            bool include_status) const {
  if (!filter.type.empty() && task.type != filter.type) {
    return false;
  }
  if (include_status && filter.status && task.status != *filter.status) {
    return false;
  }
  if (filter.owner_api_key_id && task.owner_api_key_id != *filter.owner_api_key_id) {
    return false;
  }
  if (filter.tenant_id && task.tenant_id != *filter.tenant_id) {
    return false;
  }
  return true;
}

InMemoryTaskQueue::InMemoryTaskQueue(InMemoryTaskStore& store, int lease_ms)
    : store_(store), lease_ms_(lease_ms) {}

InMemoryTaskQueue::InMemoryTaskQueue(InMemoryTaskQueueOptions options)
    : InMemoryTaskQueue(require_task_queue_store(options.store), options.lease_ms) {}

void InMemoryTaskQueue::enqueue(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = store_.get_task(task_id);
  if (!snapshot) {
    throw AgentFrameworkError("Task not found: " + task_id);
  }
  if (snapshot->task.status == TaskStatus::Cancelled) {
    return;
  }
  if (snapshot->task.status != TaskStatus::Queued) {
    store_.update_task_status(task_id, TaskStatus::Queued);
  }
  if (std::find(queued_task_ids_.begin(), queued_task_ids_.end(), task_id) == queued_task_ids_.end()) {
    queued_task_ids_.push_back(task_id);
  }
}

std::optional<TaskQueueClaim> InMemoryTaskQueue::claim(int lease_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  refresh_queued_task_ids();
  while (!queued_task_ids_.empty()) {
    const auto task_id = queued_task_ids_.front();
    queued_task_ids_.pop_front();
    auto snapshot = store_.get_task(task_id);
    if (!snapshot || snapshot->task.status != TaskStatus::Queued) {
      continue;
    }
    auto task = store_.update_task_status(task_id, TaskStatus::Running);
    auto run = store_.append_run(task_id, lease_ms > 0 ? lease_ms : lease_ms_);
    return TaskQueueClaim{task, run};
  }
  return std::nullopt;
}

AgentTaskRun InMemoryTaskQueue::heartbeat(const std::string& task_id, const std::string& run_id, int lease_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  return store_.update_run(task_id, run_id, TaskStatus::Running, {}, {}, lease_ms > 0 ? lease_ms : lease_ms_);
}

void InMemoryTaskQueue::complete(const std::string& task_id, const std::string& run_id, Value output) {
  std::lock_guard<std::mutex> lock(mutex_);
  store_.update_run(task_id, run_id, TaskStatus::Completed, output);
  store_.update_task_status(task_id, TaskStatus::Completed, std::move(output));
  remove_queued(task_id);
}

void InMemoryTaskQueue::fail(const std::string& task_id, const std::string& run_id, std::string error) {
  std::lock_guard<std::mutex> lock(mutex_);
  store_.update_run(task_id, run_id, TaskStatus::Failed, {}, error);
  store_.update_task_status(task_id, TaskStatus::Failed, {}, std::move(error));
  remove_queued(task_id);
}

void InMemoryTaskQueue::cancel(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = store_.get_task(task_id);
  if (!snapshot) {
    throw AgentFrameworkError("Task not found: " + task_id);
  }
  store_.update_task_status(task_id, TaskStatus::Cancelled);
  for (const auto& run : snapshot->runs) {
    if (run.status == TaskStatus::Running || run.status == TaskStatus::Queued) {
      store_.update_run(task_id, run.id, TaskStatus::Cancelled);
    }
  }
  remove_queued(task_id);
}

void InMemoryTaskQueue::refresh_queued_task_ids() {
  if (!queued_task_ids_.empty()) {
    return;
  }
  for (const auto& task : store_.list_tasks(TaskScopeFilter{.status = TaskStatus::Queued})) {
    queued_task_ids_.push_back(task.id);
  }
}

void InMemoryTaskQueue::remove_queued(const std::string& task_id) {
  queued_task_ids_.erase(std::remove(queued_task_ids_.begin(), queued_task_ids_.end(), task_id),
                         queued_task_ids_.end());
}

TaskEvent TaskHandlerContext::event(std::string type, Value payload) const {
  if (!store) {
    throw ConfigurationError("TaskHandlerContext.event requires a task store.");
  }
  return store->append_event(run.task_id, run.id, std::move(type), std::move(payload));
}

TaskCheckpoint TaskHandlerContext::checkpoint(std::string name, Value state) const {
  if (!store) {
    throw ConfigurationError("TaskHandlerContext.checkpoint requires a task store.");
  }
  return store->append_checkpoint(run.task_id, run.id, std::move(name), std::move(state));
}

BullMQTaskQueue::BullMQTaskQueue(BullMQTaskQueueOptions options)
    : store_(options.store),
      queue_(std::move(options.queue)),
      queue_name_(std::move(options.queue_name)),
      job_name_(std::move(options.job_name)),
      job_options_(std::move(options.job_options)),
      lease_ms_(options.lease_ms),
      close_queue_on_close_(options.close_queue_on_close) {
  if (!store_) {
    throw ConfigurationError("BullMQTaskQueue requires a task store.");
  }
  if (!queue_) {
    throw ConfigurationError("BullMQTaskQueue requires an injected queue client.");
  }
}

BullMQTaskQueue::~BullMQTaskQueue() {
  close();
}

void BullMQTaskQueue::enqueue(const std::string& task_id) {
  auto snapshot = require_snapshot(task_id);
  if (snapshot.task.status == TaskStatus::Cancelled) {
    return;
  }
  if (snapshot.task.status != TaskStatus::Queued) {
    store_->update_task_status(task_id, TaskStatus::Queued);
  }
  BullMQJobOptions options = job_options_;
  if (!options.values.is_object()) {
    options.values = Value::object({});
  }
  if (!options.values.contains("jobId")) {
    options.values["jobId"] = task_id;
  }
  std::lock_guard<std::mutex> lock(queue_mutex_);
  queue_->add(job_name_, BullMQTaskJobData{task_id}, std::move(options));
}

std::optional<TaskQueueClaim> BullMQTaskQueue::claim(int) {
  throw AgentFrameworkError(
      "BullMQTaskQueue does not support claim(); BullMQ workers are push-driven.");
}

AgentTaskRun BullMQTaskQueue::heartbeat(const std::string& task_id, const std::string& run_id, int lease_ms) {
  return store_->update_run(task_id, run_id, TaskStatus::Running, {}, {}, lease_ms > 0 ? lease_ms : lease_ms_);
}

void BullMQTaskQueue::complete(const std::string& task_id, const std::string& run_id, Value output) {
  store_->update_run(task_id, run_id, TaskStatus::Completed, output);
  store_->update_task_status(task_id, TaskStatus::Completed, std::move(output));
}

void BullMQTaskQueue::fail(const std::string& task_id, const std::string& run_id, std::string error) {
  store_->update_run(task_id, run_id, TaskStatus::Failed, {}, error);
  store_->update_task_status(task_id, TaskStatus::Failed, {}, std::move(error));
}

void BullMQTaskQueue::cancel(const std::string& task_id) {
  auto snapshot = require_snapshot(task_id);
  store_->update_task_status(task_id, TaskStatus::Cancelled);
  for (const auto& run : snapshot.runs) {
    if (run.status == TaskStatus::Running || run.status == TaskStatus::Queued) {
      store_->update_run(task_id, run.id, TaskStatus::Cancelled);
    }
  }
  try {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    queue_->remove_job(task_id);
  } catch (...) {
    // Active BullMQ jobs may be non-removable; task store state remains authoritative.
  }
}

void BullMQTaskQueue::close() {
  std::lock_guard<std::mutex> lock(queue_mutex_);
  if (closed_) {
    return;
  }
  closed_ = true;
  if (close_queue_on_close_ && queue_) {
    queue_->close();
  }
}

TaskStoreSnapshot BullMQTaskQueue::require_snapshot(const std::string& task_id) const {
  auto snapshot = store_->get_task(task_id);
  if (!snapshot) {
    throw AgentFrameworkError("Task not found: " + task_id);
  }
  return *snapshot;
}

namespace {

std::string read_bullmq_task_id(const BullMQJobLike& job) {
  const auto task_id = job.data.at("taskId").as_string(job.data.at("task_id").as_string());
  if (task_id.empty()) {
    throw AgentFrameworkError("BullMQ task job data must include a non-empty taskId.");
  }
  return task_id;
}

}  // namespace

BullMQTaskWorker::BullMQTaskWorker(BullMQTaskWorkerOptions options)
    : store_(options.store),
      task_queue_(options.task_queue),
      handler_(std::move(options.handler)),
      lease_ms_(options.lease_ms),
      throw_on_handler_error_(options.throw_on_handler_error) {
  if (!store_) {
    throw ConfigurationError("BullMQTaskWorker requires a task store.");
  }
  if (!task_queue_) {
    throw ConfigurationError("BullMQTaskWorker requires a BullMQTaskQueue.");
  }
  if (!handler_) {
    throw ConfigurationError("BullMQTaskWorker requires a task handler.");
  }
}

bool BullMQTaskWorker::run_once() {
  throw AgentFrameworkError("BullMQTaskWorker is push-driven and does not support run_once().");
}

void BullMQTaskWorker::start() {}

void BullMQTaskWorker::stop() {
  close();
}

bool BullMQTaskWorker::cancel(const std::string& task_id) {
  if (!store_->get_task(task_id)) {
    return false;
  }
  mark_cancelled(task_id);
  task_queue_->cancel(task_id);
  return true;
}

void BullMQTaskWorker::close(bool) {}

Value BullMQTaskWorker::process(const BullMQJobLike& job) {
  const auto task_id = read_bullmq_task_id(job);
  auto claim = claim_task(task_id);
  if (!claim) {
    return Value::object({{"skipped", true}});
  }

  CancellationToken cancellation;
  register_active_token(claim->task.id, cancellation);
  TaskHandlerContext context{claim->run, store_, &cancellation};
  try {
    store_->append_event(claim->task.id, claim->run.id, "task.started",
                         Value::object({{"attempt", claim->run.attempt}}));
    Value output = handler_(claim->task, context);
    if (is_task_cancelled(claim->task.id) || is_cancelled(claim->task.id) || cancellation.cancelled()) {
      task_queue_->cancel(claim->task.id);
      store_->append_event(claim->task.id, claim->run.id, "task.cancelled",
                           Value::object({{"attempt", claim->run.attempt}}));
      clear_cancelled(claim->task.id);
      unregister_active_token(claim->task.id, cancellation);
      return Value::object({{"cancelled", true}});
    }
    store_->append_checkpoint(claim->task.id, claim->run.id, "output", output);
    task_queue_->complete(claim->task.id, claim->run.id, output);
    store_->append_event(claim->task.id, claim->run.id, "task.completed", Value::object({{"output", output}}));
    unregister_active_token(claim->task.id, cancellation);
    return output;
  } catch (const std::exception& error) {
    if (is_task_cancelled(claim->task.id) || is_cancelled(claim->task.id) || cancellation.cancelled()) {
      task_queue_->cancel(claim->task.id);
      store_->append_event(claim->task.id, claim->run.id, "task.cancelled",
                           Value::object({{"attempt", claim->run.attempt}}));
      clear_cancelled(claim->task.id);
      unregister_active_token(claim->task.id, cancellation);
      return Value::object({{"cancelled", true}});
    }
    task_queue_->fail(claim->task.id, claim->run.id, error.what());
    store_->append_event(claim->task.id, claim->run.id, "task.failed",
                         Value::object({{"error", error.what()}}));
    unregister_active_token(claim->task.id, cancellation);
    if (throw_on_handler_error_) {
      throw;
    }
    return Value::object({{"failed", true}, {"error", error.what()}});
  } catch (...) {
    unregister_active_token(claim->task.id, cancellation);
    throw;
  }
}

std::optional<TaskQueueClaim> BullMQTaskWorker::claim_task(const std::string& task_id) {
  auto snapshot = store_->get_task(task_id);
  if (!snapshot) {
    throw AgentFrameworkError("Task not found: " + task_id);
  }
  if (snapshot->task.status == TaskStatus::Cancelled || snapshot->task.status != TaskStatus::Queued) {
    return std::nullopt;
  }
  auto run = store_->append_run(task_id, lease_ms_);
  auto task = store_->update_task_status(task_id, TaskStatus::Running);
  return TaskQueueClaim{task, run};
}

bool BullMQTaskWorker::is_task_cancelled(const std::string& task_id) const {
  auto snapshot = store_->get_task(task_id);
  return snapshot && snapshot->task.status == TaskStatus::Cancelled;
}

bool BullMQTaskWorker::is_cancelled(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  return cancelled_task_ids_.contains(task_id);
}

void BullMQTaskWorker::mark_cancelled(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_task_ids_.insert(task_id);
  if (auto found = active_cancellations_.find(task_id); found != active_cancellations_.end() && found->second) {
    found->second->cancel("Task \"" + task_id + "\" was cancelled.");
  }
}

void BullMQTaskWorker::clear_cancelled(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_task_ids_.erase(task_id);
}

void BullMQTaskWorker::register_active_token(const std::string& task_id, CancellationToken& token) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  active_cancellations_[task_id] = &token;
  if (cancelled_task_ids_.contains(task_id)) {
    token.cancel("Task \"" + task_id + "\" was cancelled.");
  }
}

void BullMQTaskWorker::unregister_active_token(const std::string& task_id, CancellationToken& token) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  if (auto found = active_cancellations_.find(task_id); found != active_cancellations_.end() && found->second == &token) {
    active_cancellations_.erase(found);
  }
}

AgentTaskWorker::AgentTaskWorker(InMemoryTaskStore& store, InMemoryTaskQueue& queue, TaskHandler handler,
                                 int lease_ms)
    : store_(store), queue_(queue), handler_(std::move(handler)), lease_ms_(lease_ms) {}

AgentTaskWorker::AgentTaskWorker(AgentTaskWorkerOptions options)
    : store_(require_agent_task_worker_store(options.store)),
      queue_(require_agent_task_worker_queue(options.queue)),
      handler_(std::move(options.handler)),
      lease_ms_(options.lease_ms) {
  if (!handler_) {
    throw ConfigurationError("AgentTaskWorker requires a task handler.");
  }
}

bool AgentTaskWorker::is_cancelled(const std::string& task_id) const {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  return cancelled_task_ids_.contains(task_id);
}

void AgentTaskWorker::mark_cancelled(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_task_ids_.insert(task_id);
  if (auto found = active_cancellations_.find(task_id); found != active_cancellations_.end() && found->second) {
    found->second->cancel("Task \"" + task_id + "\" was cancelled.");
  }
}

void AgentTaskWorker::clear_cancelled(const std::string& task_id) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  cancelled_task_ids_.erase(task_id);
}

void AgentTaskWorker::register_active_token(const std::string& task_id, CancellationToken& token) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  active_cancellations_[task_id] = &token;
  if (cancelled_task_ids_.contains(task_id)) {
    token.cancel("Task \"" + task_id + "\" was cancelled.");
  }
}

void AgentTaskWorker::unregister_active_token(const std::string& task_id, CancellationToken& token) {
  std::lock_guard<std::mutex> lock(cancellation_mutex_);
  if (auto found = active_cancellations_.find(task_id); found != active_cancellations_.end() && found->second == &token) {
    active_cancellations_.erase(found);
  }
}

bool AgentTaskWorker::run_once() {
  auto claim = queue_.claim(lease_ms_);
  if (!claim) {
    return false;
  }
  CancellationToken cancellation;
  register_active_token(claim->task.id, cancellation);
  TaskHandlerContext context{claim->run, &store_, &cancellation};
  try {
    try {
      store_.append_event(claim->task.id, claim->run.id, "task.started",
                          Value::object({{"attempt", claim->run.attempt}}));
      Value output = handler_(claim->task, context);
      if (is_cancelled(claim->task.id)) {
        queue_.cancel(claim->task.id);
        store_.append_event(claim->task.id, claim->run.id, "task.cancelled");
        clear_cancelled(claim->task.id);
      } else {
        store_.append_checkpoint(claim->task.id, claim->run.id, "output", output);
        queue_.complete(claim->task.id, claim->run.id, output);
        store_.append_event(claim->task.id, claim->run.id, "task.completed", Value::object({{"output", output}}));
      }
    } catch (const std::exception& error) {
      if (is_cancelled(claim->task.id) || cancellation.cancelled()) {
        queue_.cancel(claim->task.id);
        store_.append_event(claim->task.id, claim->run.id, "task.cancelled");
        clear_cancelled(claim->task.id);
      } else {
        queue_.fail(claim->task.id, claim->run.id, error.what());
        store_.append_event(claim->task.id, claim->run.id, "task.failed", Value::object({{"error", error.what()}}));
      }
    }
  } catch (...) {
    unregister_active_token(claim->task.id, cancellation);
    throw;
  }
  unregister_active_token(claim->task.id, cancellation);
  return true;
}

bool AgentTaskWorker::cancel(const std::string& task_id) {
  if (!store_.get_task(task_id)) {
    return false;
  }
  mark_cancelled(task_id);
  queue_.cancel(task_id);
  return true;
}

namespace {

TaskStatus task_status_from_string(const std::string& value) {
  if (value == "running") return TaskStatus::Running;
  if (value == "completed") return TaskStatus::Completed;
  if (value == "failed") return TaskStatus::Failed;
  if (value == "cancelled") return TaskStatus::Cancelled;
  if (value == "interrupted") return TaskStatus::Interrupted;
  if (value == "waiting") return TaskStatus::Waiting;
  return TaskStatus::Queued;
}

Value task_to_value(const AgentTask& task) {
  return Value::object({{"id", task.id},
                        {"type", task.type},
                        {"input", task.input},
                        {"idempotencyKey", task.idempotency_key},
                        {"ownerApiKeyId", task.owner_api_key_id},
                        {"tenantId", task.tenant_id},
                        {"status", to_string(task.status)},
                        {"createdAtMs", time_point_to_ms(task.created_at)},
                        {"updatedAtMs", time_point_to_ms(task.updated_at)},
                        {"queuedAtMs", task.queued_at ? time_point_to_ms(*task.queued_at) : 0},
                        {"startedAtMs", task.started_at ? time_point_to_ms(*task.started_at) : 0},
                        {"completedAtMs", task.completed_at ? time_point_to_ms(*task.completed_at) : 0},
                        {"cancelledAtMs", task.cancelled_at ? time_point_to_ms(*task.cancelled_at) : 0},
                        {"output", task.output},
                        {"error", task.error},
                        {"metadata", task.metadata}});
}

AgentTask task_from_value(const Value& value) {
  AgentTask task;
  task.id = value.at("id").as_string();
  task.type = value.at("type").as_string();
  task.input = value.at("input");
  task.idempotency_key = value.at("idempotencyKey").as_string();
  task.owner_api_key_id = value.at("ownerApiKeyId").as_string();
  task.tenant_id = value.at("tenantId").as_string();
  task.status = task_status_from_string(value.at("status").as_string("queued"));
  task.created_at = ms_to_time_point(value.at("createdAtMs").as_integer());
  task.updated_at = ms_to_time_point(value.at("updatedAtMs").as_integer());
  if (value.at("queuedAtMs").as_integer() > 0) task.queued_at = ms_to_time_point(value.at("queuedAtMs").as_integer());
  if (value.at("startedAtMs").as_integer() > 0) task.started_at = ms_to_time_point(value.at("startedAtMs").as_integer());
  if (value.at("completedAtMs").as_integer() > 0) task.completed_at = ms_to_time_point(value.at("completedAtMs").as_integer());
  if (value.at("cancelledAtMs").as_integer() > 0) task.cancelled_at = ms_to_time_point(value.at("cancelledAtMs").as_integer());
  task.output = value.at("output");
  task.error = value.at("error").as_string();
  task.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return task;
}

Value run_to_value(const AgentTaskRun& run) {
  return Value::object({{"id", run.id},
                        {"taskId", run.task_id},
                        {"status", to_string(run.status)},
                        {"startedAtMs", time_point_to_ms(run.started_at)},
                        {"completedAtMs", run.completed_at ? time_point_to_ms(*run.completed_at) : 0},
                        {"output", run.output},
                        {"error", run.error},
                        {"leaseExpiresAtMs", time_point_to_ms(run.lease_expires_at)},
                        {"heartbeatAtMs", time_point_to_ms(run.heartbeat_at)},
                        {"attempt", run.attempt}});
}

AgentTaskRun run_from_value(const Value& value) {
  AgentTaskRun run;
  run.id = value.at("id").as_string();
  run.task_id = value.at("taskId").as_string();
  run.status = task_status_from_string(value.at("status").as_string("running"));
  run.started_at = ms_to_time_point(value.at("startedAtMs").as_integer());
  if (value.at("completedAtMs").as_integer() > 0) run.completed_at = ms_to_time_point(value.at("completedAtMs").as_integer());
  run.output = value.at("output");
  run.error = value.at("error").as_string();
  run.lease_expires_at = ms_to_time_point(value.at("leaseExpiresAtMs").as_integer());
  run.heartbeat_at = ms_to_time_point(value.at("heartbeatAtMs").as_integer());
  run.attempt = static_cast<int>(value.at("attempt").as_integer(1));
  return run;
}

Value event_to_value(const TaskEvent& event) {
  return Value::object({{"id", event.id}, {"taskId", event.task_id}, {"runId", event.run_id},
                        {"type", event.type}, {"payload", event.payload},
                        {"createdAtMs", time_point_to_ms(event.created_at)}});
}

TaskEvent event_from_value(const Value& value) {
  return TaskEvent{value.at("id").as_string(), value.at("taskId").as_string(),
                   value.at("runId").as_string(), value.at("type").as_string(),
                   value.at("payload"), ms_to_time_point(value.at("createdAtMs").as_integer())};
}

Value step_to_value(const AgentTaskStep& step) {
  return Value::object({{"id", step.id}, {"taskId", step.task_id}, {"runId", step.run_id},
                        {"name", step.name}, {"status", to_string(step.status)},
                        {"input", step.input}, {"output", step.output}, {"error", step.error},
                        {"startedAtMs", time_point_to_ms(step.started_at)},
                        {"completedAtMs", step.completed_at ? time_point_to_ms(*step.completed_at) : 0}});
}

AgentTaskStep step_from_value(const Value& value) {
  AgentTaskStep step;
  step.id = value.at("id").as_string();
  step.task_id = value.at("taskId").as_string();
  step.run_id = value.at("runId").as_string();
  step.name = value.at("name").as_string();
  step.status = task_status_from_string(value.at("status").as_string("running"));
  step.input = value.at("input");
  step.output = value.at("output");
  step.error = value.at("error").as_string();
  step.started_at = ms_to_time_point(value.at("startedAtMs").as_integer());
  if (value.at("completedAtMs").as_integer() > 0) step.completed_at = ms_to_time_point(value.at("completedAtMs").as_integer());
  return step;
}

Value checkpoint_to_value(const TaskCheckpoint& checkpoint) {
  return Value::object({{"id", checkpoint.id}, {"taskId", checkpoint.task_id}, {"runId", checkpoint.run_id},
                        {"name", checkpoint.name}, {"state", checkpoint.state},
                        {"createdAtMs", time_point_to_ms(checkpoint.created_at)}});
}

TaskCheckpoint checkpoint_from_value(const Value& value) {
  return TaskCheckpoint{value.at("id").as_string(), value.at("taskId").as_string(),
                        value.at("runId").as_string(), value.at("name").as_string(),
                        value.at("state"), ms_to_time_point(value.at("createdAtMs").as_integer())};
}

std::string pg_identifier(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
}

Value pg_json_param(const Value& value, bool null_when_null = false) {
  if (null_when_null && value.is_null()) {
    return Value();
  }
  return value.stringify(0);
}

Value pg_time_param(std::chrono::system_clock::time_point value) {
  return time_point_to_ms(value);
}

Value pg_optional_time_param(const std::optional<std::chrono::system_clock::time_point>& value) {
  return value ? Value(time_point_to_ms(*value)) : Value();
}

std::chrono::system_clock::time_point pg_time_from_value(const Value& value) {
  if (value.is_number()) {
    return ms_to_time_point(value.as_integer());
  }
  if (value.is_string()) {
    try {
      return ms_to_time_point(std::stoll(value.as_string()));
    } catch (...) {
      return ms_to_time_point(0);
    }
  }
  return ms_to_time_point(0);
}

std::optional<std::chrono::system_clock::time_point> pg_optional_time_from_value(const Value& value) {
  if (value.is_null()) {
    return std::nullopt;
  }
  const auto parsed = pg_time_from_value(value);
  if (parsed.time_since_epoch().count() == 0) {
    return std::nullopt;
  }
  return parsed;
}

Value pg_json_from_value(const Value& value) {
  if (!value.is_string()) {
    return value;
  }
  try {
    return parse_json(value.as_string());
  } catch (...) {
    return value;
  }
}

AgentTask task_from_pg_row(const Value& row) {
  AgentTask task;
  task.id = row.at("id").as_string();
  task.type = row.at("type").as_string();
  task.input = pg_json_from_value(row.at("input"));
  task.idempotency_key = row.at("idempotency_key").as_string();
  task.owner_api_key_id = row.at("owner_api_key_id").as_string();
  task.tenant_id = row.at("tenant_id").as_string();
  task.status = task_status_from_string(row.at("status").as_string("queued"));
  task.created_at = pg_time_from_value(row.at("created_at"));
  task.updated_at = pg_time_from_value(row.at("updated_at"));
  task.queued_at = pg_optional_time_from_value(row.at("queued_at"));
  task.started_at = pg_optional_time_from_value(row.at("started_at"));
  task.completed_at = pg_optional_time_from_value(row.at("completed_at"));
  task.cancelled_at = pg_optional_time_from_value(row.at("cancelled_at"));
  task.output = row.at("output").is_null() ? Value() : pg_json_from_value(row.at("output"));
  task.error = row.at("error").as_string();
  task.metadata = row.at("metadata").is_null() ? Value::object({}) : pg_json_from_value(row.at("metadata"));
  return task;
}

AgentTaskRun run_from_pg_row(const Value& row) {
  AgentTaskRun run;
  run.id = row.at("id").as_string();
  run.task_id = row.at("task_id").as_string();
  run.status = task_status_from_string(row.at("status").as_string("running"));
  run.started_at = pg_time_from_value(row.at("started_at"));
  run.completed_at = pg_optional_time_from_value(row.at("completed_at"));
  run.output = row.at("output").is_null() ? Value() : pg_json_from_value(row.at("output"));
  run.error = row.at("error").as_string();
  run.lease_expires_at = pg_time_from_value(row.at("lease_expires_at"));
  run.heartbeat_at = pg_time_from_value(row.at("heartbeat_at"));
  run.attempt = static_cast<int>(row.at("attempt").as_integer(1));
  return run;
}

AgentTaskStep step_from_pg_row(const Value& row) {
  AgentTaskStep step;
  step.id = row.at("id").as_string();
  step.task_id = row.at("task_id").as_string();
  step.run_id = row.at("run_id").as_string();
  step.name = row.at("name").as_string();
  step.status = task_status_from_string(row.at("status").as_string("running"));
  step.input = row.at("input").is_null() ? Value() : pg_json_from_value(row.at("input"));
  step.output = row.at("output").is_null() ? Value() : pg_json_from_value(row.at("output"));
  step.error = row.at("error").as_string();
  step.started_at = pg_time_from_value(row.at("started_at"));
  step.completed_at = pg_optional_time_from_value(row.at("completed_at"));
  return step;
}

TaskEvent event_from_pg_row(const Value& row) {
  TaskEvent event;
  event.id = row.at("id").as_string();
  event.task_id = row.at("task_id").as_string();
  event.run_id = row.at("run_id").as_string();
  event.type = row.at("type").as_string();
  event.payload = row.at("payload").is_null() ? Value() : pg_json_from_value(row.at("payload"));
  event.created_at = pg_time_from_value(row.at("created_at"));
  return event;
}

TaskCheckpoint checkpoint_from_pg_row(const Value& row) {
  TaskCheckpoint checkpoint;
  checkpoint.id = row.at("id").as_string();
  checkpoint.task_id = row.at("task_id").as_string();
  checkpoint.run_id = row.at("run_id").as_string();
  checkpoint.name = row.at("name").as_string();
  checkpoint.state = pg_json_from_value(row.at("state"));
  checkpoint.created_at = pg_time_from_value(row.at("created_at"));
  return checkpoint;
}

int pg_count_from_row(const Value& row) {
  const auto& value = row.at("count");
  if (value.is_number()) {
    return static_cast<int>(value.as_integer());
  }
  if (value.is_string()) {
    try {
      return std::stoi(value.as_string());
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

template <typename T>
T require_pg_row(const std::vector<T>& rows, const std::string& message) {
  if (rows.empty()) {
    throw AgentFrameworkError(message);
  }
  return rows.front();
}

}  // namespace

Value agent_task_to_value(const AgentTask& task) {
  return task_to_value(task);
}

Value agent_task_run_to_value(const AgentTaskRun& run) {
  return run_to_value(run);
}

Value agent_task_step_to_value(const AgentTaskStep& step) {
  return step_to_value(step);
}

Value task_event_to_value(const TaskEvent& event) {
  return event_to_value(event);
}

Value task_checkpoint_to_value(const TaskCheckpoint& checkpoint) {
  return checkpoint_to_value(checkpoint);
}

Value task_store_snapshot_to_value(const TaskStoreSnapshot& snapshot) {
  Value::Array runs;
  for (const auto& run : snapshot.runs) {
    runs.push_back(agent_task_run_to_value(run));
  }
  Value::Array steps;
  for (const auto& step : snapshot.steps) {
    steps.push_back(agent_task_step_to_value(step));
  }
  Value::Array events;
  for (const auto& event : snapshot.events) {
    events.push_back(task_event_to_value(event));
  }
  Value::Array checkpoints;
  for (const auto& checkpoint : snapshot.checkpoints) {
    checkpoints.push_back(task_checkpoint_to_value(checkpoint));
  }
  return Value::object({{"task", agent_task_to_value(snapshot.task)},
                        {"runs", Value(std::move(runs))},
                        {"steps", Value(std::move(steps))},
                        {"events", Value(std::move(events))},
                        {"checkpoints", Value(std::move(checkpoints))}});
}

PgTaskStore::PgTaskStore(PgTaskStoreConfig config)
    : client_(std::move(config.client)),
      schema_name_(std::move(config.schema_name)),
      table_prefix_(std::move(config.table_prefix)),
      create_tables_(config.create_tables),
      close_client_on_close_(config.close_client_on_close) {
  if (!client_) {
    throw ConfigurationError("PgTaskStore requires an injected PgTaskClient.");
  }
  if (schema_name_.empty()) {
    schema_name_ = "public";
  }
  if (table_prefix_.empty()) {
    table_prefix_ = "node_agent";
  }
}

PgTaskStore::~PgTaskStore() {
  close();
}

AgentTask PgTaskStore::create_task(CreateTaskInput input) {
  ensure_ready();
  const auto now = std::chrono::system_clock::now();
  const std::string id = input.id.empty() ? generate_uuid() : input.id;
  const auto rows = client_->query(
      "INSERT INTO " + tasks_table() + " ("
      "id, type, input, idempotency_key, owner_api_key_id, tenant_id, "
      "status, created_at, updated_at, queued_at, started_at, completed_at, "
      "cancelled_at, output, error, metadata"
      ") VALUES ($1, $2, $3::jsonb, $4, $5, $6, 'queued', $7, $7, $7, NULL, NULL, NULL, NULL, NULL, $8::jsonb) "
      "RETURNING *",
      {id, input.type, pg_json_param(input.input), input.idempotency_key, input.owner_api_key_id,
       input.tenant_id, pg_time_param(now), pg_json_param(input.metadata)});
  return task_from_pg_row(require_pg_row(rows, "Task insert returned no row: " + id));
}

std::optional<TaskStoreSnapshot> PgTaskStore::get_task(const std::string& task_id) const {
  ensure_ready();
  const auto task_rows = client_->query("SELECT * FROM " + tasks_table() + " WHERE id = $1", {task_id});
  if (task_rows.empty()) {
    return std::nullopt;
  }
  TaskStoreSnapshot snapshot;
  snapshot.task = task_from_pg_row(task_rows.front());
  for (const auto& row : client_->query("SELECT * FROM " + runs_table() +
                                            " WHERE task_id = $1 ORDER BY started_at ASC",
                                        {task_id})) {
    snapshot.runs.push_back(run_from_pg_row(row));
  }
  for (const auto& row : client_->query("SELECT * FROM " + steps_table() +
                                            " WHERE task_id = $1 ORDER BY started_at ASC",
                                        {task_id})) {
    snapshot.steps.push_back(step_from_pg_row(row));
  }
  for (const auto& row : client_->query("SELECT * FROM " + events_table() +
                                            " WHERE task_id = $1 ORDER BY created_at ASC",
                                        {task_id})) {
    snapshot.events.push_back(event_from_pg_row(row));
  }
  for (const auto& row : client_->query("SELECT * FROM " + checkpoints_table() +
                                            " WHERE task_id = $1 ORDER BY created_at ASC",
                                        {task_id})) {
    snapshot.checkpoints.push_back(checkpoint_from_pg_row(row));
  }
  return snapshot;
}

std::optional<TaskStoreSnapshot> PgTaskStore::find_task_by_idempotency_key(
    const std::string& idempotency_key, const TaskScopeFilter& filter) const {
  ensure_ready();
  std::vector<Value> params{idempotency_key};
  auto conditions = scope_conditions(filter, params);
  conditions.insert(conditions.begin(), "idempotency_key = $1");
  std::ostringstream sql;
  sql << "SELECT * FROM " << tasks_table() << " WHERE ";
  for (std::size_t index = 0; index < conditions.size(); ++index) {
    if (index > 0) {
      sql << " AND ";
    }
    sql << conditions[index];
  }
  sql << " ORDER BY created_at ASC LIMIT 1";
  const auto rows = client_->query(sql.str(), params);
  return rows.empty() ? std::optional<TaskStoreSnapshot>{} : get_task(rows.front().at("id").as_string());
}

std::vector<AgentTask> PgTaskStore::list_tasks(const TaskScopeFilter& filter) const {
  ensure_ready();
  std::vector<Value> params;
  auto conditions = scope_conditions(filter, params, {}, true);
  std::ostringstream sql;
  sql << "SELECT * FROM " << tasks_table();
  if (!conditions.empty()) {
    sql << " WHERE ";
    for (std::size_t index = 0; index < conditions.size(); ++index) {
      if (index > 0) {
        sql << " AND ";
      }
      sql << conditions[index];
    }
  }
  sql << " ORDER BY COALESCE(queued_at, created_at) ASC";
  std::vector<AgentTask> tasks;
  for (const auto& row : client_->query(sql.str(), params)) {
    tasks.push_back(task_from_pg_row(row));
  }
  return tasks;
}

std::optional<TaskQueueClaim> PgTaskStore::claim_task(const TaskScopeFilter& filter, int lease_ms) {
  ensure_ready();
  auto tx = client_->connect();
  PgTaskClient& client = tx ? *tx : *client_;
  const auto now = std::chrono::system_clock::now();
  try {
    client.query("BEGIN");
    requeue_expired_runs(client, now, filter);

    std::vector<Value> params;
    auto conditions = scope_conditions(filter, params, "t");
    conditions.insert(conditions.begin(), "status = 'queued'");
    std::ostringstream select;
    select << "SELECT t.* FROM " << tasks_table() << " t WHERE ";
    for (std::size_t index = 0; index < conditions.size(); ++index) {
      if (index > 0) {
        select << " AND ";
      }
      select << conditions[index];
    }
    select << " ORDER BY COALESCE(t.queued_at, t.created_at) ASC LIMIT 1 FOR UPDATE SKIP LOCKED";
    const auto task_rows = client.query(select.str(), params);
    if (task_rows.empty()) {
      client.query("COMMIT");
      if (tx) tx->release();
      return std::nullopt;
    }

    const auto task_id = task_rows.front().at("id").as_string();
    const auto count_rows = client.query("SELECT COUNT(*)::text AS count FROM " + runs_table() +
                                             " WHERE task_id = $1",
                                         {task_id});
    const int attempt = (count_rows.empty() ? 0 : pg_count_from_row(count_rows.front())) + 1;
    AgentTaskRun run;
    run.id = generate_uuid();
    run.task_id = task_id;
    run.status = TaskStatus::Running;
    run.started_at = now;
    run.heartbeat_at = now;
    run.lease_expires_at = now + std::chrono::milliseconds(lease_ms);
    run.attempt = attempt;
    const auto run_rows = client.query(
        "INSERT INTO " + runs_table() + " ("
        "id, task_id, status, started_at, completed_at, output, error, lease_expires_at, heartbeat_at, attempt"
        ") VALUES ($1, $2, 'running', $3, NULL, NULL, NULL, $4, $3, $5) RETURNING *",
        {run.id, task_id, pg_time_param(now), pg_time_param(run.lease_expires_at), run.attempt});
    const auto updated_task_rows = client.query(
        "UPDATE " + tasks_table() +
            " SET status = 'running', started_at = COALESCE(started_at, $2), updated_at = $2 "
            "WHERE id = $1 RETURNING *",
        {task_id, pg_time_param(now)});
    client.query("COMMIT");
    if (tx) tx->release();
    return TaskQueueClaim{task_from_pg_row(require_pg_row(updated_task_rows, "Task not found: " + task_id)),
                          run_from_pg_row(require_pg_row(run_rows, "Task run insert returned no row: " + run.id))};
  } catch (...) {
    try {
      client.query("ROLLBACK");
    } catch (...) {
    }
    if (tx) tx->release();
    throw;
  }
}

AgentTask PgTaskStore::update_task_status(const std::string& task_id, TaskStatus status, Value output,
                                          std::string error) {
  ensure_ready();
  const auto now = std::chrono::system_clock::now();
  std::vector<Value> params{to_string(status), pg_time_param(now)};
  std::ostringstream set;
  set << "status = $1, updated_at = $2";
  if (status == TaskStatus::Queued) {
    set << ", queued_at = $2, completed_at = NULL, cancelled_at = NULL, error = NULL";
  } else if (status == TaskStatus::Running) {
    set << ", started_at = COALESCE(started_at, $2)";
  } else if (status == TaskStatus::Completed) {
    params.push_back(pg_json_param(output, true));
    set << ", completed_at = $2, output = $3::jsonb, error = NULL";
  } else if (status == TaskStatus::Failed) {
    params.push_back(error);
    set << ", completed_at = $2, error = $3";
  } else if (status == TaskStatus::Cancelled) {
    set << ", cancelled_at = $2, completed_at = $2";
  } else if (status == TaskStatus::Interrupted) {
    params.push_back(error);
    set << ", error = $3";
  }
  params.push_back(task_id);
  const auto rows = client_->query("UPDATE " + tasks_table() + " SET " + set.str() +
                                      " WHERE id = $" + std::to_string(params.size()) + " RETURNING *",
                                  params);
  return task_from_pg_row(require_pg_row(rows, "Task not found: " + task_id));
}

AgentTaskRun PgTaskStore::append_run(const std::string& task_id, int lease_ms) {
  ensure_ready();
  const auto now = std::chrono::system_clock::now();
  const auto count_rows = client_->query("SELECT COUNT(*)::text AS count FROM " + runs_table() +
                                            " WHERE task_id = $1",
                                        {task_id});
  const int attempt = (count_rows.empty() ? 0 : pg_count_from_row(count_rows.front())) + 1;
  const auto lease_expires = now + std::chrono::milliseconds(lease_ms);
  const auto id = generate_uuid();
  const auto rows = client_->query(
      "INSERT INTO " + runs_table() + " ("
      "id, task_id, status, started_at, completed_at, output, error, lease_expires_at, heartbeat_at, attempt"
      ") VALUES ($1, $2, 'running', $3, NULL, NULL, NULL, $4, $3, $5) RETURNING *",
      {id, task_id, pg_time_param(now), pg_time_param(lease_expires), attempt});
  return run_from_pg_row(require_pg_row(rows, "Task run insert returned no row: " + id));
}

AgentTaskRun PgTaskStore::update_run(const std::string& task_id, const std::string& run_id,
                                     TaskStatus status, Value output, std::string error, int lease_ms) {
  ensure_ready();
  const auto now = std::chrono::system_clock::now();
  std::vector<Value> params{to_string(status), pg_time_param(now)};
  std::ostringstream set;
  set << "status = $1, heartbeat_at = $2";
  if (lease_ms > 0) {
    params.push_back(pg_time_param(now + std::chrono::milliseconds(lease_ms)));
    set << ", lease_expires_at = $3";
  }
  if (status == TaskStatus::Completed) {
    params.push_back(pg_json_param(output, true));
    set << ", completed_at = $2, output = $" << params.size() << "::jsonb";
  } else if (status == TaskStatus::Failed || status == TaskStatus::Interrupted ||
             status == TaskStatus::Cancelled) {
    params.push_back(error);
    set << ", completed_at = $2, error = $" << params.size();
  }
  params.push_back(task_id);
  params.push_back(run_id);
  const auto rows = client_->query("UPDATE " + runs_table() + " SET " + set.str() +
                                      " WHERE task_id = $" + std::to_string(params.size() - 1) +
                                      " AND id = $" + std::to_string(params.size()) + " RETURNING *",
                                  params);
  return run_from_pg_row(require_pg_row(rows, "Task run not found: " + run_id));
}

AgentTaskStep PgTaskStore::append_step(AgentTaskStep step) {
  ensure_ready();
  if (step.id.empty()) {
    step.id = generate_uuid();
  }
  if (step.started_at.time_since_epoch().count() == 0) {
    step.started_at = std::chrono::system_clock::now();
  }
  const auto rows = client_->query(
      "INSERT INTO " + steps_table() + " ("
      "id, task_id, run_id, name, status, input, output, error, started_at, completed_at"
      ") VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7::jsonb, $8, $9, $10) RETURNING *",
      {step.id, step.task_id, step.run_id, step.name, to_string(step.status), pg_json_param(step.input, true),
       pg_json_param(step.output, true), step.error, pg_time_param(step.started_at),
       pg_optional_time_param(step.completed_at)});
  return step_from_pg_row(require_pg_row(rows, "Task step insert returned no row: " + step.id));
}

TaskEvent PgTaskStore::append_event(std::string task_id, std::string run_id, std::string type, Value payload) {
  ensure_ready();
  const auto id = generate_uuid();
  const auto now = std::chrono::system_clock::now();
  const auto rows = client_->query(
      "INSERT INTO " + events_table() + " (id, task_id, run_id, type, payload, created_at) "
      "VALUES ($1, $2, $3, $4, $5::jsonb, $6) RETURNING *",
      {id, task_id, run_id, type, pg_json_param(payload, true), pg_time_param(now)});
  return event_from_pg_row(require_pg_row(rows, "Task event insert returned no row: " + id));
}

TaskCheckpoint PgTaskStore::append_checkpoint(std::string task_id, std::string run_id,
                                              std::string name, Value state) {
  ensure_ready();
  const auto id = generate_uuid();
  const auto now = std::chrono::system_clock::now();
  const auto rows = client_->query(
      "INSERT INTO " + checkpoints_table() + " (id, task_id, run_id, name, state, created_at) "
      "VALUES ($1, $2, $3, $4, $5::jsonb, $6) RETURNING *",
      {id, task_id, run_id, name, pg_json_param(state), pg_time_param(now)});
  return checkpoint_from_pg_row(require_pg_row(rows, "Task checkpoint insert returned no row: " + id));
}

void PgTaskStore::close() {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (close_client_on_close_ && client_) {
    client_->close();
    close_client_on_close_ = false;
  }
}

std::string PgTaskStore::table_name(const std::string& suffix) const {
  return pg_identifier(schema_name_) + "." + pg_identifier(table_prefix_ + suffix);
}

std::string PgTaskStore::tasks_table() const {
  return table_name("_tasks");
}

std::string PgTaskStore::runs_table() const {
  return table_name("_task_runs");
}

std::string PgTaskStore::steps_table() const {
  return table_name("_task_steps");
}

std::string PgTaskStore::events_table() const {
  return table_name("_task_events");
}

std::string PgTaskStore::checkpoints_table() const {
  return table_name("_task_checkpoints");
}

std::vector<std::string> PgTaskStore::scope_conditions(const TaskScopeFilter& filter,
                                                       std::vector<Value>& params,
                                                       const std::string& alias,
                                                       bool include_status) const {
  const std::string prefix = alias.empty() ? "" : alias + ".";
  std::vector<std::string> conditions;
  if (!filter.type.empty()) {
    params.push_back(filter.type);
    conditions.push_back(prefix + "type = $" + std::to_string(params.size()));
  }
  if (filter.owner_api_key_id) {
    params.push_back(*filter.owner_api_key_id);
    conditions.push_back(prefix + "owner_api_key_id = $" + std::to_string(params.size()));
  }
  if (filter.tenant_id) {
    params.push_back(*filter.tenant_id);
    conditions.push_back(prefix + "tenant_id = $" + std::to_string(params.size()));
  }
  if (include_status && filter.status) {
    params.push_back(to_string(*filter.status));
    conditions.push_back(prefix + "status = $" + std::to_string(params.size()));
  }
  return conditions;
}

void PgTaskStore::ensure_ready() const {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  if (ready_ || !create_tables_) {
    return;
  }
  client_->query("CREATE SCHEMA IF NOT EXISTS " + pg_identifier(schema_name_));
  client_->query("CREATE TABLE IF NOT EXISTS " + tasks_table() +
                 " (id TEXT PRIMARY KEY, type TEXT NOT NULL, input JSONB NOT NULL, "
                 "idempotency_key TEXT, owner_api_key_id TEXT, tenant_id TEXT, status TEXT NOT NULL, "
                 "created_at TIMESTAMPTZ NOT NULL, updated_at TIMESTAMPTZ NOT NULL, queued_at TIMESTAMPTZ, "
                 "started_at TIMESTAMPTZ, completed_at TIMESTAMPTZ, cancelled_at TIMESTAMPTZ, "
                 "output JSONB, error TEXT, metadata JSONB NOT NULL DEFAULT '{}'::jsonb)");
  client_->query("CREATE UNIQUE INDEX IF NOT EXISTS " + pg_identifier(table_prefix_ + "_tasks_idempotency_idx") +
                 " ON " + tasks_table() +
                 " (type, idempotency_key, COALESCE(owner_api_key_id, ''), COALESCE(tenant_id, '')) "
                 "WHERE idempotency_key IS NOT NULL");
  client_->query("CREATE INDEX IF NOT EXISTS " + pg_identifier(table_prefix_ + "_tasks_claim_idx") +
                 " ON " + tasks_table() + " (status, type, owner_api_key_id, tenant_id, queued_at)");
  client_->query("CREATE TABLE IF NOT EXISTS " + runs_table() +
                 " (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES " + tasks_table() +
                 "(id) ON DELETE CASCADE, status TEXT NOT NULL, started_at TIMESTAMPTZ NOT NULL, "
                 "completed_at TIMESTAMPTZ, output JSONB, error TEXT, lease_expires_at TIMESTAMPTZ NOT NULL, "
                 "heartbeat_at TIMESTAMPTZ NOT NULL, attempt INTEGER NOT NULL)");
  client_->query("CREATE INDEX IF NOT EXISTS " + pg_identifier(table_prefix_ + "_task_runs_task_idx") +
                 " ON " + runs_table() + " (task_id, status, started_at)");
  client_->query("CREATE TABLE IF NOT EXISTS " + steps_table() +
                 " (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES " + tasks_table() +
                 "(id) ON DELETE CASCADE, run_id TEXT NOT NULL, name TEXT NOT NULL, status TEXT NOT NULL, "
                 "input JSONB, output JSONB, error TEXT, started_at TIMESTAMPTZ NOT NULL, completed_at TIMESTAMPTZ)");
  client_->query("CREATE TABLE IF NOT EXISTS " + events_table() +
                 " (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES " + tasks_table() +
                 "(id) ON DELETE CASCADE, run_id TEXT, type TEXT NOT NULL, payload JSONB, created_at TIMESTAMPTZ NOT NULL)");
  client_->query("CREATE INDEX IF NOT EXISTS " + pg_identifier(table_prefix_ + "_task_events_task_idx") +
                 " ON " + events_table() + " (task_id, created_at)");
  client_->query("CREATE TABLE IF NOT EXISTS " + checkpoints_table() +
                 " (id TEXT PRIMARY KEY, task_id TEXT NOT NULL REFERENCES " + tasks_table() +
                 "(id) ON DELETE CASCADE, run_id TEXT, name TEXT NOT NULL, state JSONB NOT NULL, "
                 "created_at TIMESTAMPTZ NOT NULL)");
  ready_ = true;
}

void PgTaskStore::requeue_expired_runs(PgTaskClient& client,
                                       std::chrono::system_clock::time_point now,
                                       const TaskScopeFilter& filter) const {
  std::vector<Value> params{pg_time_param(now)};
  auto conditions = scope_conditions(filter, params, "t");
  conditions.insert(conditions.begin(), "r.lease_expires_at <= $1");
  conditions.insert(conditions.begin(), "t.status = 'running'");
  std::ostringstream sql;
  sql << "SELECT t.id AS task_id, r.id AS run_id FROM " << tasks_table()
      << " t JOIN LATERAL (SELECT id, lease_expires_at FROM " << runs_table()
      << " WHERE task_id = t.id AND status = 'running' ORDER BY started_at DESC LIMIT 1) r ON true WHERE ";
  for (std::size_t index = 0; index < conditions.size(); ++index) {
    if (index > 0) {
      sql << " AND ";
    }
    sql << conditions[index];
  }
  sql << " FOR UPDATE OF t";
  for (const auto& row : client.query(sql.str(), params)) {
    const auto task_id = row.at("task_id").as_string();
    const auto run_id = row.at("run_id").as_string();
    client.query("UPDATE " + runs_table() +
                     " SET status = 'interrupted', completed_at = $3, error = 'Task run lease expired.' "
                     "WHERE task_id = $1 AND id = $2",
                 {task_id, run_id, pg_time_param(now)});
    client.query("UPDATE " + tasks_table() +
                     " SET status = 'queued', queued_at = $2, updated_at = $2 WHERE id = $1",
                 {task_id, pg_time_param(now)});
  }
}

FileTaskStore::FileTaskStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

FileTaskStore::FileTaskStore(FileTaskStoreConfig config)
    : FileTaskStore(std::move(config.file_path)) {}

AgentTask FileTaskStore::create_task(CreateTaskInput input) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::create_task(std::move(input));
  persist();
  return result;
}

std::optional<TaskStoreSnapshot> FileTaskStore::get_task(const std::string& task_id) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const_cast<FileTaskStore*>(this)->ensure_loaded();
  return InMemoryTaskStore::get_task(task_id);
}

std::optional<TaskStoreSnapshot> FileTaskStore::find_task_by_idempotency_key(
    const std::string& idempotency_key, const TaskScopeFilter& filter) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const_cast<FileTaskStore*>(this)->ensure_loaded();
  return InMemoryTaskStore::find_task_by_idempotency_key(idempotency_key, filter);
}

std::vector<AgentTask> FileTaskStore::list_tasks(const TaskScopeFilter& filter) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const_cast<FileTaskStore*>(this)->ensure_loaded();
  return InMemoryTaskStore::list_tasks(filter);
}

std::optional<TaskQueueClaim> FileTaskStore::claim_task(const TaskScopeFilter& filter, int lease_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::claim_task(filter, lease_ms);
  persist();
  return result;
}

AgentTask FileTaskStore::update_task_status(const std::string& task_id, TaskStatus status, Value output,
                                            std::string error) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::update_task_status(task_id, status, std::move(output), std::move(error));
  persist();
  return result;
}

AgentTaskRun FileTaskStore::append_run(const std::string& task_id, int lease_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::append_run(task_id, lease_ms);
  persist();
  return result;
}

AgentTaskRun FileTaskStore::update_run(const std::string& task_id, const std::string& run_id, TaskStatus status,
                                       Value output, std::string error, int lease_ms) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::update_run(task_id, run_id, status, std::move(output), std::move(error), lease_ms);
  persist();
  return result;
}

AgentTaskStep FileTaskStore::append_step(AgentTaskStep step) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::append_step(std::move(step));
  persist();
  return result;
}

TaskEvent FileTaskStore::append_event(std::string task_id, std::string run_id, std::string type, Value payload) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::append_event(std::move(task_id), std::move(run_id), std::move(type), std::move(payload));
  persist();
  return result;
}

TaskCheckpoint FileTaskStore::append_checkpoint(std::string task_id, std::string run_id, std::string name, Value state) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryTaskStore::append_checkpoint(std::move(task_id), std::move(run_id), std::move(name), std::move(state));
  persist();
  return result;
}

void FileTaskStore::ensure_loaded() {
  if (loaded_) return;
  loaded_ = true;
  if (!std::filesystem::exists(file_path_)) return;
  const auto raw = read_json_file(file_path_);
  for (const auto& item : raw.at("tasks").as_array()) {
    auto task = task_from_value(item);
    const auto task_id = task.id;
    tasks_[task_id] = std::move(task);
  }
  for (const auto& item : raw.at("runs").as_array()) {
    auto run = run_from_value(item);
    const auto task_id = run.task_id;
    runs_[task_id].push_back(std::move(run));
  }
  for (const auto& item : raw.at("steps").as_array()) {
    auto step = step_from_value(item);
    steps_[step.task_id].push_back(std::move(step));
  }
  for (const auto& item : raw.at("events").as_array()) {
    auto event = event_from_value(item);
    events_[event.task_id].push_back(std::move(event));
  }
  for (const auto& item : raw.at("checkpoints").as_array()) {
    auto checkpoint = checkpoint_from_value(item);
    checkpoints_[checkpoint.task_id].push_back(std::move(checkpoint));
  }
}

void FileTaskStore::persist() const {
  Value::Array tasks;
  for (const auto& [_, task] : tasks_) tasks.push_back(task_to_value(task));
  Value::Array runs;
  for (const auto& [_, items] : runs_) {
    for (const auto& run : items) runs.push_back(run_to_value(run));
  }
  Value::Array steps;
  for (const auto& [_, items] : steps_) {
    for (const auto& step : items) steps.push_back(step_to_value(step));
  }
  Value::Array events;
  for (const auto& [_, items] : events_) {
    for (const auto& event : items) events.push_back(event_to_value(event));
  }
  Value::Array checkpoints;
  for (const auto& [_, items] : checkpoints_) {
    for (const auto& checkpoint : items) checkpoints.push_back(checkpoint_to_value(checkpoint));
  }
  write_json_file(file_path_, Value::object({{"tasks", Value(tasks)}, {"runs", Value(runs)}, {"steps", Value(steps)},
                                             {"events", Value(events)}, {"checkpoints", Value(checkpoints)}}));
}
}  // namespace agent
