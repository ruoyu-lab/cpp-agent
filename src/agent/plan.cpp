#include "agent/plan.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <sstream>

namespace agent {

namespace {

bool model_settings_empty(const ModelSettings& settings) {
  return settings.model.empty() && !settings.temperature && !settings.max_output_tokens && !settings.reasoning
         && settings.extra.as_object().empty();
}

Value merge_object_values(Value base, const Value& overlay) {
  if (!base.is_object()) {
    base = Value::object({});
  }
  if (!overlay.is_object()) {
    return base;
  }
  for (const auto& [key, value] : overlay.as_object()) {
    base[key] = value;
  }
  return base;
}

RunnerRetrievalOptions merge_retrieval_options(RunnerRetrievalOptions base,
                                               const RunnerRetrievalOptions& override_options) {
  if (override_options.enabled) base.enabled = override_options.enabled;
  if (override_options.top_k) base.top_k = override_options.top_k;
  if (override_options.min_score) base.min_score = override_options.min_score;
  if (!override_options.namespace_id.empty()) base.namespace_id = override_options.namespace_id;
  return base;
}

RunnerWritebackOptions merge_writeback_options(RunnerWritebackOptions base,
                                               const RunnerWritebackOptions& override_options) {
  if (override_options.enabled) base.enabled = override_options.enabled;
  if (!override_options.namespace_id.empty()) base.namespace_id = override_options.namespace_id;
  base.metadata = merge_object_values(std::move(base.metadata), override_options.metadata);
  return base;
}

Value string_vector_value(const std::vector<std::string>& values) {
  Value::Array output;
  output.reserve(values.size());
  for (const auto& value : values) {
    output.emplace_back(value);
  }
  return Value(std::move(output));
}

Value plan_step_to_value(const PlanStep& step) {
  return Value::object({
      {"id", step.id},
      {"title", step.title},
      {"description", step.description},
      {"toolName", step.tool_name},
      {"dependsOn", string_vector_value(step.depends_on)},
  });
}

Value plan_step_input_value(const ExecutionPlan& plan, const PlanStep& step) {
  Value input = Value::object({
      {"planGoal", plan.goal},
      {"planStep", plan_step_to_value(step)},
      {"plan", execution_plan_to_value(plan)},
  });
  if (!plan.notes.empty()) {
    input["planNotes"] = string_vector_value(plan.notes);
  }
  return input;
}

Value plan_step_metadata_value(const PlanStep& step) {
  Value metadata = Value::object({{"source", "plan-and-execute"}});
  if (!step.tool_name.empty()) {
    metadata["toolName"] = step.tool_name;
  }
  return metadata;
}

void validate_execution_plan_dependencies(const ExecutionPlan& plan) {
  if (plan.steps.empty()) {
    throw PlanAndExecuteError("PlanAndExecuteRunner planner produced an empty plan.");
  }

  std::map<std::string, std::size_t> step_index_by_id;
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    const auto& step = plan.steps[index];
    if (step.id.empty()) {
      throw PlanAndExecuteError("PlanAndExecuteRunner plan step at index " +
                                std::to_string(index) + " is missing an id.");
    }
    const auto [_, inserted] = step_index_by_id.emplace(step.id, index);
    if (!inserted) {
      throw PlanAndExecuteError("PlanAndExecuteRunner plan contains duplicate step id: " + step.id);
    }
  }

  for (const auto& step : plan.steps) {
    for (const auto& dependency : step.depends_on) {
      if (dependency.empty()) {
        throw PlanAndExecuteError("PlanAndExecuteRunner step \"" + step.id +
                                  "\" contains an empty dependency id.");
      }
      if (!step_index_by_id.contains(dependency)) {
        throw PlanAndExecuteError("PlanAndExecuteRunner step \"" + step.id +
                                  "\" depends on unknown step \"" + dependency + "\".");
      }
      if (dependency == step.id) {
        throw PlanAndExecuteError("PlanAndExecuteRunner step \"" + step.id +
                                  "\" cannot depend on itself.");
      }
    }
  }

  enum class VisitState {
    Unvisited,
    Visiting,
    Visited,
  };
  std::vector<VisitState> states(plan.steps.size(), VisitState::Unvisited);
  std::vector<std::string> stack;
  const auto visit = [&](auto&& self, std::size_t index) -> void {
    if (states[index] == VisitState::Visited) {
      return;
    }
    if (states[index] == VisitState::Visiting) {
      std::ostringstream cycle;
      bool first = true;
      for (const auto& id : stack) {
        if (!first) {
          cycle << " -> ";
        }
        first = false;
        cycle << id;
      }
      if (!first) {
        cycle << " -> ";
      }
      cycle << plan.steps[index].id;
      throw PlanAndExecuteError("PlanAndExecuteRunner plan contains a dependency cycle: " +
                                cycle.str());
    }
    states[index] = VisitState::Visiting;
    stack.push_back(plan.steps[index].id);
    for (const auto& dependency : plan.steps[index].depends_on) {
      self(self, step_index_by_id.at(dependency));
    }
    stack.pop_back();
    states[index] = VisitState::Visited;
  };

  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    visit(visit, index);
  }
}

PlanAndExecuteRunStatus plan_execute_status_from_autonomous(AutonomousRunStatus status) {
  switch (status) {
    case AutonomousRunStatus::Queued:
    case AutonomousRunStatus::Running:
      return PlanAndExecuteRunStatus::Running;
    case AutonomousRunStatus::Waiting:
      return PlanAndExecuteRunStatus::Waiting;
    case AutonomousRunStatus::Completed:
      return PlanAndExecuteRunStatus::Completed;
    case AutonomousRunStatus::Failed:
      return PlanAndExecuteRunStatus::Failed;
    case AutonomousRunStatus::Cancelled:
      return PlanAndExecuteRunStatus::Cancelled;
    case AutonomousRunStatus::Interrupted:
      return PlanAndExecuteRunStatus::Interrupted;
  }
  return PlanAndExecuteRunStatus::Failed;
}

std::string latest_step_text(const AutonomousRunSnapshot& snapshot) {
  const AutonomousStep* latest = nullptr;
  for (const auto& step : snapshot.steps) {
    if (step.status != AutonomousStepStatus::Completed) {
      continue;
    }
    if (!latest || step.index >= latest->index) {
      latest = &step;
    }
  }
  if (!latest) {
    return {};
  }
  return latest->output.at("text").as_string();
}

Value plan_execute_metadata(Value base, const Value& overlay, const std::string& session_id) {
  base = merge_object_values(std::move(base), overlay);
  base["runner"] = "plan-and-execute";
  base["sessionId"] = session_id;
  return base;
}

}  // namespace

std::string to_string(PlanAndExecuteRunStatus status) {
  switch (status) {
    case PlanAndExecuteRunStatus::Running:
      return "running";
    case PlanAndExecuteRunStatus::Waiting:
      return "waiting";
    case PlanAndExecuteRunStatus::Completed:
      return "completed";
    case PlanAndExecuteRunStatus::Failed:
      return "failed";
    case PlanAndExecuteRunStatus::Cancelled:
      return "cancelled";
    case PlanAndExecuteRunStatus::Interrupted:
      return "interrupted";
  }
  return "failed";
}

AutonomousPlan autonomous_plan_from_execution_plan(const ExecutionPlan& plan) {
  validate_execution_plan_dependencies(plan);

  AutonomousPlan autonomous;
  autonomous.summary = plan.goal;
  autonomous.steps.reserve(plan.steps.size());
  for (const auto& step : plan.steps) {
    autonomous.steps.push_back(AutonomousPlanStep{
        .id = step.id,
        .title = step.title,
        .objective = step.description.empty() ? step.title : step.description,
        .input = plan_step_input_value(plan, step),
        .depends_on = step.depends_on,
        .metadata = plan_step_metadata_value(step),
    });
  }
  return autonomous;
}

ExecutionPlan execution_plan_from_autonomous_plan(const AutonomousPlan& plan) {
  ExecutionPlan execution;
  execution.goal = plan.summary.empty() ? "Execute the task safely and efficiently." : plan.summary;
  execution.updated_at = now_iso8601();
  execution.steps.reserve(plan.steps.size());
  for (const auto& step : plan.steps) {
    execution.steps.push_back(PlanStep{
        .id = step.id,
        .title = step.title,
        .description = step.objective,
        .tool_name = step.metadata.at("toolName").as_string(),
        .depends_on = step.depends_on,
    });
  }
  validate_execution_plan_dependencies(execution);
  return execution;
}

Value plan_and_execute_run_result_to_value(const PlanAndExecuteRunResult& result) {
  return Value::object({
      {"runId", result.run_id},
      {"sessionId", result.session_id},
      {"input", result.input},
      {"status", to_string(result.status)},
      {"text", result.text},
      {"error", result.error.empty() ? Value() : Value(result.error)},
      {"plan", execution_plan_to_value(result.plan)},
      {"snapshot", autonomous_run_snapshot_to_value(result.snapshot)},
      {"output", result.output},
  });
}

PlanAndExecuteRunner::PlanAndExecuteRunner(PlanAndExecuteRunnerConfig config)
    : config_(std::move(config)),
      store_(config_.store ? config_.store : &owned_store_) {
  if (!config_.planner) {
    throw ConfigurationError("PlanAndExecuteRunner requires a planner.");
  }
  if (!config_.runner) {
    throw ConfigurationError("PlanAndExecuteRunner requires an AgentRunner.");
  }
  if (config_.max_steps_per_run == 0) {
    config_.max_steps_per_run = 64;
  }
}

PlanAndExecuteRunResult PlanAndExecuteRunner::run(const std::string& input,
                                                  PlanAndExecuteRunOptions options) {
  const std::string run_id = options.run_id.empty() ? generate_uuid() : options.run_id;
  const std::string session_id = !options.session_id.empty()
                                     ? options.session_id
                                     : (!config_.session_id.empty()
                                            ? config_.session_id
                                            : "plan-execute:" + run_id);
  const ModelSettings model_settings = model_settings_empty(options.model_settings)
                                           ? config_.model_settings
                                           : options.model_settings;
  const auto retrieval_options = merge_retrieval_options(config_.retrieval_options,
                                                         options.retrieval_options);
  const auto writeback_options = merge_writeback_options(config_.writeback_options,
                                                         options.writeback_options);
  std::vector<SkillActivation> skill_activations = config_.skill_activations;
  skill_activations.insert(skill_activations.end(),
                           std::make_move_iterator(options.skill_activations.begin()),
                           std::make_move_iterator(options.skill_activations.end()));
  const Value context = merge_object_values(config_.context, options.context);
  const Value metadata = plan_execute_metadata(config_.metadata, options.metadata, session_id);
  const auto knowledge_retrieval_options = options.knowledge_retrieval_options
                                               ? options.knowledge_retrieval_options
                                               : config_.knowledge_retrieval_options;
  auto* cancellation = options.cancellation;
  auto* event_bus = config_.event_bus ? config_.event_bus : config_.runner->events().bus();

  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  if (event_bus) {
    event_bus->publish("plan_execute.started", ExecutionTarget::Run,
                       Value::object({{"runId", run_id},
                                     {"sessionId", session_id},
                                     {"input", input}}));
  }

  auto session = config_.runner->sessions().get(session_id);
  std::optional<ExecutionPlan> maybe_plan;
  try {
    maybe_plan = config_.planner->plan(PlannerParams{
        .input = input,
        .session = session.get(),
        .context = context,
        .tools = config_.planning_tools,
        .cancellation = cancellation,
    });
  } catch (const std::exception& error) {
    if (event_bus) {
      event_bus->publish("plan_execute.failed", ExecutionTarget::Run,
                         Value::object({{"runId", run_id},
                                       {"stage", "planning"},
                                       {"error", error.what()}}));
    }
    throw;
  }

  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  if (!maybe_plan) {
    if (event_bus) {
      event_bus->publish("plan_execute.failed", ExecutionTarget::Run,
                         Value::object({{"runId", run_id},
                                       {"stage", "planning"},
                                       {"error", "Planner produced an empty plan."}}));
    }
    throw PlanAndExecuteError("PlanAndExecuteRunner planner produced an empty plan.");
  }

  ExecutionPlan plan = std::move(*maybe_plan);
  validate_execution_plan_dependencies(plan);
  if (event_bus) {
    event_bus->publish("plan_execute.planned", ExecutionTarget::Run,
                       Value::object({{"runId", run_id},
                                     {"goal", plan.goal},
                                     {"stepCount", plan.steps.size()},
                                     {"plan", execution_plan_to_value(plan)}}));
  }

  auto autonomous_plan = autonomous_plan_from_execution_plan(plan);
  StaticAutonomousPlanner autonomous_planner(std::move(autonomous_plan));
  AgentRunnerStepExecutor executor(AgentRunnerStepExecutorConfig{
      .runner = config_.runner,
      .session_id = session_id,
      .session_resolver = config_.step_session_resolver,
      .model_settings = model_settings,
      .context = context,
      .retrieval_options = retrieval_options,
      .writeback_options = writeback_options,
      .skill_activations = skill_activations,
      .knowledge_retrieval_options = knowledge_retrieval_options,
      .prompt_builder = config_.step_prompt_builder,
      .cancellation = cancellation,
      .enable_planning = config_.enable_step_planning,
  });
  AutonomousRunManager manager(AutonomousRunManagerConfig{
      .store = store_,
      .planner = &autonomous_planner,
      .executor = &executor,
      .max_steps_per_run = config_.max_steps_per_run,
  });

  manager.create_run(CreateAutonomousRunInput{
      .id = run_id,
      .goal = input,
      .input = Value(input),
      .metadata = metadata,
  });
  manager.plan(run_id);
  auto snapshot = manager.run(run_id);
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  PlanAndExecuteRunResult result{
      .run_id = run_id,
      .session_id = session_id,
      .input = input,
      .plan = std::move(plan),
      .snapshot = snapshot,
      .status = plan_execute_status_from_autonomous(snapshot.run.status),
      .text = latest_step_text(snapshot),
      .error = snapshot.run.error,
      .output = snapshot.run.output,
  };

  if (event_bus) {
    std::string category = "plan_execute.finished";
    switch (result.status) {
      case PlanAndExecuteRunStatus::Completed:
        category = "plan_execute.completed";
        break;
      case PlanAndExecuteRunStatus::Failed:
        category = "plan_execute.failed";
        break;
      case PlanAndExecuteRunStatus::Waiting:
        category = "plan_execute.waiting";
        break;
      case PlanAndExecuteRunStatus::Cancelled:
        category = "plan_execute.cancelled";
        break;
      case PlanAndExecuteRunStatus::Interrupted:
        category = "plan_execute.interrupted";
        break;
      case PlanAndExecuteRunStatus::Running:
        break;
    }
    event_bus->publish(category, ExecutionTarget::Run,
                       Value::object({{"runId", run_id},
                                     {"status", to_string(result.status)},
                                     {"stepCount", result.snapshot.steps.size()},
                                     {"error", result.error.empty() ? Value() : Value(result.error)}}));
  }
  return result;
}

}  // namespace agent
