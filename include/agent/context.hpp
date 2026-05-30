#pragma once

#include "agent/memory.hpp"

#include <mutex>

namespace agent {

class CancellationToken;
class ToolRegistry;

struct EmbeddedContextBlock {
  std::string title;
  std::string content;
  int priority = 0;
};

using ContextResolver = std::function<std::vector<EmbeddedContextBlock>(const Value& runtime)>;

struct ContextSource {
  std::string id;
  std::string title;
  ContextResolver resolve;
  int priority = 0;
};

class EmbeddedContextManager {
 public:
  explicit EmbeddedContextManager(std::vector<ContextSource> sources = {});
  EmbeddedContextManager(const EmbeddedContextManager& other);
  EmbeddedContextManager& operator=(const EmbeddedContextManager& other);
  EmbeddedContextManager(EmbeddedContextManager&& other) noexcept;
  EmbeddedContextManager& operator=(EmbeddedContextManager&& other) noexcept;
  ContextSource& register_source(ContextSource source);
  std::vector<EmbeddedContextBlock> resolve_blocks(const Value& runtime = Value::object({})) const;
  std::optional<AgentMessage> build_message(const Value& runtime = Value::object({})) const;

 private:
  mutable std::mutex mutex_;
  std::vector<ContextSource> sources_;
};

struct PlanStep {
  std::string id;
  std::string title;
  std::string description;
  std::string tool_name;
  std::vector<std::string> depends_on;
};

struct ExecutionPlan {
  std::string goal;
  std::vector<PlanStep> steps;
  std::vector<std::string> notes;
  std::string updated_at;
};

struct PlannerParams {
  std::string input;
  SessionMemory* session = nullptr;
  Value context = Value::object({});
  ToolRegistry* tools = nullptr;
  std::vector<RetrievedMemory> memory_hits;
  std::vector<KnowledgeSearchHit> knowledge_hits;
  CancellationToken* cancellation = nullptr;
};

class Planner {
 public:
  virtual ~Planner() = default;
  virtual std::optional<ExecutionPlan> plan(const PlannerParams& params) = 0;
};

using PlannerHandler = std::function<std::optional<ExecutionPlan>(const PlannerParams&)>;

class StaticPlanner final : public Planner {
 public:
  explicit StaticPlanner(ExecutionPlan plan);
  explicit StaticPlanner(PlannerHandler handler);
  std::optional<ExecutionPlan> plan(const PlannerParams& params) override;

 private:
  std::optional<ExecutionPlan> plan_;
  PlannerHandler handler_;
};

struct ModelPlannerConfig {
  std::shared_ptr<ChatModelAdapter> model;
  std::size_t max_steps = 6;
  std::string system_prompt =
      "You are a planning assistant for an AI agent. Create concise execution plans and return only JSON.";
  ModelSettings model_settings;
};

class ModelPlanner final : public Planner {
 public:
  explicit ModelPlanner(ModelPlannerConfig config);
  std::optional<ExecutionPlan> plan(const PlannerParams& params) override;

 private:
  ModelPlannerConfig config_;
};

std::optional<ExecutionPlan> normalize_execution_plan(const Value& value);
Value execution_plan_to_value(const ExecutionPlan& plan);
std::optional<ExecutionPlan> parse_execution_plan_text(const std::string& text);
std::string render_execution_plan(const ExecutionPlan& plan);
AgentMessage create_plan_message(const ExecutionPlan& plan);

}  // namespace agent
