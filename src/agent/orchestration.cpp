#include "agent/orchestration.hpp"
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

namespace {

ArtifactLogRecord make_artifact_log_record(std::string key, std::string event, Value value = {}) {
  return ArtifactLogRecord{generate_uuid(), std::move(key), std::move(event), std::move(value), now_iso8601()};
}

}  // namespace

InMemoryArtifactStore::InMemoryArtifactStore(std::vector<ArtifactRecord> initial) {
  for (auto& record : initial) {
    records_[record.key] = std::move(record);
  }
}

std::optional<ArtifactRecord> InMemoryArtifactStore::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(key);
  return found == records_.end() ? std::nullopt : std::optional<ArtifactRecord>(found->second);
}

ArtifactRecord InMemoryArtifactStore::set(std::string key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  ArtifactRecord record{key, value, now_iso8601()};
  records_[key] = record;
  history_.push_back(make_artifact_log_record(std::move(key), "set", std::move(value)));
  return record;
}

std::vector<ArtifactRecord> InMemoryArtifactStore::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ArtifactRecord> records;
  records.reserve(records_.size());
  for (const auto& [_, record] : records_) {
    records.push_back(record);
  }
  return records;
}

void InMemoryArtifactStore::clear(std::string key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (key.empty()) {
    records_.clear();
    history_.push_back(make_artifact_log_record("*", "clear-all"));
    return;
  }
  records_.erase(key);
  history_.push_back(make_artifact_log_record(std::move(key), "clear"));
}

ArtifactLogRecord InMemoryArtifactStore::append_log(std::string key, std::string event, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  ArtifactLogRecord record = make_artifact_log_record(std::move(key), std::move(event), std::move(value));
  history_.push_back(record);
  return record;
}

std::vector<ArtifactLogRecord> InMemoryArtifactStore::history(std::string key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (key.empty()) {
    return history_;
  }
  std::vector<ArtifactLogRecord> records;
  std::copy_if(history_.begin(), history_.end(), std::back_inserter(records), [&](const auto& record) {
    return record.key == key;
  });
  return records;
}

ArtifactSnapshot InMemoryArtifactStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  ArtifactSnapshot snapshot;
  snapshot.records.reserve(records_.size());
  for (const auto& [_, record] : records_) {
    snapshot.records.push_back(record);
  }
  snapshot.history = history_;
  return snapshot;
}

namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

Value coordinator_result_value(const AgentRunnerRunResult& result) {
  return Value::object({{"text", result.text},
                        {"sessionId", result.session_id},
                        {"iterationCount", result.iteration_count},
                        {"terminationReason", to_string(result.termination_reason)}});
}

Value coordinator_error_value(const std::exception& error) {
  return Value::object({{"message", error.what()}});
}

Value evaluation_result_value(const EvaluationResult& evaluation) {
  return Value::object({{"accepted", evaluation.accepted},
                        {"score", evaluation.score},
                        {"feedback", evaluation.feedback.empty() ? Value() : Value(evaluation.feedback)},
                        {"metadata", evaluation.metadata}});
}

Value artifact_records_object(const InMemoryArtifactStore* store) {
  Value::Object object;
  if (!store) {
    return Value(std::move(object));
  }
  for (const auto& record : store->list()) {
    object[record.key] = record.value;
  }
  return Value(std::move(object));
}

std::string workflow_output_text(const WorkflowExecutionResult& result) {
  return result.output.is_string() ? result.output.as_string() : safe_json_stringify(result.output);
}

AgentRunner* require_worker_agent(const CoordinatorContext& context, const std::string& worker_id) {
  const auto found = context.worker_agents.find(worker_id);
  if (found == context.worker_agents.end() || !found->second) {
    throw AgentFrameworkError("Worker agent not found: " + worker_id);
  }
  return found->second;
}

Value artifact_record_to_value(const ArtifactRecord& record) {
  return Value::object({{"key", record.key}, {"value", record.value}, {"updatedAt", record.updated_at}});
}

ArtifactRecord artifact_record_from_value(const Value& value) {
  return ArtifactRecord{value.at("key").as_string(),
                        value.at("value"),
                        value.at("updatedAt").as_string()};
}

Value artifact_log_to_value(const ArtifactLogRecord& record) {
  return Value::object({{"id", record.id},
                        {"key", record.key},
                        {"event", record.event},
                        {"value", record.value},
                        {"at", record.at}});
}

ArtifactLogRecord artifact_log_from_value(const Value& value) {
  return ArtifactLogRecord{value.at("id").as_string(),
                           value.at("key").as_string(),
                           value.at("event").as_string(),
                           value.at("value"),
                           value.at("at").as_string()};
}

Value shared_state_record_to_value(const SharedStateRecord& record) {
  return Value::object({{"key", record.key}, {"value", record.value}, {"updatedAt", record.updated_at}});
}

SharedStateRecord shared_state_record_from_value(const Value& value) {
  return SharedStateRecord{value.at("key").as_string(),
                           value.at("value"),
                           value.at("updatedAt").as_string()};
}

}  // namespace

FileArtifactStore::FileArtifactStore(std::filesystem::path base_dir, std::string namespace_id)
    : base_dir_(std::move(base_dir)), namespace_id_(namespace_id.empty() ? "default" : std::move(namespace_id)) {}

FileArtifactStore::FileArtifactStore(FileArtifactStoreConfig config)
    : FileArtifactStore(std::move(config.base_dir), std::move(config.namespace_id)) {}

std::optional<ArtifactRecord> FileArtifactStore::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto snapshot = read_snapshot();
  const auto found = std::find_if(snapshot.records.begin(), snapshot.records.end(), [&](const auto& record) {
    return record.key == key;
  });
  return found == snapshot.records.end() ? std::nullopt : std::optional<ArtifactRecord>(*found);
}

ArtifactRecord FileArtifactStore::set(std::string key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = read_snapshot();
  ArtifactRecord record{key, value, now_iso8601()};
  const auto found = std::find_if(snapshot.records.begin(), snapshot.records.end(), [&](const auto& item) {
    return item.key == key;
  });
  if (found == snapshot.records.end()) {
    snapshot.records.push_back(record);
  } else {
    *found = record;
  }
  snapshot.history.push_back(make_artifact_log_record(std::move(key), "set", std::move(value)));
  write_snapshot(snapshot);
  return record;
}

std::vector<ArtifactRecord> FileArtifactStore::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return read_snapshot().records;
}

void FileArtifactStore::clear(std::string key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = read_snapshot();
  if (key.empty()) {
    snapshot.records.clear();
    snapshot.history.push_back(make_artifact_log_record("*", "clear-all"));
    write_snapshot(snapshot);
    return;
  }
  snapshot.records.erase(std::remove_if(snapshot.records.begin(), snapshot.records.end(), [&](const auto& record) {
    return record.key == key;
  }), snapshot.records.end());
  snapshot.history.push_back(make_artifact_log_record(std::move(key), "clear"));
  write_snapshot(snapshot);
}

ArtifactLogRecord FileArtifactStore::append_log(std::string key, std::string event, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto snapshot = read_snapshot();
  ArtifactLogRecord record = make_artifact_log_record(std::move(key), std::move(event), std::move(value));
  snapshot.history.push_back(record);
  write_snapshot(snapshot);
  return record;
}

std::vector<ArtifactLogRecord> FileArtifactStore::history(std::string key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto snapshot = read_snapshot();
  if (key.empty()) {
    return snapshot.history;
  }
  std::vector<ArtifactLogRecord> records;
  std::copy_if(snapshot.history.begin(), snapshot.history.end(), std::back_inserter(records), [&](const auto& record) {
    return record.key == key;
  });
  return records;
}

ArtifactSnapshot FileArtifactStore::snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return read_snapshot();
}

std::filesystem::path FileArtifactStore::file_path() const {
  return base_dir_ / (encode_uri_component(namespace_id_) + ".json");
}

std::filesystem::path FileArtifactStore::legacy_file_path() const {
  return base_dir_ / namespace_id_ / "artifacts.json";
}

ArtifactSnapshot FileArtifactStore::read_snapshot() const {
  auto path = file_path();
  if (!std::filesystem::exists(path)) {
    const auto legacy = legacy_file_path();
    if (std::filesystem::exists(legacy)) {
      path = legacy;
    }
  }
  if (!std::filesystem::exists(path)) {
    return {};
  }
  const auto raw = read_json_file(path);
  ArtifactSnapshot snapshot;
  for (const auto& item : raw.at("records").as_array()) {
    snapshot.records.push_back(artifact_record_from_value(item));
  }
  for (const auto& item : raw.at("history").as_array()) {
    snapshot.history.push_back(artifact_log_from_value(item));
  }
  return snapshot;
}

void FileArtifactStore::write_snapshot(const ArtifactSnapshot& snapshot) const {
  Value::Array records;
  for (const auto& record : snapshot.records) {
    records.push_back(artifact_record_to_value(record));
  }
  Value::Array history;
  for (const auto& record : snapshot.history) {
    history.push_back(artifact_log_to_value(record));
  }
  write_json_file(file_path(), Value::object({{"records", Value(records)}, {"history", Value(history)}}));
}

std::optional<SharedStateRecord> InMemorySharedStateStore::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(key);
  return found == records_.end() ? std::nullopt : std::optional<SharedStateRecord>(found->second);
}

SharedStateRecord InMemorySharedStateStore::set(std::string key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  SharedStateRecord record{key, std::move(value), now_iso8601()};
  records_[key] = record;
  return record;
}

SharedStateRecord InMemorySharedStateStore::merge(std::string key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = records_.find(key);
  Value merged = found == records_.end() ? Value::object({}) : found->second.value;
  if (!merged.is_object()) {
    merged = Value::object({});
  }
  if (value.is_object()) {
    for (const auto& [field, next] : value.as_object()) {
      merged[field] = next;
    }
  }
  SharedStateRecord record{key, std::move(merged), now_iso8601()};
  records_[key] = record;
  return record;
}

std::vector<SharedStateRecord> InMemorySharedStateStore::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SharedStateRecord> records;
  records.reserve(records_.size());
  for (const auto& [_, record] : records_) {
    records.push_back(record);
  }
  return records;
}

void InMemorySharedStateStore::clear(std::string key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (key.empty()) {
    records_.clear();
    return;
  }
  records_.erase(key);
}

FileSharedStateStore::FileSharedStateStore(std::filesystem::path base_dir, std::string namespace_id)
    : base_dir_(std::move(base_dir)), namespace_id_(namespace_id.empty() ? "default" : std::move(namespace_id)) {}

FileSharedStateStore::FileSharedStateStore(FileSharedStateStoreConfig config)
    : FileSharedStateStore(std::move(config.base_dir), std::move(config.namespace_id)) {}

std::optional<SharedStateRecord> FileSharedStateStore::get(const std::string& key) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto records = read_records();
  const auto found = records.find(key);
  return found == records.end() ? std::nullopt : std::optional<SharedStateRecord>(found->second);
}

SharedStateRecord FileSharedStateStore::set(std::string key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto records = read_records();
  SharedStateRecord record{key, std::move(value), now_iso8601()};
  records[key] = record;
  write_records(records);
  return record;
}

SharedStateRecord FileSharedStateStore::merge(std::string key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto records = read_records();
  Value merged = records.contains(key) ? records.at(key).value : Value::object({});
  if (!merged.is_object()) {
    merged = Value::object({});
  }
  if (value.is_object()) {
    for (const auto& [field, next] : value.as_object()) {
      merged[field] = next;
    }
  }
  SharedStateRecord record{key, std::move(merged), now_iso8601()};
  records[key] = record;
  write_records(records);
  return record;
}

std::vector<SharedStateRecord> FileSharedStateStore::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto records_map = read_records();
  std::vector<SharedStateRecord> records;
  records.reserve(records_map.size());
  for (const auto& [_, record] : records_map) {
    records.push_back(record);
  }
  return records;
}

void FileSharedStateStore::clear(std::string key) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (key.empty()) {
    write_records({});
    return;
  }
  auto records = read_records();
  records.erase(key);
  write_records(records);
}

std::filesystem::path FileSharedStateStore::file_path() const {
  return base_dir_ / (encode_uri_component(namespace_id_) + ".json");
}

std::filesystem::path FileSharedStateStore::legacy_file_path() const {
  return base_dir_ / namespace_id_ / "shared-state.json";
}

std::map<std::string, SharedStateRecord> FileSharedStateStore::read_records() const {
  auto path = file_path();
  if (!std::filesystem::exists(path)) {
    const auto legacy = legacy_file_path();
    if (std::filesystem::exists(legacy)) {
      path = legacy;
    }
  }
  if (!std::filesystem::exists(path)) {
    return {};
  }
  const auto raw = read_json_file(path);
  std::map<std::string, SharedStateRecord> records;
  const auto& items = raw.is_array() ? raw.as_array() : raw.at("records").as_array();
  for (const auto& item : items) {
    auto record = shared_state_record_from_value(item);
    records[record.key] = std::move(record);
  }
  return records;
}

void FileSharedStateStore::write_records(const std::map<std::string, SharedStateRecord>& records) const {
  Value::Array items;
  for (const auto& [_, record] : records) {
    items.push_back(shared_state_record_to_value(record));
  }
  write_json_file(file_path(), Value(items));
}

MessageEnvelope InMemoryMailbox::publish(std::string channel, std::string sender, Value payload,
                                         std::string recipient, std::string type) {
  std::lock_guard<std::mutex> lock(mutex_);
  MessageEnvelope envelope{generate_uuid(), std::move(channel), std::move(sender), std::move(recipient),
                           std::move(type), std::move(payload), now_iso8601(), ""};
  messages_.push_back(envelope);
  return envelope;
}

std::vector<MessageEnvelope> InMemoryMailbox::receive(MailboxReadOptions options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<MessageEnvelope> messages;
  for (const auto& message : messages_) {
    if (!options.channel.empty() && message.channel != options.channel) {
      continue;
    }
    if (!options.recipient.empty() && message.recipient != options.recipient) {
      continue;
    }
    if (!message.read_at.empty()) {
      continue;
    }
    messages.push_back(message);
    if (messages.size() >= options.limit) {
      break;
    }
  }
  return messages;
}

void InMemoryMailbox::ack(const std::vector<std::string>& ids) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::set<std::string> id_set(ids.begin(), ids.end());
  const auto at = now_iso8601();
  for (auto& message : messages_) {
    if (id_set.contains(message.id)) {
      message.read_at = at;
    }
  }
}

void InMemoryMailbox::clear(std::string channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (channel.empty()) {
    messages_.clear();
    return;
  }
  messages_.erase(std::remove_if(messages_.begin(), messages_.end(),
                                 [&](const auto& message) { return message.channel == channel; }),
                  messages_.end());
}

SharedTaskState create_shared_task_state(Value input, Value metadata) {
  return SharedTaskState{generate_uuid(), std::move(input), {}, Value::object({}), std::move(metadata)};
}

RuleBasedTaskRouter::RuleBasedTaskRouter(std::vector<RoutingRule> rules, std::string fallback_worker_id)
    : rules_(std::move(rules)), fallback_worker_id_(std::move(fallback_worker_id)) {}

std::string RuleBasedTaskRouter::route(const std::string& input) const {
  std::string normalized = input;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });

  double best_score = -std::numeric_limits<double>::infinity();
  std::string best_worker;
  for (const auto& rule : rules_) {
    bool excluded = false;
    for (auto keyword : rule.exclude) {
      std::transform(keyword.begin(), keyword.end(), keyword.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      excluded = excluded || normalized.find(keyword) != std::string::npos;
    }
    if (excluded) {
      continue;
    }
    double score = rule.weight;
    for (auto keyword : rule.include) {
      std::transform(keyword.begin(), keyword.end(), keyword.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      if (normalized.find(keyword) != std::string::npos) {
        score += 1;
      }
    }
    if (score > best_score) {
      best_score = score;
      best_worker = rule.worker_id;
    }
  }
  return best_worker.empty() ? fallback_worker_id_ : best_worker;
}

std::string RuleBasedTaskRouter::route(const std::string& input,
                                       const CoordinatorContext&) const {
  return route(input);
}

WeightedTaskRouter::WeightedTaskRouter(std::map<std::string, double> weights, std::string fallback_worker_id)
    : weights_(std::move(weights)), fallback_worker_id_(std::move(fallback_worker_id)) {}

std::string WeightedTaskRouter::route(const std::string& input) const {
  std::string normalized = input;
  std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  double best_score = -std::numeric_limits<double>::infinity();
  std::string best_worker;
  for (const auto& [worker_id, weight] : weights_) {
    std::string worker = worker_id;
    std::transform(worker.begin(), worker.end(), worker.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    const double score = weight + (normalized.find(worker) != std::string::npos ? 1 : 0);
    if (score > best_score) {
      best_score = score;
      best_worker = worker_id;
    }
  }
  return best_worker.empty() ? fallback_worker_id_ : best_worker;
}

std::string WeightedTaskRouter::route(const std::string& input,
                                      const CoordinatorContext&) const {
  return route(input);
}

MailboxTaskRouter::MailboxTaskRouter(MailboxTaskRouterOptions options)
    : fallback_worker_id_(std::move(options.fallback_worker_id)),
      channel_(options.channel.empty() ? DEFAULT_SUPERVISOR_INBOX_CHANNEL : std::move(options.channel)) {}

std::string MailboxTaskRouter::route(const std::string&, const CoordinatorContext& context) const {
  if (!context.mailbox) {
    return fallback_worker_id_;
  }
  auto messages = context.mailbox->receive(MailboxReadOptions{
      .channel = channel_,
      .limit = 1,
  });
  if (messages.empty()) {
    return fallback_worker_id_;
  }
  const auto& message = messages.front();
  std::string worker_id = message.recipient;
  if (worker_id.empty() && message.payload.is_object()) {
    worker_id = message.payload.at("workerId").as_string();
  }
  if (worker_id.empty()) {
    worker_id = fallback_worker_id_;
  }
  if (!worker_id.empty()) {
    context.mailbox->ack({message.id});
  }
  return worker_id;
}

std::string ReplanStrategy::build_next_input(const AgentRunnerRunResult& result,
                                             const CoordinatorContext&) const {
  return result.text;
}

RuleBasedReplanStrategy::RuleBasedReplanStrategy(RuleBasedReplanStrategyConfig config)
    : required_keywords_(std::move(config.required_keywords)),
      rejected_keywords_(std::move(config.rejected_keywords)),
      next_prompt_(std::move(config.next_prompt)) {
  for (auto& keyword : required_keywords_) {
    keyword = lower_copy(std::move(keyword));
  }
  for (auto& keyword : rejected_keywords_) {
    keyword = lower_copy(std::move(keyword));
  }
}

bool RuleBasedReplanStrategy::should_replan(const AgentRunnerRunResult& result,
                                            const CoordinatorContext&) const {
  const auto text = lower_copy(result.text);
  for (const auto& keyword : rejected_keywords_) {
    if (!keyword.empty() && text.find(keyword) != std::string::npos) {
      return true;
    }
  }
  if (required_keywords_.empty()) {
    return false;
  }
  return !std::all_of(required_keywords_.begin(), required_keywords_.end(), [&](const std::string& keyword) {
    return keyword.empty() || text.find(keyword) != std::string::npos;
  });
}

std::string RuleBasedReplanStrategy::build_next_input(const AgentRunnerRunResult& result,
                                                      const CoordinatorContext&) const {
  if (next_prompt_.empty()) {
    return result.text;
  }
  return result.text + "\n\n" + next_prompt_;
}

ArtifactAwareReplanStrategy::ArtifactAwareReplanStrategy(ArtifactAwareReplanStrategyConfig config)
    : artifact_key_(std::move(config.artifact_key)),
      required_value_(std::move(config.required_value)),
      next_prompt_(std::move(config.next_prompt)) {
  if (artifact_key_.empty()) {
    throw ConfigurationError("ArtifactAwareReplanStrategy requires an artifact key.");
  }
}

bool ArtifactAwareReplanStrategy::should_replan(const AgentRunnerRunResult&,
                                                const CoordinatorContext& context) const {
  if (!context.artifact_store) {
    throw ConfigurationError("ArtifactAwareReplanStrategy requires an artifact store.");
  }
  const auto artifact = context.artifact_store->get(artifact_key_);
  if (!artifact) {
    return true;
  }
  return required_value_ ? artifact->value != *required_value_ : artifact->value.is_null();
}

std::string ArtifactAwareReplanStrategy::build_next_input(const AgentRunnerRunResult& result,
                                                          const CoordinatorContext&) const {
  if (next_prompt_.empty()) {
    return result.text;
  }
  return result.text + "\n\n" + next_prompt_;
}

ThresholdEvaluationStrategy::ThresholdEvaluationStrategy(ThresholdEvaluationStrategyConfig config)
    : min_length_(std::max(0, config.min_length)),
      required_keywords_(std::move(config.required_keywords)) {
  for (auto& keyword : required_keywords_) {
    keyword = lower_copy(std::move(keyword));
  }
}

EvaluationResult ThresholdEvaluationStrategy::evaluate(const AgentRunnerRunResult& result,
                                                       const CoordinatorContext&) const {
  const auto text = lower_copy(result.text);
  const auto trimmed = trim_copy(result.text);
  const bool length_accepted = static_cast<int>(trimmed.size()) >= min_length_;
  const bool keywords_accepted = std::all_of(required_keywords_.begin(), required_keywords_.end(),
                                             [&](const std::string& keyword) {
                                               return keyword.empty() || text.find(keyword) != std::string::npos;
                                             });
  const bool accepted = length_accepted && keywords_accepted;
  return EvaluationResult{
      .accepted = accepted,
      .score = static_cast<double>(trimmed.size()),
      .feedback = accepted ? std::string{} : "Output did not meet evaluation threshold.",
  };
}

KeywordEvaluationStrategy::KeywordEvaluationStrategy(KeywordEvaluationStrategyConfig config)
    : required_keywords_(std::move(config.required_keywords)),
      feedback_(std::move(config.feedback)) {
  for (auto& keyword : required_keywords_) {
    keyword = lower_copy(std::move(keyword));
  }
}

EvaluationResult KeywordEvaluationStrategy::evaluate(const AgentRunnerRunResult& result,
                                                     const CoordinatorContext&) const {
  const auto text = lower_copy(result.text);
  const bool accepted = std::all_of(required_keywords_.begin(), required_keywords_.end(),
                                    [&](const std::string& keyword) {
                                      return keyword.empty() || text.find(keyword) != std::string::npos;
                                    });
  return EvaluationResult{
      .accepted = accepted,
      .score = accepted ? static_cast<double>(result.text.size()) : 0.0,
      .feedback = accepted ? std::string{} : feedback_,
  };
}

namespace {

EvaluationResult default_evaluation(const AgentRunnerRunResult& result) {
  const auto trimmed = trim_copy(result.text);
  return EvaluationResult{
      .accepted = !trimmed.empty(),
      .score = static_cast<double>(trimmed.size()),
  };
}

}  // namespace

EvaluatorLoopCoordinator::EvaluatorLoopCoordinator(EvaluatorLoopCoordinatorOptions options)
    : max_attempts_(options.max_attempts),
      evaluation_strategy_(std::move(options.evaluation_strategy)) {}

EvaluatorCoordinatorResult EvaluatorLoopCoordinator::run(std::string input, CoordinatorContext& context) const {
  if (!context.primary_agent) {
    throw ConfigurationError("EvaluatorLoopCoordinator requires a primary agent.");
  }
  if (!context.artifact_store) {
    throw ConfigurationError("EvaluatorLoopCoordinator requires an artifact store.");
  }

  std::string current_input = input;
  AgentRunnerRunResult last_result;
  std::optional<EvaluationResult> last_evaluation;
  bool produced_result = false;
  int attempts_run = 0;
  for (int attempt = 0; attempt < max_attempts_; ++attempt) {
    attempts_run = attempt + 1;
    const std::string key = "evaluation:" + std::to_string(attempts_run);
    try {
      last_result = context.primary_agent->execution().run(current_input,
                                              "default",
                                              {},
                                              {},
                                              {},
                                              {},
                                              Value::object({{"evaluationAttempt", attempts_run},
                                                            {"taskId", context.state.task_id}}));
    } catch (const std::exception& error) {
      context.artifact_store->append_log(key, "failed", coordinator_error_value(error));
      throw;
    }
    produced_result = true;
    const auto evaluation = evaluation_strategy_ ? evaluation_strategy_->evaluate(last_result, context)
                                                 : default_evaluation(last_result);
    last_evaluation = evaluation;
    context.artifact_store->set(key, evaluation_result_value(evaluation));
    context.artifact_store->append_log(key, evaluation.accepted ? "accepted" : "rejected",
                                       evaluation_result_value(evaluation));
    if (evaluation.accepted) {
      context.state.output = last_result.text;
      return EvaluatorCoordinatorResult{last_result, true, attempts_run, evaluation};
    }
    current_input = evaluation.feedback.empty()
                        ? last_result.text
                        : input + "\n\nRevision guidance:\n" + evaluation.feedback;
  }

  if (!produced_result) {
    throw AgentFrameworkError("EvaluatorLoopCoordinator did not produce a result.");
  }
  context.state.output = last_result.text;
  return EvaluatorCoordinatorResult{last_result, false, attempts_run, last_evaluation};
}

SupervisorWorkerCoordinator::SupervisorWorkerCoordinator(SupervisorWorkerCoordinatorOptions options)
    : router_(std::move(options.router)),
      assignment_channel_(options.assignment_channel.empty()
                              ? DEFAULT_WORKER_ASSIGNMENT_CHANNEL
                              : std::move(options.assignment_channel)) {
  if (!router_) {
    throw ConfigurationError("SupervisorWorkerCoordinator requires a router.");
  }
}

AgentRunnerRunResult SupervisorWorkerCoordinator::run(std::string input, CoordinatorContext& context) const {
  if (!context.artifact_store) {
    throw ConfigurationError("SupervisorWorkerCoordinator requires an artifact store.");
  }
  const auto worker_id = router_->route(input, context);
  if (worker_id.empty()) {
    throw AgentFrameworkError("TaskRouterStrategy did not select a worker.");
  }
  AgentRunner* worker = require_worker_agent(context, worker_id);
  if (context.mailbox) {
    context.mailbox->publish(assignment_channel_,
                             "supervisor",
                             Value::object({{"input", input}}),
                             worker_id,
                             "assignment");
  }

  const std::string key = "worker:" + worker_id;
  AgentRunnerRunResult result;
  try {
    result = worker->execution().run(input,
                         "default",
                         {},
                         {},
                         {},
                         {},
                         Value::object({{"coordinator", "supervisor-worker"},
                                       {"workerId", worker_id},
                                       {"taskId", context.state.task_id}}));
  } catch (const std::exception& error) {
    context.artifact_store->append_log(key, "failed", coordinator_error_value(error));
    throw;
  }
  context.artifact_store->set(key, coordinator_result_value(result));
  context.artifact_store->append_log(key, "completed", result.text);
  if (context.shared_state) {
    context.shared_state->merge(key, Value::object({{"output", result.text}}));
  }
  context.state.output = result.text;
  return result;
}

SupervisorEvaluatorCoordinator::SupervisorEvaluatorCoordinator(SupervisorEvaluatorCoordinatorOptions options)
    : router_(std::move(options.router)),
      evaluation_strategy_(std::move(options.evaluation_strategy)),
      max_attempts_(options.max_attempts),
      lock_worker_(options.lock_worker) {
  if (!router_) {
    throw ConfigurationError("SupervisorEvaluatorCoordinator requires a router.");
  }
  if (!evaluation_strategy_) {
    throw ConfigurationError("SupervisorEvaluatorCoordinator requires an evaluation strategy.");
  }
}

EvaluatorCoordinatorResult SupervisorEvaluatorCoordinator::run(std::string input, CoordinatorContext& context) const {
  if (!context.artifact_store) {
    throw ConfigurationError("SupervisorEvaluatorCoordinator requires an artifact store.");
  }
  std::string current_input = input;
  std::string locked_worker_id;
  AgentRunnerRunResult last_result;
  std::optional<EvaluationResult> last_evaluation;
  bool produced_result = false;
  int attempts_run = 0;

  for (int attempt = 0; attempt < max_attempts_; ++attempt) {
    attempts_run = attempt + 1;
    std::string worker_id = lock_worker_ && !locked_worker_id.empty()
                                ? locked_worker_id
                                : router_->route(current_input, context);
    if (worker_id.empty()) {
      throw AgentFrameworkError("TaskRouterStrategy did not select a worker.");
    }
    if (lock_worker_ && locked_worker_id.empty()) {
      locked_worker_id = worker_id;
    }
    AgentRunner* worker = require_worker_agent(context, worker_id);
    const std::string worker_key = "supervisor-evaluator:worker:" + std::to_string(attempts_run);
    try {
      last_result = worker->execution().run(current_input,
                                "default",
                                {},
                                {},
                                {},
                                {},
                                Value::object({{"coordinator", "supervisor-evaluator"},
                                              {"attempt", attempts_run},
                                              {"workerId", worker_id},
                                              {"taskId", context.state.task_id}}));
    } catch (const std::exception& error) {
      context.artifact_store->append_log(worker_key, "failed", coordinator_error_value(error));
      throw;
    }
    produced_result = true;
    context.artifact_store->set(worker_key,
                                Value::object({{"workerId", worker_id},
                                              {"result", coordinator_result_value(last_result)}}));

    const auto evaluation = evaluation_strategy_->evaluate(last_result, context);
    last_evaluation = evaluation;
    context.artifact_store->set("supervisor-evaluator:evaluation:" + std::to_string(attempts_run),
                                evaluation_result_value(evaluation));
    if (evaluation.accepted) {
      context.state.output = last_result.text;
      return EvaluatorCoordinatorResult{last_result, true, attempts_run, evaluation};
    }
    current_input = evaluation.feedback.empty()
                        ? last_result.text
                        : input + "\n\nRevision guidance:\n" + evaluation.feedback;
  }

  if (!produced_result) {
    throw AgentFrameworkError("SupervisorEvaluatorCoordinator did not produce a result.");
  }
  context.state.output = last_result.text;
  return EvaluatorCoordinatorResult{last_result, false, attempts_run, last_evaluation};
}

WorkflowExecutionResult DefaultCoordinatorWorkflowBridge::run(const WorkflowBridgeInput& input,
                                                              CoordinatorContext& context) const {
  if (!context.workflow_engine) {
    throw ConfigurationError("DefaultCoordinatorWorkflowBridge requires a workflow engine.");
  }
  Value workflow_context = input.context.is_object() ? input.context : Value::object({});
  workflow_context["orchestrationTaskId"] = context.state.task_id;
  workflow_context["orchestrationMetadata"] = context.state.metadata;
  workflow_context["artifacts"] = context.state.artifacts.is_object()
                                      ? context.state.artifacts
                                      : Value::object({});
  if (workflow_context.at("artifacts").as_object().empty()) {
    workflow_context["artifacts"] = artifact_records_object(context.artifact_store);
  }
  return context.workflow_engine->run(input.definition, input.input, std::move(workflow_context));
}

WorkflowExecutionResult DefaultCoordinatorWorkflowBridge::resume(const std::string& workflow_run_id,
                                                                 CoordinatorContext& context) const {
  if (!context.workflow_engine) {
    throw ConfigurationError("DefaultCoordinatorWorkflowBridge requires a workflow engine.");
  }
  return context.workflow_engine->resume(workflow_run_id);
}

WorkflowBackedCoordinator::WorkflowBackedCoordinator(WorkflowBackedCoordinatorOptions options)
    : definition_(std::move(options.definition)),
      bridge_(std::move(options.bridge)),
      input_mapper_(std::move(options.input_mapper)) {
  if (definition_.id.empty()) {
    throw ConfigurationError("WorkflowBackedCoordinator requires a workflow definition.");
  }
  if (!bridge_) {
    bridge_ = std::make_shared<DefaultCoordinatorWorkflowBridge>();
  }
}

WorkflowBackedCoordinatorResult WorkflowBackedCoordinator::run(std::string input,
                                                               CoordinatorContext& context) const {
  if (!context.artifact_store) {
    throw ConfigurationError("WorkflowBackedCoordinator requires an artifact store.");
  }
  WorkflowBridgeInput bridge_input;
  bridge_input.definition = definition_;
  bridge_input.input = input_mapper_ ? input_mapper_(input, context)
                                     : Value::object({{"prompt", input}});
  WorkflowExecutionResult workflow;
  try {
    workflow = bridge_->run(bridge_input, context);
  } catch (const std::exception& error) {
    context.artifact_store->append_log("workflow:last", "failed", coordinator_error_value(error));
    throw;
  }
  const std::string text = workflow_output_text(workflow);
  context.state.output = text;
  context.artifact_store->set("workflow:last", workflow_execution_result_to_value(workflow));
  return WorkflowBackedCoordinatorResult{std::move(workflow), text};
}

SupervisorWorkflowCoordinator::SupervisorWorkflowCoordinator(SupervisorWorkflowCoordinatorOptions options)
    : router_(std::move(options.router)),
      bridge_(std::move(options.bridge)),
      definition_(std::move(options.definition)) {
  if (!router_) {
    throw ConfigurationError("SupervisorWorkflowCoordinator requires a router.");
  }
  if (!bridge_) {
    bridge_ = std::make_shared<DefaultCoordinatorWorkflowBridge>();
  }
  if (definition_.id.empty()) {
    throw ConfigurationError("SupervisorWorkflowCoordinator requires a workflow definition.");
  }
}

WorkflowBackedCoordinatorResult SupervisorWorkflowCoordinator::run(std::string input,
                                                                   CoordinatorContext& context) const {
  if (!context.artifact_store) {
    throw ConfigurationError("SupervisorWorkflowCoordinator requires an artifact store.");
  }
  const auto worker_id = router_->route(input, context);
  if (worker_id.empty()) {
    throw AgentFrameworkError("SupervisorWorkflowCoordinator could not route a worker.");
  }
  AgentRunner* worker = require_worker_agent(context, worker_id);

  AgentRunnerRunResult worker_result;
  try {
    worker_result = worker->execution().run(input,
                                "default",
                                {},
                                {},
                                {},
                                {},
                                Value::object({{"coordinator", "supervisor-workflow"},
                                              {"workerId", worker_id},
                                              {"taskId", context.state.task_id}}));
  } catch (const std::exception& error) {
    context.artifact_store->append_log("worker:" + worker_id + ":latest",
                                       "failed",
                                       coordinator_error_value(error));
    throw;
  }
  context.artifact_store->set("worker:" + worker_id + ":latest", coordinator_result_value(worker_result));
  if (context.shared_state) {
    context.shared_state->merge("supervisor-workflow",
                                Value::object({{"workerId", worker_id},
                                              {"workerOutput", worker_result.text}}));
  }

  WorkflowBridgeInput bridge_input;
  bridge_input.definition = definition_;
  bridge_input.input = Value::object({{"workerId", worker_id},
                                      {"workerOutput", worker_result.text},
                                      {"originalInput", input}});
  WorkflowExecutionResult workflow;
  try {
    workflow = bridge_->run(bridge_input, context);
  } catch (const std::exception& error) {
    context.artifact_store->append_log("workflow:last", "failed", coordinator_error_value(error));
    throw;
  }
  const std::string text = workflow_output_text(workflow);
  context.state.output = text;
  context.artifact_store->set("workflow:last", workflow_execution_result_to_value(workflow));
  return WorkflowBackedCoordinatorResult{std::move(workflow), text};
}

PlanActObserveCoordinator::PlanActObserveCoordinator(int max_rounds) : max_rounds_(max_rounds) {}

PlanActObserveCoordinator::PlanActObserveCoordinator(PlanActObserveCoordinatorOptions options)
    : max_rounds_(options.max_rounds), replan_strategy_(std::move(options.replan_strategy)) {}

AgentRunnerRunResult PlanActObserveCoordinator::run(std::string input, CoordinatorContext& context) const {
  if (!context.primary_agent) {
    throw ConfigurationError("PlanActObserveCoordinator requires a primary agent.");
  }
  if (!context.artifact_store) {
    throw ConfigurationError("PlanActObserveCoordinator requires an artifact store.");
  }
  AgentRunnerRunResult last_result;
  bool produced_result = false;
  std::string current_input = std::move(input);
  for (int round = 0; round < max_rounds_; ++round) {
    const std::string key = "round:" + std::to_string(round + 1);
    try {
      last_result = context.primary_agent->execution().run(current_input,
                                              "default",
                                              {},
                                              {},
                                              {},
                                              {},
                                              Value::object({{"orchestrationRound", round + 1},
                                                            {"taskId", context.state.task_id}}));
    } catch (const std::exception& error) {
      context.artifact_store->append_log(key, "failed", coordinator_error_value(error));
      throw;
    }
    produced_result = true;
    context.state.output = last_result.text;
    context.artifact_store->set(key, coordinator_result_value(last_result));
    context.artifact_store->append_log(key, "completed", last_result.text);
    if (context.shared_state) {
      context.shared_state->merge("plan-act-observe",
                                  Value::object({{"lastRound", round + 1}, {"lastOutput", last_result.text}}));
    }
    if (!replan_strategy_ || !replan_strategy_->should_replan(last_result, context)) {
      break;
    }
    current_input = replan_strategy_->build_next_input(last_result, context);
  }
  if (!produced_result) {
    throw AgentFrameworkError("PlanActObserveCoordinator did not produce a result.");
  }
  return last_result;
}
}  // namespace agent
