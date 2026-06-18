#include "agent/tool_runs.hpp"

#include <algorithm>
#include <chrono>

namespace agent {

namespace {

Value merge_metadata_values(const Value& base, const Value& overlay) {
  if (!base.is_object()) {
    return overlay.is_object() ? overlay : Value::object({});
  }
  if (!overlay.is_object()) {
    return base;
  }
  Value merged = base;
  for (const auto& [key, value] : overlay.as_object()) {
    merged[key] = value;
  }
  return merged;
}

bool snapshot_matches(const ToolRunSnapshot& snapshot, const ToolRunListOptions& options) {
  if (options.status && snapshot.status != *options.status) {
    return false;
  }
  if (options.active_only && tool_run_status_is_terminal(snapshot.status)) {
    return false;
  }
  if (!options.kind.empty() && snapshot.kind != options.kind) {
    return false;
  }
  if (!options.tool_name.empty() && snapshot.tool_name != options.tool_name) {
    return false;
  }
  return true;
}

bool wait_condition_met(const ToolRunSnapshot& snapshot, const ToolRunWaitOptions& options) {
  if (options.status && snapshot.status == *options.status) {
    return true;
  }
  if (options.ready && snapshot.ready == *options.ready) {
    return true;
  }
  if (options.terminal && tool_run_status_is_terminal(snapshot.status) == *options.terminal) {
    return true;
  }
  switch (options.until) {
    case ToolRunWaitUntil::Ready:
      return snapshot.ready || tool_run_status_is_terminal(snapshot.status);
    case ToolRunWaitUntil::Terminal:
      return tool_run_status_is_terminal(snapshot.status);
  }
  return false;
}

}  // namespace

std::string to_string(ToolRunStatus status) {
  switch (status) {
    case ToolRunStatus::Queued:
      return "queued";
    case ToolRunStatus::Running:
      return "running";
    case ToolRunStatus::Waiting:
      return "waiting";
    case ToolRunStatus::Completed:
      return "completed";
    case ToolRunStatus::Failed:
      return "failed";
    case ToolRunStatus::Cancelled:
      return "cancelled";
  }
  return "running";
}

ToolRunStatus tool_run_status_from_string(const std::string& value, ToolRunStatus fallback) {
  if (value == "queued") return ToolRunStatus::Queued;
  if (value == "running") return ToolRunStatus::Running;
  if (value == "waiting") return ToolRunStatus::Waiting;
  if (value == "completed") return ToolRunStatus::Completed;
  if (value == "failed") return ToolRunStatus::Failed;
  if (value == "cancelled") return ToolRunStatus::Cancelled;
  return fallback;
}

bool tool_run_status_is_terminal(ToolRunStatus status) {
  return status == ToolRunStatus::Completed || status == ToolRunStatus::Failed ||
         status == ToolRunStatus::Cancelled;
}

Value tool_run_snapshot_to_value(const ToolRunSnapshot& snapshot) {
  return Value::object({
      {"runId", snapshot.run_id},
      {"toolCallId", snapshot.tool_call_id},
      {"toolName", snapshot.tool_name},
      {"kind", snapshot.kind},
      {"label", snapshot.label},
      {"status", to_string(snapshot.status)},
      {"startedAt", snapshot.started_at},
      {"updatedAt", snapshot.updated_at},
      {"finishedAt", snapshot.finished_at.empty() ? Value() : Value(snapshot.finished_at)},
      {"ready", snapshot.ready},
      {"error", snapshot.error.empty() ? Value() : Value(snapshot.error)},
      {"metadata", snapshot.metadata.is_object() ? snapshot.metadata : Value::object({})},
  });
}

Value tool_run_event_to_value(const ToolRunEvent& event) {
  return Value::object({
      {"runId", event.run_id},
      {"sequence", static_cast<long long>(event.sequence)},
      {"type", event.type},
      {"stream", event.stream.empty() ? Value() : Value(event.stream)},
      {"text", event.text.empty() ? Value() : Value(event.text)},
      {"message", event.message.empty() ? Value() : Value(event.message)},
      {"payload", event.payload.is_null() ? Value::object({}) : event.payload},
      {"metadata", event.metadata.is_object() ? event.metadata : Value::object({})},
      {"createdAt", event.created_at},
  });
}

Value tool_run_read_result_to_value(const ToolRunReadResult& result) {
  Value::Array events;
  Value::Array logs;
  events.reserve(result.events.size());
  for (const auto& event : result.events) {
    auto value = tool_run_event_to_value(event);
    if (event.type == "log") {
      logs.push_back(value);
    }
    events.push_back(std::move(value));
  }
  return Value::object({
      {"run", tool_run_snapshot_to_value(result.run)},
      {"handle", tool_run_snapshot_to_value(result.run)},
      {"cursor", static_cast<long long>(result.cursor)},
      {"nextCursor", static_cast<long long>(result.next_cursor)},
      {"hasMore", result.has_more},
      {"events", Value(std::move(events))},
      {"logs", Value(std::move(logs))},
  });
}

Value tool_run_snapshots_to_value(const std::vector<ToolRunSnapshot>& snapshots) {
  Value::Array values;
  values.reserve(snapshots.size());
  for (const auto& snapshot : snapshots) {
    values.push_back(tool_run_snapshot_to_value(snapshot));
  }
  return Value(std::move(values));
}

InMemoryToolRunManager::Entry& InMemoryToolRunManager::require_locked(const std::string& run_id) {
  const auto found = runs_.find(run_id);
  if (found == runs_.end()) {
    throw ConfigurationError("Unknown tool run: " + run_id);
  }
  return found->second;
}

const InMemoryToolRunManager::Entry& InMemoryToolRunManager::require_locked(const std::string& run_id) const {
  const auto found = runs_.find(run_id);
  if (found == runs_.end()) {
    throw ConfigurationError("Unknown tool run: " + run_id);
  }
  return found->second;
}

ToolRunEvent InMemoryToolRunManager::append_event_locked(Entry& entry, ToolRunEventInput event) {
  ToolRunEvent stored;
  stored.run_id = entry.snapshot.run_id;
  stored.sequence = entry.events.empty() ? 1 : entry.events.back().sequence + 1;
  stored.type = event.type.empty() ? "event" : std::move(event.type);
  stored.stream = std::move(event.stream);
  stored.text = std::move(event.text);
  stored.message = std::move(event.message);
  stored.payload = event.payload.is_null() ? Value::object({}) : std::move(event.payload);
  stored.metadata = event.metadata.is_object() ? std::move(event.metadata) : Value::object({});
  stored.created_at = now_iso8601();
  entry.events.push_back(stored);
  entry.snapshot.updated_at = stored.created_at;
  return stored;
}

ToolRunSnapshot InMemoryToolRunManager::start(ToolRunStartOptions options) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::string run_id = options.run_id.empty() ? generate_uuid() : std::move(options.run_id);
  if (runs_.count(run_id) > 0) {
    throw ConfigurationError("Tool run already exists: " + run_id);
  }
  const auto now = now_iso8601();
  Entry entry;
  entry.snapshot.run_id = run_id;
  entry.snapshot.tool_call_id = std::move(options.tool_call_id);
  entry.snapshot.tool_name = std::move(options.tool_name);
  entry.snapshot.kind = options.kind.empty() ? "custom" : std::move(options.kind);
  entry.snapshot.label = std::move(options.label);
  entry.snapshot.status = options.status;
  entry.snapshot.ready = options.ready;
  entry.snapshot.started_at = now;
  entry.snapshot.updated_at = now;
  if (tool_run_status_is_terminal(entry.snapshot.status)) {
    entry.snapshot.finished_at = now;
  }
  entry.snapshot.metadata = options.metadata.is_object() ? std::move(options.metadata) : Value::object({});
  entry.cancel = std::move(options.cancel);
  auto [it, _] = runs_.emplace(entry.snapshot.run_id, std::move(entry));
  auto& stored = it->second;
  append_event_locked(stored, ToolRunEventInput{
                                  .type = "lifecycle.started",
                                  .message = "Tool run started.",
                                  .payload = tool_run_snapshot_to_value(stored.snapshot),
                              });
  cv_.notify_all();
  return stored.snapshot;
}

std::optional<ToolRunSnapshot> InMemoryToolRunManager::get(const std::string& run_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = runs_.find(run_id);
  if (found == runs_.end()) {
    return std::nullopt;
  }
  return found->second.snapshot;
}

std::vector<ToolRunSnapshot> InMemoryToolRunManager::list(ToolRunListOptions options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolRunSnapshot> snapshots;
  for (const auto& [_, entry] : runs_) {
    if (snapshot_matches(entry.snapshot, options)) {
      snapshots.push_back(entry.snapshot);
    }
  }
  std::sort(snapshots.begin(), snapshots.end(), [](const ToolRunSnapshot& left,
                                                   const ToolRunSnapshot& right) {
    return left.updated_at > right.updated_at;
  });
  if (options.limit > 0 && snapshots.size() > options.limit) {
    snapshots.resize(options.limit);
  }
  return snapshots;
}

ToolRunSnapshot InMemoryToolRunManager::update(const std::string& run_id, ToolRunUpdate update) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& entry = require_locked(run_id);
  if (update.status) {
    entry.snapshot.status = *update.status;
    if (tool_run_status_is_terminal(*update.status) && entry.snapshot.finished_at.empty()) {
      entry.snapshot.finished_at = now_iso8601();
    }
  }
  if (update.ready) {
    entry.snapshot.ready = *update.ready;
  }
  if (update.error) {
    entry.snapshot.error = *update.error;
  }
  if (update.metadata) {
    entry.snapshot.metadata = update.merge_metadata
                                  ? merge_metadata_values(entry.snapshot.metadata, *update.metadata)
                                  : (*update.metadata).is_object() ? *update.metadata : Value::object({});
  }
  entry.snapshot.updated_at = now_iso8601();
  append_event_locked(entry, ToolRunEventInput{
                                 .type = "lifecycle.updated",
                                 .message = "Tool run updated.",
                                 .payload = tool_run_snapshot_to_value(entry.snapshot),
                             });
  cv_.notify_all();
  return entry.snapshot;
}

ToolRunEvent InMemoryToolRunManager::append_event(const std::string& run_id, ToolRunEventInput event) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& entry = require_locked(run_id);
  auto stored = append_event_locked(entry, std::move(event));
  cv_.notify_all();
  return stored;
}

ToolRunEvent InMemoryToolRunManager::append_log(const std::string& run_id,
                                                std::string stream,
                                                std::string text,
                                                Value metadata) {
  return append_event(run_id, ToolRunEventInput{
                                  .type = "log",
                                  .stream = std::move(stream),
                                  .text = std::move(text),
                                  .metadata = std::move(metadata),
                              });
}

ToolRunReadResult InMemoryToolRunManager::read(const std::string& run_id, ToolRunReadOptions options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto& entry = require_locked(run_id);
  ToolRunReadResult result;
  result.run = entry.snapshot;
  result.cursor = options.cursor;

  std::vector<ToolRunEvent> selected;
  const auto limit = options.limit == 0 ? static_cast<std::size_t>(100) : options.limit;
  const bool tail_requested = options.tail > 0;
  for (const auto& event : entry.events) {
    const bool is_log = event.type == "log";
    if ((is_log && !options.include_logs) || (!is_log && !options.include_events)) {
      continue;
    }
    if (!tail_requested && event.sequence <= options.cursor) {
      continue;
    }
    selected.push_back(event);
  }
  if (options.tail > 0 && selected.size() > options.tail) {
    selected.erase(selected.begin(), selected.end() - static_cast<std::ptrdiff_t>(options.tail));
  }
  result.has_more = !tail_requested && selected.size() > limit;
  if (selected.size() > limit) {
    selected.resize(limit);
  }
  result.events = std::move(selected);
  result.next_cursor = result.cursor;
  if (!result.events.empty()) {
    result.next_cursor = result.events.back().sequence;
  }
  return result;
}

ToolRunSnapshot InMemoryToolRunManager::cancel(const std::string& run_id, std::string reason) {
  std::function<void(const std::string&)> cancel_callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& entry = require_locked(run_id);
    if (tool_run_status_is_terminal(entry.snapshot.status)) {
      return entry.snapshot;
    }
    cancel_callback = entry.cancel;
    entry.snapshot.status = ToolRunStatus::Cancelled;
    entry.snapshot.finished_at = now_iso8601();
    entry.snapshot.updated_at = entry.snapshot.finished_at;
    if (!reason.empty()) {
      entry.snapshot.error = reason;
    }
    append_event_locked(entry, ToolRunEventInput{
                                   .type = "lifecycle.cancelled",
                                   .message = reason.empty() ? "Tool run cancelled." : reason,
                                   .payload = tool_run_snapshot_to_value(entry.snapshot),
                               });
    cv_.notify_all();
  }
  if (cancel_callback) {
    try {
      cancel_callback(reason);
    } catch (const std::exception& error) {
      append_event(run_id, ToolRunEventInput{
                               .type = "cancel.error",
                               .message = error.what(),
                           });
    }
  }
  auto snapshot = get(run_id);
  if (!snapshot) {
    throw ConfigurationError("Unknown tool run: " + run_id);
  }
  return *snapshot;
}

std::optional<ToolRunSnapshot> InMemoryToolRunManager::wait(const std::string& run_id,
                                                            ToolRunWaitOptions options) const {
  std::unique_lock<std::mutex> lock(mutex_);
  const auto predicate = [&]() {
    const auto found = runs_.find(run_id);
    return found == runs_.end() || wait_condition_met(found->second.snapshot, options);
  };
  if (options.timeout_ms > 0) {
    const bool completed = cv_.wait_for(lock, std::chrono::milliseconds(options.timeout_ms), predicate);
    if (!completed) {
      return std::nullopt;
    }
  } else {
    cv_.wait(lock, predicate);
  }
  const auto found = runs_.find(run_id);
  if (found == runs_.end()) {
    return std::nullopt;
  }
  return found->second.snapshot;
}

}  // namespace agent
