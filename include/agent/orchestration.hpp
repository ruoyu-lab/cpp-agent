#pragma once

#include "agent/runtime.hpp"
#include "agent/workflow.hpp"

#include <mutex>

namespace agent {

struct ArtifactRecord {
  std::string key;
  Value value;
  std::string updated_at;
};

struct ArtifactLogRecord {
  std::string id;
  std::string key;
  std::string event;
  Value value;
  std::string at;
};

struct ArtifactSnapshot {
  std::vector<ArtifactRecord> records;
  std::vector<ArtifactLogRecord> history;
};

class InMemoryArtifactStore {
 public:
  explicit InMemoryArtifactStore(std::vector<ArtifactRecord> initial = {});
  [[nodiscard]] std::optional<ArtifactRecord> get(const std::string& key) const;
  ArtifactRecord set(std::string key, Value value);
  [[nodiscard]] std::vector<ArtifactRecord> list() const;
  void clear(std::string key = {});
  ArtifactLogRecord append_log(std::string key, std::string event, Value value = {});
  [[nodiscard]] std::vector<ArtifactLogRecord> history(std::string key = {}) const;
  [[nodiscard]] ArtifactSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ArtifactRecord> records_;
  std::vector<ArtifactLogRecord> history_;
};

struct FileArtifactStoreConfig {
  std::filesystem::path base_dir;
  std::string namespace_id = "default";
};

class FileArtifactStore {
 public:
  FileArtifactStore(std::filesystem::path base_dir, std::string namespace_id = "default");
  explicit FileArtifactStore(FileArtifactStoreConfig config);
  [[nodiscard]] std::optional<ArtifactRecord> get(const std::string& key) const;
  ArtifactRecord set(std::string key, Value value);
  [[nodiscard]] std::vector<ArtifactRecord> list() const;
  void clear(std::string key = {});
  ArtifactLogRecord append_log(std::string key, std::string event, Value value = {});
  [[nodiscard]] std::vector<ArtifactLogRecord> history(std::string key = {}) const;
  [[nodiscard]] ArtifactSnapshot snapshot() const;

 private:
  [[nodiscard]] std::filesystem::path file_path() const;
  [[nodiscard]] std::filesystem::path legacy_file_path() const;
  [[nodiscard]] ArtifactSnapshot read_snapshot() const;
  void write_snapshot(const ArtifactSnapshot& snapshot) const;

  std::filesystem::path base_dir_;
  std::string namespace_id_;
  mutable std::mutex mutex_;
};

struct SharedStateRecord {
  std::string key;
  Value value;
  std::string updated_at;
};

class InMemorySharedStateStore {
 public:
  [[nodiscard]] std::optional<SharedStateRecord> get(const std::string& key) const;
  SharedStateRecord set(std::string key, Value value);
  SharedStateRecord merge(std::string key, Value value);
  [[nodiscard]] std::vector<SharedStateRecord> list() const;
  void clear(std::string key = {});

 private:
  mutable std::mutex mutex_;
  std::map<std::string, SharedStateRecord> records_;
};

struct FileSharedStateStoreConfig {
  std::filesystem::path base_dir;
  std::string namespace_id = "default";
};

class FileSharedStateStore {
 public:
  FileSharedStateStore(std::filesystem::path base_dir, std::string namespace_id = "default");
  explicit FileSharedStateStore(FileSharedStateStoreConfig config);
  [[nodiscard]] std::optional<SharedStateRecord> get(const std::string& key) const;
  SharedStateRecord set(std::string key, Value value);
  SharedStateRecord merge(std::string key, Value value);
  [[nodiscard]] std::vector<SharedStateRecord> list() const;
  void clear(std::string key = {});

 private:
  [[nodiscard]] std::filesystem::path file_path() const;
  [[nodiscard]] std::filesystem::path legacy_file_path() const;
  [[nodiscard]] std::map<std::string, SharedStateRecord> read_records() const;
  void write_records(const std::map<std::string, SharedStateRecord>& records) const;

  std::filesystem::path base_dir_;
  std::string namespace_id_;
  mutable std::mutex mutex_;
};

struct MessageEnvelope {
  std::string id;
  std::string channel;
  std::string sender;
  std::string recipient;
  std::string type;
  Value payload;
  std::string created_at;
  std::string read_at;
};

struct MailboxReadOptions {
  std::string channel;
  std::string recipient;
  std::size_t limit = std::numeric_limits<std::size_t>::max();
};

class InMemoryMailbox {
 public:
  MessageEnvelope publish(std::string channel, std::string sender, Value payload,
                          std::string recipient = {}, std::string type = {});
  [[nodiscard]] std::vector<MessageEnvelope> receive(MailboxReadOptions options = {}) const;
  void ack(const std::vector<std::string>& ids);
  void clear(std::string channel = {});

 private:
  mutable std::mutex mutex_;
  std::vector<MessageEnvelope> messages_;
};

struct SharedTaskState {
  std::string task_id;
  Value input;
  Value output;
  Value artifacts = Value::object({});
  Value metadata = Value::object({});
};

SharedTaskState create_shared_task_state(Value input, Value metadata = Value::object({}));

inline constexpr const char* DEFAULT_SUPERVISOR_INBOX_CHANNEL = "supervisor.inbox";
inline constexpr const char* DEFAULT_WORKER_ASSIGNMENT_CHANNEL = "worker.assignments";

struct CoordinatorContext {
  AgentRunner* primary_agent = nullptr;
  std::map<std::string, AgentRunner*> worker_agents;
  WorkflowEngine* workflow_engine = nullptr;
  InMemoryArtifactStore* artifact_store = nullptr;
  InMemorySharedStateStore* shared_state = nullptr;
  InMemoryMailbox* mailbox = nullptr;
  SharedTaskState state;
};

class TaskRouterStrategy {
 public:
  virtual ~TaskRouterStrategy() = default;
  [[nodiscard]] virtual std::string route(const std::string& input,
                                          const CoordinatorContext& context) const = 0;
};

struct RoutingRule {
  std::string worker_id;
  std::vector<std::string> include;
  std::vector<std::string> exclude;
  double weight = 0;
};

class RuleBasedTaskRouter : public TaskRouterStrategy {
 public:
  RuleBasedTaskRouter(std::vector<RoutingRule> rules, std::string fallback_worker_id = {});
  [[nodiscard]] std::string route(const std::string& input) const;
  [[nodiscard]] std::string route(const std::string& input,
                                  const CoordinatorContext& context) const override;

 private:
  std::vector<RoutingRule> rules_;
  std::string fallback_worker_id_;
};

class WeightedTaskRouter : public TaskRouterStrategy {
 public:
  WeightedTaskRouter(std::map<std::string, double> weights, std::string fallback_worker_id = {});
  [[nodiscard]] std::string route(const std::string& input) const;
  [[nodiscard]] std::string route(const std::string& input,
                                  const CoordinatorContext& context) const override;

 private:
  std::map<std::string, double> weights_;
  std::string fallback_worker_id_;
};

struct MailboxTaskRouterOptions {
  std::string fallback_worker_id;
  std::string channel = DEFAULT_SUPERVISOR_INBOX_CHANNEL;
};

class MailboxTaskRouter : public TaskRouterStrategy {
 public:
  explicit MailboxTaskRouter(MailboxTaskRouterOptions options = {});
  [[nodiscard]] std::string route(const std::string& input,
                                  const CoordinatorContext& context) const override;

 private:
  std::string fallback_worker_id_;
  std::string channel_;
};

class ReplanStrategy {
 public:
  virtual ~ReplanStrategy() = default;
  [[nodiscard]] virtual bool should_replan(const AgentRunnerRunResult& result,
                                           const CoordinatorContext& context) const = 0;
  [[nodiscard]] virtual std::string build_next_input(const AgentRunnerRunResult& result,
                                                     const CoordinatorContext& context) const;
};

struct RuleBasedReplanStrategyConfig {
  std::vector<std::string> required_keywords;
  std::vector<std::string> rejected_keywords;
  std::string next_prompt;
};

class RuleBasedReplanStrategy : public ReplanStrategy {
 public:
  explicit RuleBasedReplanStrategy(RuleBasedReplanStrategyConfig config = {});
  [[nodiscard]] bool should_replan(const AgentRunnerRunResult& result,
                                   const CoordinatorContext& context) const override;
  [[nodiscard]] std::string build_next_input(const AgentRunnerRunResult& result,
                                             const CoordinatorContext& context) const override;

 private:
  std::vector<std::string> required_keywords_;
  std::vector<std::string> rejected_keywords_;
  std::string next_prompt_;
};

struct ArtifactAwareReplanStrategyConfig {
  std::string artifact_key;
  std::optional<Value> required_value;
  std::string next_prompt;
};

class ArtifactAwareReplanStrategy : public ReplanStrategy {
 public:
  explicit ArtifactAwareReplanStrategy(ArtifactAwareReplanStrategyConfig config);
  [[nodiscard]] bool should_replan(const AgentRunnerRunResult& result,
                                   const CoordinatorContext& context) const override;
  [[nodiscard]] std::string build_next_input(const AgentRunnerRunResult& result,
                                             const CoordinatorContext& context) const override;

 private:
  std::string artifact_key_;
  std::optional<Value> required_value_;
  std::string next_prompt_;
};

struct EvaluationResult {
  bool accepted = false;
  double score = 0;
  std::string feedback;
  Value metadata = Value::object({});
};

class EvaluationStrategy {
 public:
  virtual ~EvaluationStrategy() = default;
  [[nodiscard]] virtual EvaluationResult evaluate(const AgentRunnerRunResult& result,
                                                  const CoordinatorContext& context) const = 0;
};

struct ThresholdEvaluationStrategyConfig {
  int min_length = 1;
  std::vector<std::string> required_keywords;
};

class ThresholdEvaluationStrategy : public EvaluationStrategy {
 public:
  explicit ThresholdEvaluationStrategy(ThresholdEvaluationStrategyConfig config = {});
  [[nodiscard]] EvaluationResult evaluate(const AgentRunnerRunResult& result,
                                          const CoordinatorContext& context) const override;

 private:
  int min_length_ = 1;
  std::vector<std::string> required_keywords_;
};

struct KeywordEvaluationStrategyConfig {
  std::vector<std::string> required_keywords;
  std::string feedback;
};

class KeywordEvaluationStrategy : public EvaluationStrategy {
 public:
  explicit KeywordEvaluationStrategy(KeywordEvaluationStrategyConfig config);
  [[nodiscard]] EvaluationResult evaluate(const AgentRunnerRunResult& result,
                                          const CoordinatorContext& context) const override;

 private:
  std::vector<std::string> required_keywords_;
  std::string feedback_;
};

struct EvaluatorCoordinatorResult {
  AgentRunnerRunResult result;
  bool accepted = false;
  int attempts = 0;
  std::optional<EvaluationResult> evaluation;
};

struct EvaluatorLoopCoordinatorOptions {
  int max_attempts = 2;
  std::shared_ptr<EvaluationStrategy> evaluation_strategy;
};

class EvaluatorLoopCoordinator {
 public:
  explicit EvaluatorLoopCoordinator(EvaluatorLoopCoordinatorOptions options = {});
  EvaluatorCoordinatorResult run(std::string input, CoordinatorContext& context) const;

 private:
  int max_attempts_ = 2;
  std::shared_ptr<EvaluationStrategy> evaluation_strategy_;
};

struct SupervisorWorkerCoordinatorOptions {
  std::shared_ptr<TaskRouterStrategy> router;
  std::string assignment_channel = DEFAULT_WORKER_ASSIGNMENT_CHANNEL;
};

class SupervisorWorkerCoordinator {
 public:
  explicit SupervisorWorkerCoordinator(SupervisorWorkerCoordinatorOptions options);
  AgentRunnerRunResult run(std::string input, CoordinatorContext& context) const;

 private:
  std::shared_ptr<TaskRouterStrategy> router_;
  std::string assignment_channel_;
};

struct SupervisorEvaluatorCoordinatorOptions {
  std::shared_ptr<TaskRouterStrategy> router;
  std::shared_ptr<EvaluationStrategy> evaluation_strategy;
  int max_attempts = 2;
  bool lock_worker = true;
};

class SupervisorEvaluatorCoordinator {
 public:
  explicit SupervisorEvaluatorCoordinator(SupervisorEvaluatorCoordinatorOptions options);
  EvaluatorCoordinatorResult run(std::string input, CoordinatorContext& context) const;

 private:
  std::shared_ptr<TaskRouterStrategy> router_;
  std::shared_ptr<EvaluationStrategy> evaluation_strategy_;
  int max_attempts_ = 2;
  bool lock_worker_ = true;
};

struct WorkflowBridgeInput {
  WorkflowDefinition definition;
  Value input = Value::object({});
  Value context = Value::object({});
};

struct WorkflowBackedCoordinatorResult {
  WorkflowExecutionResult workflow;
  std::string text;
};

class CoordinatorWorkflowBridge {
 public:
  virtual ~CoordinatorWorkflowBridge() = default;
  virtual WorkflowExecutionResult run(const WorkflowBridgeInput& input,
                                      CoordinatorContext& context) const = 0;
  virtual WorkflowExecutionResult resume(const std::string& workflow_run_id,
                                         CoordinatorContext& context) const = 0;
};

class DefaultCoordinatorWorkflowBridge : public CoordinatorWorkflowBridge {
 public:
  WorkflowExecutionResult run(const WorkflowBridgeInput& input,
                              CoordinatorContext& context) const override;
  WorkflowExecutionResult resume(const std::string& workflow_run_id,
                                 CoordinatorContext& context) const override;
};

using WorkflowBackedInputMapper = std::function<Value(const std::string&, const CoordinatorContext&)>;

struct WorkflowBackedCoordinatorOptions {
  WorkflowDefinition definition;
  std::shared_ptr<CoordinatorWorkflowBridge> bridge;
  WorkflowBackedInputMapper input_mapper;
};

class WorkflowBackedCoordinator {
 public:
  explicit WorkflowBackedCoordinator(WorkflowBackedCoordinatorOptions options);
  WorkflowBackedCoordinatorResult run(std::string input, CoordinatorContext& context) const;

 private:
  WorkflowDefinition definition_;
  std::shared_ptr<CoordinatorWorkflowBridge> bridge_;
  WorkflowBackedInputMapper input_mapper_;
};

struct SupervisorWorkflowCoordinatorOptions {
  std::shared_ptr<TaskRouterStrategy> router;
  std::shared_ptr<CoordinatorWorkflowBridge> bridge;
  WorkflowDefinition definition;
};

class SupervisorWorkflowCoordinator {
 public:
  explicit SupervisorWorkflowCoordinator(SupervisorWorkflowCoordinatorOptions options);
  WorkflowBackedCoordinatorResult run(std::string input, CoordinatorContext& context) const;

 private:
  std::shared_ptr<TaskRouterStrategy> router_;
  std::shared_ptr<CoordinatorWorkflowBridge> bridge_;
  WorkflowDefinition definition_;
};

struct PlanActObserveCoordinatorOptions {
  int max_rounds = 3;
  std::shared_ptr<ReplanStrategy> replan_strategy;
};

class PlanActObserveCoordinator {
 public:
  explicit PlanActObserveCoordinator(int max_rounds = 3);
  explicit PlanActObserveCoordinator(PlanActObserveCoordinatorOptions options);
  AgentRunnerRunResult run(std::string input, CoordinatorContext& context) const;

 private:
  int max_rounds_;
  std::shared_ptr<ReplanStrategy> replan_strategy_;
};

}  // namespace agent
