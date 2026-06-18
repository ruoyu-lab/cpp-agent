#pragma once

#include "agent/autonomous.hpp"

namespace agent {

enum class PlanAndExecuteRunStatus {
  Running,
  Waiting,
  Completed,
  Failed,
  Cancelled,
  Interrupted,
};

std::string to_string(PlanAndExecuteRunStatus status);

class PlanAndExecuteError : public AgentFrameworkError {
 public:
  using AgentFrameworkError::AgentFrameworkError;
};

struct PlanAndExecuteRunOptions {
  std::string run_id;
  std::string session_id;
  ModelSettings model_settings;
  RunnerRetrievalOptions retrieval_options;
  RunnerWritebackOptions writeback_options;
  std::vector<SkillActivation> skill_activations;
  Value context = Value::object({});
  Value metadata = Value::object({});
  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt;
  CancellationToken* cancellation = nullptr;
};

struct PlanAndExecuteRunnerConfig {
  std::shared_ptr<Planner> planner;
  AgentRunner* runner = nullptr;
  AutonomousStore* store = nullptr;
  ToolRegistry* planning_tools = nullptr;
  EventBus* event_bus = nullptr;
  std::string session_id;
  ModelSettings model_settings;
  RunnerRetrievalOptions retrieval_options;
  RunnerWritebackOptions writeback_options;
  std::vector<SkillActivation> skill_activations;
  Value context = Value::object({});
  Value metadata = Value::object({});
  std::optional<RunnerKnowledgeRetrievalOptions> knowledge_retrieval_options = std::nullopt;
  AgentRunnerStepSessionResolver step_session_resolver;
  AgentRunnerStepPromptBuilder step_prompt_builder;
  std::size_t max_steps_per_run = 64;
  bool enable_step_planning = false;
};

struct PlanAndExecuteRunResult {
  std::string run_id;
  std::string session_id;
  std::string input;
  ExecutionPlan plan;
  AutonomousRunSnapshot snapshot;
  PlanAndExecuteRunStatus status = PlanAndExecuteRunStatus::Running;
  std::string text;
  std::string error;
  Value output;
};

AutonomousPlan autonomous_plan_from_execution_plan(const ExecutionPlan& plan);
ExecutionPlan execution_plan_from_autonomous_plan(const AutonomousPlan& plan);
Value plan_and_execute_run_result_to_value(const PlanAndExecuteRunResult& result);

class PlanAndExecuteRunner {
 public:
  explicit PlanAndExecuteRunner(PlanAndExecuteRunnerConfig config);

  PlanAndExecuteRunResult run(const std::string& input,
                              PlanAndExecuteRunOptions options = {});

 private:
  PlanAndExecuteRunnerConfig config_;
  InMemoryAutonomousStore owned_store_;
  AutonomousStore* store_;
};

}  // namespace agent
