#pragma once

#include "agent/core.hpp"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace agent {

enum class ToolRunStatus {
  Queued,
  Running,
  Waiting,
  Completed,
  Failed,
  Cancelled,
};

std::string to_string(ToolRunStatus status);
ToolRunStatus tool_run_status_from_string(const std::string& value,
                                          ToolRunStatus fallback = ToolRunStatus::Running);
bool tool_run_status_is_terminal(ToolRunStatus status);

enum class ToolRunWaitUntil {
  Ready,
  Terminal,
};

struct ToolRunSnapshot {
  std::string run_id;
  std::string tool_call_id;
  std::string tool_name;
  std::string kind = "custom";
  std::string label;
  ToolRunStatus status = ToolRunStatus::Running;
  std::string started_at;
  std::string updated_at;
  std::string finished_at;
  bool ready = false;
  std::string error;
  Value metadata = Value::object({});
};

struct ToolRunStartOptions {
  std::string run_id;
  std::string tool_call_id;
  std::string tool_name;
  std::string kind = "custom";
  std::string label;
  ToolRunStatus status = ToolRunStatus::Running;
  bool ready = false;
  Value metadata = Value::object({});
  std::function<void(const std::string& reason)> cancel;
};

struct ToolRunUpdate {
  std::optional<ToolRunStatus> status;
  std::optional<bool> ready;
  std::optional<std::string> error;
  std::optional<Value> metadata;
  bool merge_metadata = true;
};

struct ToolRunEvent {
  std::string run_id;
  std::uint64_t sequence = 0;
  std::string type = "event";
  std::string stream;
  std::string text;
  std::string message;
  Value payload = Value::object({});
  Value metadata = Value::object({});
  std::string created_at;
};

struct ToolRunEventInput {
  std::string type = "event";
  std::string stream;
  std::string text;
  std::string message;
  Value payload = Value::object({});
  Value metadata = Value::object({});
};

struct ToolRunReadOptions {
  std::uint64_t cursor = 0;
  std::size_t limit = 100;
  std::size_t tail = 0;
  bool include_events = true;
  bool include_logs = true;
};

struct ToolRunReadResult {
  ToolRunSnapshot run;
  std::uint64_t cursor = 0;
  std::uint64_t next_cursor = 0;
  bool has_more = false;
  // All events since the requested cursor/tail/limit window. Log records are
  // first-class events with type == "log"; JSON serializers also expose a
  // convenience `logs` array filtered from this collection.
  std::vector<ToolRunEvent> events;
};

struct ToolRunListOptions {
  std::optional<ToolRunStatus> status;
  std::string kind;
  std::string tool_name;
  bool active_only = false;
  std::size_t limit = 100;
};

struct ToolRunWaitOptions {
  ToolRunWaitUntil until = ToolRunWaitUntil::Terminal;
  std::optional<ToolRunStatus> status;
  std::optional<bool> ready;
  std::optional<bool> terminal;
  int timeout_ms = 0;
};

Value tool_run_snapshot_to_value(const ToolRunSnapshot& snapshot);
Value tool_run_event_to_value(const ToolRunEvent& event);
Value tool_run_read_result_to_value(const ToolRunReadResult& result);
Value tool_run_snapshots_to_value(const std::vector<ToolRunSnapshot>& snapshots);

class ToolRunManager {
 public:
  virtual ~ToolRunManager() = default;

  virtual ToolRunSnapshot start(ToolRunStartOptions options) = 0;
  virtual std::optional<ToolRunSnapshot> get(const std::string& run_id) const = 0;
  virtual std::vector<ToolRunSnapshot> list(ToolRunListOptions options = {}) const = 0;
  virtual ToolRunSnapshot update(const std::string& run_id, ToolRunUpdate update) = 0;
  virtual ToolRunEvent append_event(const std::string& run_id, ToolRunEventInput event) = 0;
  virtual ToolRunEvent append_log(const std::string& run_id,
                                  std::string stream,
                                  std::string text,
                                  Value metadata = Value::object({})) = 0;
  virtual ToolRunReadResult read(const std::string& run_id, ToolRunReadOptions options = {}) const = 0;
  virtual ToolRunSnapshot cancel(const std::string& run_id, std::string reason = {}) = 0;
  virtual std::optional<ToolRunSnapshot> wait(const std::string& run_id,
                                              ToolRunWaitOptions options = {}) const = 0;
};

class InMemoryToolRunManager final : public ToolRunManager {
 public:
  ToolRunSnapshot start(ToolRunStartOptions options) override;
  std::optional<ToolRunSnapshot> get(const std::string& run_id) const override;
  std::vector<ToolRunSnapshot> list(ToolRunListOptions options = {}) const override;
  ToolRunSnapshot update(const std::string& run_id, ToolRunUpdate update) override;
  ToolRunEvent append_event(const std::string& run_id, ToolRunEventInput event) override;
  ToolRunEvent append_log(const std::string& run_id,
                          std::string stream,
                          std::string text,
                          Value metadata = Value::object({})) override;
  ToolRunReadResult read(const std::string& run_id, ToolRunReadOptions options = {}) const override;
  ToolRunSnapshot cancel(const std::string& run_id, std::string reason = {}) override;
  std::optional<ToolRunSnapshot> wait(const std::string& run_id,
                                      ToolRunWaitOptions options = {}) const override;

 private:
  struct Entry {
    ToolRunSnapshot snapshot;
    std::vector<ToolRunEvent> events;
    std::function<void(const std::string& reason)> cancel;
  };

  Entry& require_locked(const std::string& run_id);
  const Entry& require_locked(const std::string& run_id) const;
  ToolRunEvent append_event_locked(Entry& entry, ToolRunEventInput event);

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::map<std::string, Entry> runs_;
};

}  // namespace agent
