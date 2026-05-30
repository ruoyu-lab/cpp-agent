#include "agent/agent.hpp"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace agent {

namespace {

Value string_array_to_value(const std::vector<std::string>& values) {
  Value::Array output;
  for (const auto& value : values) {
    output.emplace_back(value);
  }
  return Value(output);
}

std::vector<std::string> string_array_from_value(const Value& value) {
  std::vector<std::string> output;
  for (const auto& item : value.as_array()) {
    output.push_back(item.as_string());
  }
  return output;
}

bool metadata_matches(const Value& metadata, const Value& filter) {
  if (!filter.is_object()) {
    return true;
  }
  for (const auto& [key, value] : filter.as_object()) {
    if (!metadata.is_object() || !metadata.contains(key) || metadata.at(key) != value) {
      return false;
    }
  }
  return true;
}

bool dependencies_completed(const AutonomousStep& step, const std::vector<AutonomousStep>& steps) {
  if (step.depends_on.empty()) {
    return true;
  }
  for (const auto& dependency : step.depends_on) {
    const auto found = std::find_if(steps.begin(), steps.end(), [&](const auto& candidate) {
      return candidate.id == dependency;
    });
    if (found == steps.end() || found->status != AutonomousStepStatus::Completed) {
      return false;
    }
  }
  return true;
}

Value run_output_from_steps(const std::vector<AutonomousStep>& steps) {
  Value::Array output;
  for (const auto& step : steps) {
    output.push_back(Value::object({{"stepId", step.id},
                                    {"title", step.title},
                                    {"output", step.output}}));
  }
  return Value(output);
}

Value merge_metadata(Value base, const Value& overlay) {
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

Value plan_step_to_value(const AutonomousPlanStep& step) {
  return Value::object({{"id", step.id.empty() ? Value() : Value(step.id)},
                        {"title", step.title},
                        {"objective", step.objective},
                        {"input", step.input},
                        {"dependsOn", string_array_to_value(step.depends_on)},
                        {"metadata", step.metadata}});
}

Value plan_to_value(const AutonomousPlan& plan) {
  Value::Array steps;
  for (const auto& step : plan.steps) {
    steps.push_back(plan_step_to_value(step));
  }
  return Value::object({{"summary", plan.summary.empty() ? Value() : Value(plan.summary)},
                        {"steps", Value(steps)}});
}

std::vector<AutonomousPlanStep> plan_steps_from_value(const Value& value) {
  if (!value.is_array()) {
    throw AgentFrameworkError("Autonomous planner JSON must include a steps array.");
  }
  std::vector<AutonomousPlanStep> steps;
  for (const auto& item : value.as_array()) {
    if (!item.is_object()) {
      throw AgentFrameworkError("Autonomous planner step must be an object.");
    }
    AutonomousPlanStep step;
    step.id = item.at("id").as_string();
    step.title = item.at("title").as_string();
    if (step.title.empty()) {
      throw AgentFrameworkError("Autonomous planner step title is required.");
    }
    step.objective = item.at("objective").as_string(step.title);
    step.input = item.at("input");
    const auto& depends_on = item.contains("dependsOn") ? item.at("dependsOn") : item.at("depends_on");
    if (depends_on.is_array()) {
      step.depends_on = string_array_from_value(depends_on);
    }
    step.metadata = item.at("metadata").is_object() ? item.at("metadata") : Value::object({});
    steps.push_back(std::move(step));
  }
  return steps;
}

AutonomousPlan parse_plan_json(const std::string& text) {
  const auto object_start = text.find('{');
  const auto array_start = text.find('[');
  if (object_start == std::string::npos && array_start == std::string::npos) {
    throw AgentFrameworkError("Autonomous planner did not return JSON.");
  }
  const std::size_t start = object_start == std::string::npos
                                ? array_start
                                : (array_start == std::string::npos ? object_start : std::min(object_start, array_start));
  const char open = text[start];
  const char close = open == '[' ? ']' : '}';
  const auto end = text.rfind(close);
  if (end == std::string::npos || end < start) {
    throw AgentFrameworkError("Autonomous planner returned incomplete JSON.");
  }

  const auto parsed = parse_json(text.substr(start, end - start + 1));
  AutonomousPlan plan;
  if (parsed.is_array()) {
    plan.steps = plan_steps_from_value(parsed);
    return plan;
  }
  if (!parsed.is_object()) {
    throw AgentFrameworkError("Autonomous planner JSON must be an object or step array.");
  }
  plan.summary = parsed.at("summary").as_string();
  plan.steps = plan_steps_from_value(parsed.at("steps"));
  return plan;
}

std::string build_planning_prompt(const AutonomousRun& run) {
  std::ostringstream out;
  out << "Create a concise execution plan for this autonomous goal.\n";
  out << "Return only JSON with shape {\"summary\": string, \"steps\": [{\"title\": string, "
         "\"objective\": string, \"input\"?: unknown}]}.\n";
  out << "Goal: " << run.goal;
  if (!run.input.is_null()) {
    out << "\nInput: " << safe_json_stringify(run.input);
  }
  return out.str();
}

std::string build_step_prompt(const AutonomousRun& run,
                              const AutonomousStep& step,
                              const std::vector<AutonomousStep>& previous_steps) {
  Value::Array previous;
  for (const auto& candidate : previous_steps) {
    if (candidate.status == AutonomousStepStatus::Completed) {
      previous.push_back(Value::object({{"title", candidate.title}, {"output", candidate.output}}));
    }
  }

  std::vector<std::string> parts;
  parts.push_back("Goal: " + run.goal);
  if (!run.input.is_null()) {
    parts.push_back("Run input: " + safe_json_stringify(run.input));
  }
  if (!previous.empty()) {
    parts.push_back("Previous completed steps: " + safe_json_stringify(Value(previous)));
  }
  parts.push_back("Current step: " + step.title);
  parts.push_back("Step objective: " + step.objective);
  if (!step.input.is_null()) {
    parts.push_back("Step input: " + safe_json_stringify(step.input));
  }

  std::ostringstream out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index > 0) {
      out << "\n\n";
    }
    out << parts[index];
  }
  return out.str();
}

Value model_response_to_value(const ModelResponse& response) {
  return Value::object({
      {"id", response.id.empty() ? Value() : Value(response.id)},
      {"provider", response.provider},
      {"model", response.model},
      {"text", response.text},
      {"finishReason", response.finish_reason},
      {"reasoning", response.reasoning ? Value::object({{"text", response.reasoning->text},
                                                        {"format", response.reasoning->format}})
                                      : Value()},
      {"message", agent_message_to_value(assistant_message_from_response(response))},
      {"raw", response.raw},
  });
}

Value tool_invoke_result_to_value(const ToolInvokeResult& result) {
  if (std::holds_alternative<Value>(result)) {
    return std::get<Value>(result);
  }
  const auto& envelope = std::get<ToolResultEnvelope>(result);
  Value::Object object{{"metadata", envelope.metadata}};
  if (envelope.value) {
    object["value"] = *envelope.value;
  }
  if (envelope.content) {
    auto message = agent_message_to_value(create_message(MessageRole::Assistant, *envelope.content));
    object["content"] = message.at("content");
  }
  return Value(std::move(object));
}

Value tool_execution_result_to_value(const ToolExecutionResult& result) {
  Value::Object object{
      {"toolCall", Value::object({{"id", result.tool_call.id},
                                  {"name", result.tool_call.name},
                                  {"arguments", result.tool_call.arguments}})},
      {"ok", result.ok},
      {"error", result.error.empty() ? Value() : Value(result.error)},
      {"output", result.output},
      {"message", agent_message_to_value(result.message)},
  };
  if (result.result) {
    object["result"] = tool_invoke_result_to_value(*result.result);
  }
  return Value(std::move(object));
}

Value agent_trace_entry_to_value(const AgentTraceEntry& entry) {
  Value::Object object{{"type", entry.type}, {"iteration", entry.iteration}};
  if (entry.type == "model") {
    object["response"] = model_response_to_value(entry.response);
  }
  if (!entry.tool_results.empty()) {
    Value::Array results;
    for (const auto& result : entry.tool_results) {
      results.push_back(tool_execution_result_to_value(result));
    }
    object["toolResults"] = Value(std::move(results));
  }
  return Value(std::move(object));
}

Value agent_runner_result_to_value(const AgentRunnerRunResult& result) {
  Value::Array trace;
  for (const auto& entry : result.trace) {
    trace.push_back(agent_trace_entry_to_value(entry));
  }
  Value::Array messages;
  for (const auto& message : result.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array memory_hits;
  for (const auto& memory : result.memory_hits) {
    memory_hits.push_back(retrieved_memory_to_value(memory));
  }
  return Value::object({
      {"sessionId", result.session_id},
      {"iterationCount", result.iteration_count},
      {"text", result.text},
      {"response", model_response_to_value(result.response)},
      {"trace", Value(std::move(trace))},
      {"messages", Value(std::move(messages))},
      {"terminationReason", to_string(result.termination_reason)},
      {"memoryHits", Value(std::move(memory_hits))},
  });
}

Value planner_context(Value context, const AutonomousRun& run) {
  if (!context.is_object()) {
    context = Value::object({});
  }
  context["autonomousRunId"] = run.id;
  context["autonomousGoal"] = run.goal;
  return context;
}

Value executor_context(Value context, const AutonomousStepExecutionInput& input) {
  if (!context.is_object()) {
    context = Value::object({});
  }
  Value::Array previous;
  for (const auto& step : input.previous_steps) {
    previous.push_back(autonomous_step_to_value(step));
  }
  context["autonomousRunId"] = input.run.id;
  context["autonomousStepId"] = input.step.id;
  context["autonomousGoal"] = input.run.goal;
  context["previousSteps"] = Value(std::move(previous));
  return context;
}

std::string pg_autonomous_identifier(const std::string& value) {
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

Value pg_autonomous_json_param(const Value& value, bool null_when_null = false) {
  if (null_when_null && value.is_null()) {
    return Value();
  }
  return value.stringify(0);
}

Value pg_autonomous_optional_string_param(const std::string& value) {
  return value.empty() ? Value() : Value(value);
}

Value pg_autonomous_json_from_value(const Value& value) {
  if (!value.is_string()) {
    return value;
  }
  try {
    return parse_json(value.as_string());
  } catch (...) {
    return value;
  }
}

std::size_t pg_autonomous_size_from_value(const Value& value) {
  if (value.is_number()) {
    return static_cast<std::size_t>(std::max<long long>(0, value.as_integer()));
  }
  if (value.is_string()) {
    try {
      return static_cast<std::size_t>(std::max<long long>(0, std::stoll(value.as_string())));
    } catch (...) {
      return 0;
    }
  }
  return 0;
}

int pg_autonomous_int_from_value(const Value& value) {
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

AutonomousRun autonomous_run_from_pg_row(const Value& row) {
  AutonomousRun run;
  run.id = row.at("id").as_string();
  run.goal = row.at("goal").as_string();
  run.input = row.at("input").is_null() ? Value() : pg_autonomous_json_from_value(row.at("input"));
  run.status = autonomous_run_status_from_string(row.at("status").as_string("queued"));
  run.output = row.at("output").is_null() ? Value() : pg_autonomous_json_from_value(row.at("output"));
  run.error = row.at("error").as_string();
  run.created_at = row.at("created_at").as_string();
  run.updated_at = row.at("updated_at").as_string();
  run.completed_at = row.at("completed_at").as_string();
  run.metadata = row.at("metadata").is_null() ? Value::object({}) : pg_autonomous_json_from_value(row.at("metadata"));
  return run;
}

AutonomousStep autonomous_step_from_pg_row(const Value& row) {
  AutonomousStep step;
  step.id = row.at("id").as_string();
  step.run_id = row.at("run_id").as_string();
  step.index = pg_autonomous_size_from_value(row.at("step_index"));
  step.title = row.at("title").as_string();
  step.objective = row.at("objective").as_string();
  step.input = row.at("input").is_null() ? Value() : pg_autonomous_json_from_value(row.at("input"));
  step.status = autonomous_step_status_from_string(row.at("status").as_string("pending"));
  step.attempts = pg_autonomous_int_from_value(row.at("attempts"));
  step.output = row.at("output").is_null() ? Value() : pg_autonomous_json_from_value(row.at("output"));
  step.error = row.at("error").as_string();
  step.wait_reason = row.at("wait_reason").as_string();
  const auto depends_on = pg_autonomous_json_from_value(row.at("depends_on"));
  step.depends_on = depends_on.is_array() ? string_array_from_value(depends_on) : std::vector<std::string>{};
  step.created_at = row.at("created_at").as_string();
  step.updated_at = row.at("updated_at").as_string();
  step.started_at = row.at("started_at").as_string();
  step.completed_at = row.at("completed_at").as_string();
  step.metadata = row.at("metadata").is_null() ? Value::object({}) : pg_autonomous_json_from_value(row.at("metadata"));
  return step;
}

AutonomousEvent autonomous_event_from_pg_row(const Value& row) {
  AutonomousEvent event;
  event.id = row.at("id").as_string();
  event.run_id = row.at("run_id").as_string();
  event.step_id = row.at("step_id").as_string();
  event.type = row.at("type").as_string();
  event.payload = row.at("payload").is_null() ? Value() : pg_autonomous_json_from_value(row.at("payload"));
  event.created_at = row.at("created_at").as_string();
  return event;
}

AutonomousCheckpoint autonomous_checkpoint_from_pg_row(const Value& row) {
  AutonomousCheckpoint checkpoint;
  checkpoint.id = row.at("id").as_string();
  checkpoint.run_id = row.at("run_id").as_string();
  checkpoint.step_id = row.at("step_id").as_string();
  checkpoint.name = row.at("name").as_string();
  checkpoint.state = pg_autonomous_json_from_value(row.at("state"));
  checkpoint.created_at = row.at("created_at").as_string();
  return checkpoint;
}

const Value& require_pg_autonomous_row(const std::vector<Value>& rows, const std::string& message) {
  if (rows.empty()) {
    throw ConfigurationError(message);
  }
  return rows.front();
}

}  // namespace

std::string to_string(AutonomousRunStatus status) {
  switch (status) {
    case AutonomousRunStatus::Queued:
      return "queued";
    case AutonomousRunStatus::Running:
      return "running";
    case AutonomousRunStatus::Waiting:
      return "waiting";
    case AutonomousRunStatus::Completed:
      return "completed";
    case AutonomousRunStatus::Failed:
      return "failed";
    case AutonomousRunStatus::Cancelled:
      return "cancelled";
    case AutonomousRunStatus::Interrupted:
      return "interrupted";
  }
  return "queued";
}

std::string to_string(AutonomousStepStatus status) {
  switch (status) {
    case AutonomousStepStatus::Pending:
      return "pending";
    case AutonomousStepStatus::Running:
      return "running";
    case AutonomousStepStatus::Waiting:
      return "waiting";
    case AutonomousStepStatus::Completed:
      return "completed";
    case AutonomousStepStatus::Failed:
      return "failed";
    case AutonomousStepStatus::Cancelled:
      return "cancelled";
    case AutonomousStepStatus::Skipped:
      return "skipped";
  }
  return "pending";
}

AutonomousRunStatus autonomous_run_status_from_string(const std::string& value) {
  if (value == "running") return AutonomousRunStatus::Running;
  if (value == "waiting") return AutonomousRunStatus::Waiting;
  if (value == "completed") return AutonomousRunStatus::Completed;
  if (value == "failed") return AutonomousRunStatus::Failed;
  if (value == "cancelled") return AutonomousRunStatus::Cancelled;
  if (value == "interrupted") return AutonomousRunStatus::Interrupted;
  return AutonomousRunStatus::Queued;
}

AutonomousStepStatus autonomous_step_status_from_string(const std::string& value) {
  if (value == "running") return AutonomousStepStatus::Running;
  if (value == "waiting") return AutonomousStepStatus::Waiting;
  if (value == "completed") return AutonomousStepStatus::Completed;
  if (value == "failed") return AutonomousStepStatus::Failed;
  if (value == "cancelled") return AutonomousStepStatus::Cancelled;
  if (value == "skipped") return AutonomousStepStatus::Skipped;
  return AutonomousStepStatus::Pending;
}

AutonomousRun InMemoryAutonomousStore::create_run(CreateAutonomousRunInput input) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (input.id.empty()) {
    input.id = generate_uuid();
  }
  if (runs_.contains(input.id)) {
    throw AgentFrameworkError("Autonomous run already exists: " + input.id);
  }
  const auto created = now_iso8601();
  AutonomousRun run;
  run.id = std::move(input.id);
  run.goal = std::move(input.goal);
  run.input = std::move(input.input);
  run.metadata = input.metadata.is_object() ? std::move(input.metadata) : Value::object({});
  run.created_at = created;
  run.updated_at = created;
  runs_[run.id] = run;
  steps_[run.id] = {};
  events_[run.id] = {};
  checkpoints_[run.id] = {};
  return run;
}

std::optional<AutonomousRunSnapshot> InMemoryAutonomousStore::get_run(const std::string& run_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto found = runs_.find(run_id);
  if (found == runs_.end()) {
    return std::nullopt;
  }
  return AutonomousRunSnapshot{
      .run = found->second,
      .steps = steps_[run_id],
      .events = events_[run_id],
      .checkpoints = checkpoints_[run_id],
  };
}

AutonomousRun InMemoryAutonomousStore::update_run(const std::string& run_id, const AutonomousRunPatch& patch) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto& run = require_run(run_id);
  if (patch.has_goal) {
    run.goal = patch.goal;
  }
  if (patch.has_input) {
    run.input = patch.input;
  }
  if (patch.status) {
    run.status = *patch.status;
  }
  if (patch.has_output) {
    run.output = patch.output;
  }
  if (patch.has_error) {
    run.error = patch.error;
  }
  if (patch.has_completed_at) {
    run.completed_at = patch.completed_at;
  }
  if (patch.has_metadata) {
    run.metadata = patch.metadata.is_object() ? patch.metadata : Value::object({});
  }
  run.updated_at = patch.updated_at.empty() ? now_iso8601() : patch.updated_at;
  return run;
}

std::vector<AutonomousStep> InMemoryAutonomousStore::replace_steps(
    const std::string& run_id,
    const std::vector<AutonomousPlanStep>& steps) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  (void)require_run(run_id);
  auto normalized = normalize_steps(run_id, 0, steps);
  steps_[run_id] = normalized;
  return normalized;
}

std::vector<AutonomousStep> InMemoryAutonomousStore::append_steps(
    const std::string& run_id,
    const std::vector<AutonomousPlanStep>& steps) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  (void)require_run(run_id);
  auto& current = steps_[run_id];
  auto normalized = normalize_steps(run_id, current.size(), steps);
  current.insert(current.end(), normalized.begin(), normalized.end());
  return normalized;
}

AutonomousStep InMemoryAutonomousStore::update_step(
    const std::string& run_id,
    const std::string& step_id,
    const AutonomousStepPatch& patch) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  (void)require_run(run_id);
  auto& steps = steps_[run_id];
  auto found = std::find_if(steps.begin(), steps.end(), [&](const auto& step) {
    return step.id == step_id;
  });
  if (found == steps.end()) {
    throw AgentFrameworkError("Autonomous step not found: " + step_id);
  }
  if (patch.has_title) {
    found->title = patch.title;
  }
  if (patch.has_objective) {
    found->objective = patch.objective;
  }
  if (patch.has_input) {
    found->input = patch.input;
  }
  if (patch.status) {
    found->status = *patch.status;
  }
  if (patch.has_output) {
    found->output = patch.output;
  }
  if (patch.has_error) {
    found->error = patch.error;
  }
  if (patch.has_wait_reason) {
    found->wait_reason = patch.wait_reason;
  }
  if (patch.has_depends_on) {
    found->depends_on = patch.depends_on;
  }
  if (patch.has_metadata) {
    found->metadata = patch.metadata.is_object() ? patch.metadata : Value::object({});
  }
  if (patch.has_started_at) {
    found->started_at = patch.started_at;
  }
  if (patch.has_completed_at) {
    found->completed_at = patch.completed_at;
  }
  if (patch.attempts) {
    found->attempts = *patch.attempts;
  }
  found->updated_at = patch.updated_at.empty() ? now_iso8601() : patch.updated_at;
  return *found;
}

AutonomousEvent InMemoryAutonomousStore::append_event(AutonomousEvent event) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  (void)require_run(event.run_id);
  if (event.id.empty()) {
    event.id = generate_uuid();
  }
  if (event.created_at.empty()) {
    event.created_at = now_iso8601();
  }
  events_[event.run_id].push_back(event);
  return event;
}

AutonomousCheckpoint InMemoryAutonomousStore::append_checkpoint(AutonomousCheckpoint checkpoint) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  (void)require_run(checkpoint.run_id);
  if (checkpoint.id.empty()) {
    checkpoint.id = generate_uuid();
  }
  if (checkpoint.created_at.empty()) {
    checkpoint.created_at = now_iso8601();
  }
  checkpoints_[checkpoint.run_id].push_back(checkpoint);
  return checkpoint;
}

std::vector<AutonomousRun> InMemoryAutonomousStore::list_runs(AutonomousRunFilter filter) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<AutonomousRun> runs;
  for (const auto& [_, run] : runs_) {
    if (filter.status && run.status != *filter.status) {
      continue;
    }
    if (!metadata_matches(run.metadata, filter.metadata)) {
      continue;
    }
    runs.push_back(run);
  }
  return runs;
}

AutonomousRun& InMemoryAutonomousStore::require_run(const std::string& run_id) {
  const auto found = runs_.find(run_id);
  if (found == runs_.end()) {
    throw AgentFrameworkError("Autonomous run not found: " + run_id);
  }
  return found->second;
}

std::vector<AutonomousStep> InMemoryAutonomousStore::normalize_steps(
    const std::string& run_id,
    std::size_t existing_count,
    const std::vector<AutonomousPlanStep>& steps) const {
  std::vector<AutonomousStep> normalized;
  const auto created = now_iso8601();
  for (std::size_t offset = 0; offset < steps.size(); ++offset) {
    const auto& step = steps[offset];
    AutonomousStep normalized_step;
    normalized_step.id = step.id.empty() ? generate_uuid() : step.id;
    normalized_step.run_id = run_id;
    normalized_step.index = existing_count + offset;
    normalized_step.title = step.title;
    normalized_step.objective = step.objective.empty() ? step.title : step.objective;
    normalized_step.input = step.input;
    normalized_step.depends_on = step.depends_on;
    normalized_step.metadata = step.metadata.is_object() ? step.metadata : Value::object({});
    normalized_step.created_at = created;
    normalized_step.updated_at = created;
    normalized.push_back(std::move(normalized_step));
  }
  return normalized;
}

FileAutonomousStore::FileAutonomousStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

AutonomousRun FileAutonomousStore::create_run(CreateAutonomousRunInput input) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto run = InMemoryAutonomousStore::create_run(std::move(input));
  persist();
  return run;
}

std::optional<AutonomousRunSnapshot> FileAutonomousStore::get_run(const std::string& run_id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryAutonomousStore::get_run(run_id);
}

AutonomousRun FileAutonomousStore::update_run(const std::string& run_id, const AutonomousRunPatch& patch) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto run = InMemoryAutonomousStore::update_run(run_id, patch);
  persist();
  return run;
}

std::vector<AutonomousStep> FileAutonomousStore::replace_steps(
    const std::string& run_id,
    const std::vector<AutonomousPlanStep>& steps) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto output = InMemoryAutonomousStore::replace_steps(run_id, steps);
  persist();
  return output;
}

std::vector<AutonomousStep> FileAutonomousStore::append_steps(
    const std::string& run_id,
    const std::vector<AutonomousPlanStep>& steps) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto output = InMemoryAutonomousStore::append_steps(run_id, steps);
  persist();
  return output;
}

AutonomousStep FileAutonomousStore::update_step(
    const std::string& run_id,
    const std::string& step_id,
    const AutonomousStepPatch& patch) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto step = InMemoryAutonomousStore::update_step(run_id, step_id, patch);
  persist();
  return step;
}

AutonomousEvent FileAutonomousStore::append_event(AutonomousEvent event) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto output = InMemoryAutonomousStore::append_event(std::move(event));
  persist();
  return output;
}

AutonomousCheckpoint FileAutonomousStore::append_checkpoint(AutonomousCheckpoint checkpoint) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto output = InMemoryAutonomousStore::append_checkpoint(std::move(checkpoint));
  persist();
  return output;
}

std::vector<AutonomousRun> FileAutonomousStore::list_runs(AutonomousRunFilter filter) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryAutonomousStore::list_runs(std::move(filter));
}

void FileAutonomousStore::ensure_loaded() {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  if (!std::filesystem::exists(file_path_)) {
    return;
  }
  const auto root = read_json_file(file_path_);
  for (const auto& item : root.at("runs").as_array()) {
    auto run = autonomous_run_from_value(item);
    runs_[run.id] = run;
  }
  for (const auto& group : root.at("steps").as_array()) {
    const auto run_id = group.at("runId").as_string();
    for (const auto& item : group.at("items").as_array()) {
      steps_[run_id].push_back(autonomous_step_from_value(item));
    }
  }
  for (const auto& group : root.at("events").as_array()) {
    const auto run_id = group.at("runId").as_string();
    for (const auto& item : group.at("items").as_array()) {
      events_[run_id].push_back(autonomous_event_from_value(item));
    }
  }
  for (const auto& group : root.at("checkpoints").as_array()) {
    const auto run_id = group.at("runId").as_string();
    for (const auto& item : group.at("items").as_array()) {
      checkpoints_[run_id].push_back(autonomous_checkpoint_from_value(item));
    }
  }
}

void FileAutonomousStore::persist() const {
  Value::Array runs;
  for (const auto& [_, run] : runs_) {
    runs.push_back(autonomous_run_to_value(run));
  }
  const auto groups_to_value = [](const auto& groups, auto serializer) {
    Value::Array output;
    for (const auto& [run_id, items] : groups) {
      Value::Array serialized;
      for (const auto& item : items) {
        serialized.push_back(serializer(item));
      }
      output.push_back(Value::object({{"runId", run_id}, {"items", Value(serialized)}}));
    }
    return Value(output);
  };
  write_json_file(file_path_, Value::object({
      {"runs", Value(runs)},
      {"steps", groups_to_value(steps_, autonomous_step_to_value)},
      {"events", groups_to_value(events_, autonomous_event_to_value)},
      {"checkpoints", groups_to_value(checkpoints_, autonomous_checkpoint_to_value)},
  }));
}

PgAutonomousStore::PgAutonomousStore(PgAutonomousStoreConfig config)
    : client_(std::move(config.client)),
      schema_name_(config.schema_name.empty() ? "public" : std::move(config.schema_name)),
      table_prefix_(config.table_prefix.empty() ? "node_agent" : std::move(config.table_prefix)),
      create_tables_(config.create_tables),
      close_client_on_close_(config.close_client_on_close) {
  if (!client_) {
    throw ConfigurationError("PgAutonomousStore requires an injected PgAutonomousClient.");
  }
}

PgAutonomousStore::~PgAutonomousStore() {
  close();
}

AutonomousRun PgAutonomousStore::create_run(CreateAutonomousRunInput input) {
  ensure_ready();
  const auto id = input.id.empty() ? generate_uuid() : std::move(input.id);
  const auto created = now_iso8601();
  const auto metadata = input.metadata.is_object() ? input.metadata : Value::object({});
  const auto rows = client_->query(
      "INSERT INTO " + runs_table() + " ("
      "id, goal, input, status, output, error, created_at, updated_at, completed_at, metadata"
      ") VALUES ($1, $2, $3::jsonb, 'queued', NULL, NULL, $4, $4, NULL, $5::jsonb) RETURNING *",
      {id, input.goal, pg_autonomous_json_param(input.input), created, pg_autonomous_json_param(metadata)});
  return autonomous_run_from_pg_row(require_pg_autonomous_row(rows, "Autonomous run insert returned no row: " + id));
}

std::optional<AutonomousRunSnapshot> PgAutonomousStore::get_run(const std::string& run_id) {
  ensure_ready();
  const auto run_rows = client_->query("SELECT * FROM " + runs_table() + " WHERE id = $1", {run_id});
  if (run_rows.empty()) {
    return std::nullopt;
  }
  AutonomousRunSnapshot snapshot;
  snapshot.run = autonomous_run_from_pg_row(run_rows.front());
  for (const auto& row : client_->query(
           "SELECT * FROM " + steps_table() + " WHERE run_id = $1 ORDER BY step_index ASC", {run_id})) {
    snapshot.steps.push_back(autonomous_step_from_pg_row(row));
  }
  for (const auto& row : client_->query(
           "SELECT * FROM " + events_table() + " WHERE run_id = $1 ORDER BY created_at ASC", {run_id})) {
    snapshot.events.push_back(autonomous_event_from_pg_row(row));
  }
  for (const auto& row : client_->query(
           "SELECT * FROM " + checkpoints_table() + " WHERE run_id = $1 ORDER BY created_at ASC", {run_id})) {
    snapshot.checkpoints.push_back(autonomous_checkpoint_from_pg_row(row));
  }
  return snapshot;
}

AutonomousRun PgAutonomousStore::update_run(const std::string& run_id, const AutonomousRunPatch& patch) {
  ensure_ready();
  std::vector<std::string> updates;
  std::vector<Value> params;
  const auto add_text = [&](const std::string& column, const std::string& value) {
    updates.push_back(column + " = $" + std::to_string(params.size() + 1));
    params.push_back(value);
  };
  const auto add_optional_text = [&](const std::string& column, const std::string& value) {
    updates.push_back(column + " = $" + std::to_string(params.size() + 1));
    params.push_back(pg_autonomous_optional_string_param(value));
  };
  const auto add_json = [&](const std::string& column, const Value& value) {
    updates.push_back(column + " = $" + std::to_string(params.size() + 1) + "::jsonb");
    params.push_back(pg_autonomous_json_param(value));
  };

  if (patch.has_goal) add_text("goal", patch.goal);
  if (patch.has_input) add_json("input", patch.input);
  if (patch.status) add_text("status", to_string(*patch.status));
  if (patch.has_output) add_json("output", patch.output);
  if (patch.has_error) add_optional_text("error", patch.error);
  add_text("updated_at", patch.updated_at.empty() ? now_iso8601() : patch.updated_at);
  if (patch.has_completed_at) add_optional_text("completed_at", patch.completed_at);
  if (patch.has_metadata) add_json("metadata", patch.metadata.is_object() ? patch.metadata : Value::object({}));

  params.push_back(run_id);
  const auto rows = client_->query("UPDATE " + runs_table() + " SET " + [&]() {
    std::ostringstream out;
    for (std::size_t index = 0; index < updates.size(); ++index) {
      if (index > 0) out << ", ";
      out << updates[index];
    }
    return out.str();
  }() + " WHERE id = $" + std::to_string(params.size()) + " RETURNING *", params);
  return autonomous_run_from_pg_row(require_pg_autonomous_row(rows, "Autonomous run not found: " + run_id));
}

std::vector<AutonomousStep> PgAutonomousStore::replace_steps(
    const std::string& run_id,
    const std::vector<AutonomousPlanStep>& steps) {
  ensure_ready();
  auto tx = client_->connect();
  PgAutonomousClient& client = tx ? *tx : *client_;
  try {
    client.query("BEGIN");
    client.query("DELETE FROM " + steps_table() + " WHERE run_id = $1", {run_id});
    auto inserted = insert_steps(client, run_id, normalize_steps(run_id, 0, steps));
    client.query("COMMIT");
    if (tx) {
      tx->release();
    }
    return inserted;
  } catch (...) {
    try {
      client.query("ROLLBACK");
    } catch (...) {
    }
    if (tx) {
      tx->release();
    }
    throw;
  }
}

std::vector<AutonomousStep> PgAutonomousStore::append_steps(
    const std::string& run_id,
    const std::vector<AutonomousPlanStep>& steps) {
  ensure_ready();
  const auto count_rows = client_->query(
      "SELECT COUNT(*)::text AS count FROM " + steps_table() + " WHERE run_id = $1", {run_id});
  const auto existing_count = count_rows.empty() ? 0 : pg_autonomous_size_from_value(count_rows.front().at("count"));
  return insert_steps(*client_, run_id, normalize_steps(run_id, existing_count, steps));
}

AutonomousStep PgAutonomousStore::update_step(
    const std::string& run_id,
    const std::string& step_id,
    const AutonomousStepPatch& patch) {
  ensure_ready();
  std::vector<std::string> updates;
  std::vector<Value> params;
  const auto add_text = [&](const std::string& column, const std::string& value) {
    updates.push_back(column + " = $" + std::to_string(params.size() + 1));
    params.push_back(value);
  };
  const auto add_optional_text = [&](const std::string& column, const std::string& value) {
    updates.push_back(column + " = $" + std::to_string(params.size() + 1));
    params.push_back(pg_autonomous_optional_string_param(value));
  };
  const auto add_json = [&](const std::string& column, const Value& value) {
    updates.push_back(column + " = $" + std::to_string(params.size() + 1) + "::jsonb");
    params.push_back(pg_autonomous_json_param(value));
  };

  if (patch.has_title) add_text("title", patch.title);
  if (patch.has_objective) add_text("objective", patch.objective);
  if (patch.has_input) add_json("input", patch.input);
  if (patch.status) add_text("status", to_string(*patch.status));
  if (patch.attempts) {
    updates.push_back("attempts = $" + std::to_string(params.size() + 1));
    params.push_back(*patch.attempts);
  }
  if (patch.has_output) add_json("output", patch.output);
  if (patch.has_error) add_optional_text("error", patch.error);
  if (patch.has_wait_reason) add_optional_text("wait_reason", patch.wait_reason);
  if (patch.has_depends_on) add_json("depends_on", string_array_to_value(patch.depends_on));
  add_text("updated_at", patch.updated_at.empty() ? now_iso8601() : patch.updated_at);
  if (patch.has_started_at) add_optional_text("started_at", patch.started_at);
  if (patch.has_completed_at) add_optional_text("completed_at", patch.completed_at);
  if (patch.has_metadata) add_json("metadata", patch.metadata.is_object() ? patch.metadata : Value::object({}));

  params.push_back(run_id);
  params.push_back(step_id);
  const auto rows = client_->query("UPDATE " + steps_table() + " SET " + [&]() {
    std::ostringstream out;
    for (std::size_t index = 0; index < updates.size(); ++index) {
      if (index > 0) out << ", ";
      out << updates[index];
    }
    return out.str();
  }() + " WHERE run_id = $" + std::to_string(params.size() - 1) +
                                " AND id = $" + std::to_string(params.size()) + " RETURNING *",
      params);
  return autonomous_step_from_pg_row(require_pg_autonomous_row(rows, "Autonomous step not found: " + step_id));
}

AutonomousEvent PgAutonomousStore::append_event(AutonomousEvent event) {
  ensure_ready();
  if (event.id.empty()) {
    event.id = generate_uuid();
  }
  if (event.created_at.empty()) {
    event.created_at = now_iso8601();
  }
  const auto rows = client_->query(
      "INSERT INTO " + events_table() + " (id, run_id, step_id, type, payload, created_at) "
      "VALUES ($1, $2, $3, $4, $5::jsonb, $6) RETURNING *",
      {event.id, event.run_id, pg_autonomous_optional_string_param(event.step_id), event.type,
       pg_autonomous_json_param(event.payload, true), event.created_at});
  return autonomous_event_from_pg_row(require_pg_autonomous_row(rows, "Autonomous event insert returned no row: " +
                                                                         event.id));
}

AutonomousCheckpoint PgAutonomousStore::append_checkpoint(AutonomousCheckpoint checkpoint) {
  ensure_ready();
  if (checkpoint.id.empty()) {
    checkpoint.id = generate_uuid();
  }
  if (checkpoint.created_at.empty()) {
    checkpoint.created_at = now_iso8601();
  }
  const auto rows = client_->query(
      "INSERT INTO " + checkpoints_table() + " (id, run_id, step_id, name, state, created_at) "
      "VALUES ($1, $2, $3, $4, $5::jsonb, $6) RETURNING *",
      {checkpoint.id, checkpoint.run_id, pg_autonomous_optional_string_param(checkpoint.step_id), checkpoint.name,
       pg_autonomous_json_param(checkpoint.state), checkpoint.created_at});
  return autonomous_checkpoint_from_pg_row(require_pg_autonomous_row(
      rows, "Autonomous checkpoint insert returned no row: " + checkpoint.id));
}

std::vector<AutonomousRun> PgAutonomousStore::list_runs(AutonomousRunFilter filter) {
  ensure_ready();
  std::vector<std::string> conditions;
  std::vector<Value> params;
  if (filter.status) {
    conditions.push_back("status = $" + std::to_string(params.size() + 1));
    params.push_back(to_string(*filter.status));
  }
  if (filter.metadata.is_object() && !filter.metadata.as_object().empty()) {
    conditions.push_back("metadata @> $" + std::to_string(params.size() + 1) + "::jsonb");
    params.push_back(pg_autonomous_json_param(filter.metadata));
  }
  std::string where;
  if (!conditions.empty()) {
    std::ostringstream out;
    out << " WHERE ";
    for (std::size_t index = 0; index < conditions.size(); ++index) {
      if (index > 0) out << " AND ";
      out << conditions[index];
    }
    where = out.str();
  }
  const auto rows = client_->query("SELECT * FROM " + runs_table() + where + " ORDER BY created_at ASC", params);
  std::vector<AutonomousRun> runs;
  runs.reserve(rows.size());
  for (const auto& row : rows) {
    runs.push_back(autonomous_run_from_pg_row(row));
  }
  return runs;
}

void PgAutonomousStore::close() {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (!close_client_on_close_ || !client_) {
    return;
  }
  client_->close();
  close_client_on_close_ = false;
}

std::string PgAutonomousStore::table_name(const std::string& suffix) const {
  return pg_autonomous_identifier(schema_name_) + "." + pg_autonomous_identifier(table_prefix_ + suffix);
}

std::string PgAutonomousStore::runs_table() const {
  return table_name("_autonomous_runs");
}

std::string PgAutonomousStore::steps_table() const {
  return table_name("_autonomous_steps");
}

std::string PgAutonomousStore::events_table() const {
  return table_name("_autonomous_events");
}

std::string PgAutonomousStore::checkpoints_table() const {
  return table_name("_autonomous_checkpoints");
}

std::vector<AutonomousStep> PgAutonomousStore::normalize_steps(
    const std::string& run_id,
    std::size_t existing_count,
    const std::vector<AutonomousPlanStep>& steps) const {
  std::vector<AutonomousStep> normalized;
  const auto created = now_iso8601();
  for (std::size_t offset = 0; offset < steps.size(); ++offset) {
    const auto& step = steps[offset];
    AutonomousStep normalized_step;
    normalized_step.id = step.id.empty() ? generate_uuid() : step.id;
    normalized_step.run_id = run_id;
    normalized_step.index = existing_count + offset;
    normalized_step.title = step.title;
    normalized_step.objective = step.objective.empty() ? step.title : step.objective;
    normalized_step.input = step.input;
    normalized_step.depends_on = step.depends_on;
    normalized_step.metadata = step.metadata.is_object() ? step.metadata : Value::object({});
    normalized_step.created_at = created;
    normalized_step.updated_at = created;
    normalized.push_back(std::move(normalized_step));
  }
  return normalized;
}

std::vector<AutonomousStep> PgAutonomousStore::insert_steps(
    PgAutonomousClient& client,
    const std::string& run_id,
    const std::vector<AutonomousStep>& steps) const {
  std::vector<AutonomousStep> inserted;
  inserted.reserve(steps.size());
  for (const auto& step : steps) {
    const auto rows = client.query(
        "INSERT INTO " + steps_table() + " ("
        "id, run_id, step_index, title, objective, input, status, attempts, output, error, wait_reason, "
        "depends_on, created_at, updated_at, started_at, completed_at, metadata"
        ") VALUES ($1, $2, $3, $4, $5, $6::jsonb, $7, $8, $9::jsonb, $10, $11, "
        "$12::jsonb, $13, $14, $15, $16, $17::jsonb) RETURNING *",
        {step.id, run_id, step.index, step.title, step.objective, pg_autonomous_json_param(step.input),
         to_string(step.status), step.attempts, pg_autonomous_json_param(step.output, true),
         pg_autonomous_optional_string_param(step.error), pg_autonomous_optional_string_param(step.wait_reason),
         pg_autonomous_json_param(string_array_to_value(step.depends_on)), step.created_at, step.updated_at,
         pg_autonomous_optional_string_param(step.started_at), pg_autonomous_optional_string_param(step.completed_at),
         pg_autonomous_json_param(step.metadata)});
    inserted.push_back(autonomous_step_from_pg_row(
        require_pg_autonomous_row(rows, "Autonomous step insert returned no row: " + step.id)));
  }
  return inserted;
}

void PgAutonomousStore::ensure_ready() const {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (ready_ || !create_tables_) {
    return;
  }
  client_->query("CREATE SCHEMA IF NOT EXISTS " + pg_autonomous_identifier(schema_name_));
  client_->query("CREATE TABLE IF NOT EXISTS " + runs_table() + " ("
                 "id TEXT PRIMARY KEY,"
                 "goal TEXT NOT NULL,"
                 "input JSONB,"
                 "status TEXT NOT NULL,"
                 "output JSONB,"
                 "error TEXT,"
                 "created_at TEXT NOT NULL,"
                 "updated_at TEXT NOT NULL,"
                 "completed_at TEXT,"
                 "metadata JSONB NOT NULL DEFAULT '{}'::jsonb"
                 ")");
  client_->query("CREATE INDEX IF NOT EXISTS " +
                 pg_autonomous_identifier(table_prefix_ + "_autonomous_runs_status_idx") + " ON " +
                 runs_table() + " (status, created_at)");
  client_->query("CREATE INDEX IF NOT EXISTS " +
                 pg_autonomous_identifier(table_prefix_ + "_autonomous_runs_metadata_idx") + " ON " +
                 runs_table() + " USING GIN (metadata)");
  client_->query("CREATE TABLE IF NOT EXISTS " + steps_table() + " ("
                 "id TEXT PRIMARY KEY,"
                 "run_id TEXT NOT NULL REFERENCES " + runs_table() + "(id) ON DELETE CASCADE,"
                 "step_index INTEGER NOT NULL,"
                 "title TEXT NOT NULL,"
                 "objective TEXT NOT NULL,"
                 "input JSONB,"
                 "status TEXT NOT NULL,"
                 "attempts INTEGER NOT NULL,"
                 "output JSONB,"
                 "error TEXT,"
                 "wait_reason TEXT,"
                 "depends_on JSONB NOT NULL DEFAULT '[]'::jsonb,"
                 "created_at TEXT NOT NULL,"
                 "updated_at TEXT NOT NULL,"
                 "started_at TEXT,"
                 "completed_at TEXT,"
                 "metadata JSONB NOT NULL DEFAULT '{}'::jsonb"
                 ")");
  client_->query("CREATE INDEX IF NOT EXISTS " +
                 pg_autonomous_identifier(table_prefix_ + "_autonomous_steps_run_idx") + " ON " +
                 steps_table() + " (run_id, step_index)");
  client_->query("CREATE TABLE IF NOT EXISTS " + events_table() + " ("
                 "id TEXT PRIMARY KEY,"
                 "run_id TEXT NOT NULL REFERENCES " + runs_table() + "(id) ON DELETE CASCADE,"
                 "step_id TEXT,"
                 "type TEXT NOT NULL,"
                 "payload JSONB,"
                 "created_at TEXT NOT NULL"
                 ")");
  client_->query("CREATE TABLE IF NOT EXISTS " + checkpoints_table() + " ("
                 "id TEXT PRIMARY KEY,"
                 "run_id TEXT NOT NULL REFERENCES " + runs_table() + "(id) ON DELETE CASCADE,"
                 "step_id TEXT,"
                 "name TEXT NOT NULL,"
                 "state JSONB NOT NULL,"
                 "created_at TEXT NOT NULL"
                 ")");
  ready_ = true;
}

StaticAutonomousPlanner::StaticAutonomousPlanner(std::vector<AutonomousPlanStep> steps) {
  plan_.steps = std::move(steps);
}

StaticAutonomousPlanner::StaticAutonomousPlanner(AutonomousPlan plan) : plan_(std::move(plan)) {}

StaticAutonomousPlanner::StaticAutonomousPlanner(AutonomousPlannerHandler handler)
    : handler_(std::move(handler)) {}

AutonomousPlan StaticAutonomousPlanner::plan(const AutonomousPlanningInput& input) {
  return handler_ ? handler_(input) : plan_;
}

AgentAutonomousPlanner::AgentAutonomousPlanner(AgentAutonomousPlannerConfig config)
    : runner_(config.runner),
      session_id_(std::move(config.session_id)),
      model_settings_(std::move(config.model_settings)),
      context_(std::move(config.context)) {
  if (!runner_) {
    throw ConfigurationError("AgentAutonomousPlanner requires an AgentRunner.");
  }
}

AutonomousPlan AgentAutonomousPlanner::plan(const AutonomousPlanningInput& input) {
  const auto session_id = session_id_.empty() ? "autonomous:" + input.run.id + ":planner" : session_id_;
  auto result = runner_->run(build_planning_prompt(input.run),
                             session_id,
                             model_settings_,
                             RunnerRetrievalOptions{},
                             RunnerWritebackOptions{},
                             std::vector<SkillActivation>{},
                             planner_context(context_, input.run));
  return parse_plan_json(result.text);
}

CallbackAutonomousStepExecutor::CallbackAutonomousStepExecutor(AutonomousStepExecutorHandler handler)
    : handler_(std::move(handler)) {
  if (!handler_) {
    throw ConfigurationError("CallbackAutonomousStepExecutor requires a handler.");
  }
}

AutonomousStepExecutionResult CallbackAutonomousStepExecutor::execute(const AutonomousStepExecutionInput& input) {
  return handler_(input);
}

AgentRunnerStepExecutor::AgentRunnerStepExecutor(AgentRunnerStepExecutorConfig config)
    : runner_(config.runner),
      session_id_(std::move(config.session_id)),
      session_resolver_(std::move(config.session_resolver)),
      model_settings_(std::move(config.model_settings)),
      context_(std::move(config.context)) {
  if (!runner_) {
    throw ConfigurationError("AgentRunnerStepExecutor requires an AgentRunner.");
  }
}

AutonomousStepExecutionResult AgentRunnerStepExecutor::execute(const AutonomousStepExecutionInput& input) {
  std::string session_id;
  if (session_resolver_) {
    session_id = session_resolver_(input);
  }
  if (session_id.empty()) {
    session_id = session_id_.empty() ? "autonomous:" + input.run.id : session_id_;
  }
  auto result = runner_->run(build_step_prompt(input.run, input.step, input.previous_steps),
                             session_id,
                             model_settings_,
                             RunnerRetrievalOptions{},
                             RunnerWritebackOptions{},
                             std::vector<SkillActivation>{},
                             executor_context(context_, input));
  const auto output = agent_runner_result_to_value(result);
  if (input.checkpoint) {
    input.checkpoint("runner.state", output);
  }
  Value metadata = Value::object({{"text", result.text}});
  return AutonomousStepExecutionResult{
      .status = AutonomousStepStatus::Completed,
      .output = output,
      .metadata = metadata,
  };
}

AutonomousRunManager::AutonomousRunManager(AutonomousRunManagerConfig config)
    : store_(config.store),
      planner_(config.planner),
      executor_(config.executor),
      max_steps_per_run_(config.max_steps_per_run == 0 ? 64 : config.max_steps_per_run) {
  if (!store_ || !planner_ || !executor_) {
    throw ConfigurationError("AutonomousRunManager requires store, planner, and executor.");
  }
}

AutonomousRun AutonomousRunManager::create_run(CreateAutonomousRunInput input) {
  auto run = store_->create_run(std::move(input));
  store_->append_event(AutonomousEvent{
      .run_id = run.id,
      .type = "autonomous.run.created",
      .payload = Value::object({{"goal", run.goal}}),
  });
  return run;
}

AutonomousRunSnapshot AutonomousRunManager::plan(const std::string& run_id) {
  auto snapshot = require_snapshot(run_id);
  if (!snapshot.steps.empty()) {
    return snapshot;
  }
  auto plan = planner_->plan(AutonomousPlanningInput{.run = snapshot.run});
  if (plan.steps.empty()) {
    throw AgentFrameworkError("Autonomous planner produced an empty plan.");
  }
  auto steps = store_->replace_steps(run_id, plan.steps);
  store_->append_checkpoint(AutonomousCheckpoint{.run_id = run_id, .name = "plan", .state = plan_to_value(plan)});
  store_->append_event(AutonomousEvent{
      .run_id = run_id,
      .type = "autonomous.run.planned",
      .payload = Value::object({{"summary", plan.summary}, {"stepCount", steps.size()}}),
  });
  return require_snapshot(run_id);
}

AutonomousRunSnapshot AutonomousRunManager::run(const std::string& run_id) {
  auto snapshot = prepare_run(run_id);
  std::size_t executed = 0;
  while (executed < max_steps_per_run_) {
    snapshot = require_snapshot(run_id);
    auto next = std::find_if(snapshot.steps.begin(), snapshot.steps.end(), [&](const auto& step) {
      return step.status == AutonomousStepStatus::Pending && dependencies_completed(step, snapshot.steps);
    });
    if (next == snapshot.steps.end()) {
      const bool waiting = std::any_of(snapshot.steps.begin(), snapshot.steps.end(), [](const auto& step) {
        return step.status == AutonomousStepStatus::Waiting;
      });
      if (waiting) {
        store_->update_run(run_id, AutonomousRunPatch{.status = AutonomousRunStatus::Waiting});
        return require_snapshot(run_id);
      }
      return complete_run(run_id, snapshot);
    }
    executed += 1;
    snapshot = execute_step(snapshot, *next);
    if (snapshot.run.status == AutonomousRunStatus::Waiting ||
        snapshot.run.status == AutonomousRunStatus::Failed ||
        snapshot.run.status == AutonomousRunStatus::Interrupted ||
        snapshot.run.status == AutonomousRunStatus::Cancelled) {
      return snapshot;
    }
  }
  throw AgentFrameworkError("Autonomous run exceeded maxStepsPerRun (" + std::to_string(max_steps_per_run_) + ").");
}

AutonomousRunSnapshot AutonomousRunManager::resume(const std::string& run_id, bool retry_failed_step) {
  auto snapshot = require_snapshot(run_id);
  if (snapshot.run.status == AutonomousRunStatus::Completed ||
      snapshot.run.status == AutonomousRunStatus::Cancelled) {
    return snapshot;
  }
  auto failed = std::find_if(snapshot.steps.begin(), snapshot.steps.end(), [](const auto& step) {
    return step.status == AutonomousStepStatus::Failed;
  });
  if (failed != snapshot.steps.end() && retry_failed_step) {
    store_->update_step(run_id, failed->id, AutonomousStepPatch{
        .status = AutonomousStepStatus::Pending,
        .has_error = true,
        .has_completed_at = true,
    });
    store_->append_event(AutonomousEvent{.run_id = run_id,
                                         .step_id = failed->id,
                                         .type = "autonomous.step.retry-scheduled"});
    snapshot = require_snapshot(run_id);
  }
  if (snapshot.run.status == AutonomousRunStatus::Waiting &&
      std::any_of(snapshot.steps.begin(), snapshot.steps.end(), [](const auto& step) {
        return step.status == AutonomousStepStatus::Waiting;
      })) {
    return snapshot;
  }
  return run(run_id);
}

AutonomousRunSnapshot AutonomousRunManager::complete_waiting_step(
    const std::string& run_id,
    const std::string& step_id,
    Value output) {
  const auto snapshot = require_snapshot(run_id);
  const auto found = std::find_if(snapshot.steps.begin(), snapshot.steps.end(), [&](const auto& step) {
    return step.id == step_id;
  });
  if (found == snapshot.steps.end()) {
    throw AgentFrameworkError("Autonomous step not found: " + step_id);
  }
  if (found->status != AutonomousStepStatus::Waiting) {
    throw AgentFrameworkError("Autonomous step is not waiting: " + step_id);
  }
  store_->update_step(run_id, step_id, AutonomousStepPatch{
      .status = AutonomousStepStatus::Completed,
      .output = output,
      .has_output = true,
      .has_wait_reason = true,
      .has_completed_at = true,
      .completed_at = now_iso8601(),
  });
  store_->append_event(AutonomousEvent{
      .run_id = run_id,
      .step_id = step_id,
      .type = "autonomous.step.completed",
      .payload = Value::object({{"output", output}, {"source", "external"}}),
  });
  store_->update_run(run_id, AutonomousRunPatch{.status = AutonomousRunStatus::Queued});
  return require_snapshot(run_id);
}

AutonomousRunSnapshot AutonomousRunManager::cancel(const std::string& run_id, std::string reason) {
  const auto snapshot = require_snapshot(run_id);
  const auto completed = now_iso8601();
  store_->update_run(run_id, AutonomousRunPatch{
      .status = AutonomousRunStatus::Cancelled,
      .error = reason,
      .has_error = true,
      .completed_at = completed,
      .has_completed_at = true,
  });
  for (const auto& step : snapshot.steps) {
    if (step.status == AutonomousStepStatus::Pending || step.status == AutonomousStepStatus::Running ||
        step.status == AutonomousStepStatus::Waiting) {
      store_->update_step(run_id, step.id, AutonomousStepPatch{
          .status = AutonomousStepStatus::Cancelled,
          .error = reason,
          .has_error = true,
          .completed_at = completed,
          .has_completed_at = true,
      });
    }
  }
  store_->append_event(AutonomousEvent{.run_id = run_id,
                                       .type = "autonomous.run.cancelled",
                                       .payload = Value::object({{"reason", reason}})});
  return require_snapshot(run_id);
}

AutonomousRunSnapshot AutonomousRunManager::require_snapshot(const std::string& run_id) {
  auto snapshot = store_->get_run(run_id);
  if (!snapshot) {
    throw AgentFrameworkError("Autonomous run not found: " + run_id);
  }
  return *snapshot;
}

AutonomousRunSnapshot AutonomousRunManager::prepare_run(const std::string& run_id) {
  auto snapshot = require_snapshot(run_id);
  if (snapshot.run.status == AutonomousRunStatus::Completed ||
      snapshot.run.status == AutonomousRunStatus::Cancelled) {
    return snapshot;
  }
  if (snapshot.steps.empty()) {
    snapshot = plan(run_id);
  }
  requeue_running_steps(snapshot);
  store_->update_run(run_id, AutonomousRunPatch{.status = AutonomousRunStatus::Running});
  store_->append_event(AutonomousEvent{.run_id = run_id, .type = "autonomous.run.started"});
  return require_snapshot(run_id);
}

void AutonomousRunManager::requeue_running_steps(const AutonomousRunSnapshot& snapshot) {
  for (const auto& step : snapshot.steps) {
    if (step.status != AutonomousStepStatus::Running) {
      continue;
    }
    store_->update_step(snapshot.run.id, step.id, AutonomousStepPatch{
        .status = AutonomousStepStatus::Pending,
        .error = "Step was interrupted before completion.",
        .has_error = true,
        .has_started_at = true,
    });
    store_->append_event(AutonomousEvent{.run_id = snapshot.run.id,
                                         .step_id = step.id,
                                         .type = "autonomous.step.interrupted"});
  }
}

AutonomousRunSnapshot AutonomousRunManager::execute_step(
    const AutonomousRunSnapshot& snapshot,
    const AutonomousStep& step) {
  const auto started = now_iso8601();
  auto running = store_->update_step(snapshot.run.id, step.id, AutonomousStepPatch{
      .status = AutonomousStepStatus::Running,
      .has_error = true,
      .has_wait_reason = true,
      .started_at = started,
      .has_started_at = true,
      .attempts = step.attempts + 1,
  });
  store_->append_event(AutonomousEvent{.run_id = snapshot.run.id,
                                       .step_id = step.id,
                                       .type = "autonomous.step.started",
                                       .payload = Value::object({{"attempt", running.attempts}})});
  store_->append_checkpoint(AutonomousCheckpoint{.run_id = snapshot.run.id,
                                                 .step_id = step.id,
                                                 .name = "step.started",
                                                 .state = autonomous_step_to_value(running)});
  try {
    auto result = executor_->execute(AutonomousStepExecutionInput{
        .run = snapshot.run,
        .step = running,
        .previous_steps = [&]() {
          std::vector<AutonomousStep> previous;
          std::copy_if(snapshot.steps.begin(), snapshot.steps.end(), std::back_inserter(previous),
                       [&](const auto& candidate) { return candidate.index < step.index; });
          return previous;
        }(),
        .checkpoint = [&](std::string name, Value state) {
          return store_->append_checkpoint(AutonomousCheckpoint{
              .run_id = snapshot.run.id,
              .step_id = step.id,
              .name = std::move(name),
              .state = std::move(state),
          });
        },
    });
    const auto completed = now_iso8601();
    if (result.status == AutonomousStepStatus::Waiting) {
      store_->update_step(snapshot.run.id, step.id, AutonomousStepPatch{
          .status = AutonomousStepStatus::Waiting,
          .output = result.output,
          .has_output = true,
          .wait_reason = result.wait_reason,
          .has_wait_reason = true,
          .metadata = merge_metadata(running.metadata, result.metadata),
          .has_metadata = true,
      });
      store_->update_run(snapshot.run.id, AutonomousRunPatch{.status = AutonomousRunStatus::Waiting});
      store_->append_event(AutonomousEvent{.run_id = snapshot.run.id,
                                           .step_id = step.id,
                                           .type = "autonomous.step.waiting",
                                           .payload = Value::object({{"reason", result.wait_reason}})});
      return require_snapshot(snapshot.run.id);
    }
    store_->update_step(snapshot.run.id, step.id, AutonomousStepPatch{
        .status = AutonomousStepStatus::Completed,
        .output = result.output,
        .has_output = true,
        .completed_at = completed,
        .has_completed_at = true,
        .metadata = merge_metadata(running.metadata, result.metadata),
        .has_metadata = true,
    });
    store_->append_checkpoint(AutonomousCheckpoint{.run_id = snapshot.run.id,
                                                   .step_id = step.id,
                                                   .name = "step.completed",
                                                   .state = result.output});
    store_->append_event(AutonomousEvent{.run_id = snapshot.run.id,
                                         .step_id = step.id,
                                         .type = "autonomous.step.completed",
                                         .payload = Value::object({{"output", result.output}})});
    if (!result.next_steps.empty()) {
      store_->append_steps(snapshot.run.id, result.next_steps);
    }
    return require_snapshot(snapshot.run.id);
  } catch (const std::exception& error) {
    const std::string message = error.what();
    store_->update_step(snapshot.run.id, step.id, AutonomousStepPatch{
        .status = AutonomousStepStatus::Failed,
        .error = message,
        .has_error = true,
        .completed_at = now_iso8601(),
        .has_completed_at = true,
    });
    store_->update_run(snapshot.run.id, AutonomousRunPatch{
        .status = AutonomousRunStatus::Failed,
        .error = message,
        .has_error = true,
        .completed_at = now_iso8601(),
        .has_completed_at = true,
    });
    store_->append_event(AutonomousEvent{.run_id = snapshot.run.id,
                                         .step_id = step.id,
                                         .type = "autonomous.run.failed",
                                         .payload = Value::object({{"error", message}})});
    return require_snapshot(snapshot.run.id);
  }
}

AutonomousRunSnapshot AutonomousRunManager::complete_run(
    const std::string& run_id,
    const AutonomousRunSnapshot& snapshot) {
  const auto completed = now_iso8601();
  const auto output = run_output_from_steps(snapshot.steps);
  store_->update_run(run_id, AutonomousRunPatch{
      .status = AutonomousRunStatus::Completed,
      .output = output,
      .has_output = true,
      .completed_at = completed,
      .has_completed_at = true,
      .updated_at = completed,
  });
  store_->append_checkpoint(AutonomousCheckpoint{.run_id = run_id,
                                                 .name = "run.completed",
                                                 .state = Value::object({{"output", output}})});
  store_->append_event(AutonomousEvent{.run_id = run_id,
                                       .type = "autonomous.run.completed",
                                       .payload = Value::object({{"output", output}})});
  return require_snapshot(run_id);
}

Value autonomous_run_to_value(const AutonomousRun& run) {
  return Value::object({{"id", run.id},
                        {"goal", run.goal},
                        {"input", run.input},
                        {"status", to_string(run.status)},
                        {"output", run.output},
                        {"error", run.error.empty() ? Value() : Value(run.error)},
                        {"createdAt", run.created_at},
                        {"updatedAt", run.updated_at},
                        {"completedAt", run.completed_at.empty() ? Value() : Value(run.completed_at)},
                        {"metadata", run.metadata}});
}

Value autonomous_step_to_value(const AutonomousStep& step) {
  return Value::object({{"id", step.id},
                        {"runId", step.run_id},
                        {"index", step.index},
                        {"title", step.title},
                        {"objective", step.objective},
                        {"input", step.input},
                        {"status", to_string(step.status)},
                        {"attempts", step.attempts},
                        {"output", step.output},
                        {"error", step.error.empty() ? Value() : Value(step.error)},
                        {"waitReason", step.wait_reason.empty() ? Value() : Value(step.wait_reason)},
                        {"dependsOn", string_array_to_value(step.depends_on)},
                        {"createdAt", step.created_at},
                        {"updatedAt", step.updated_at},
                        {"startedAt", step.started_at.empty() ? Value() : Value(step.started_at)},
                        {"completedAt", step.completed_at.empty() ? Value() : Value(step.completed_at)},
                        {"metadata", step.metadata}});
}

Value autonomous_event_to_value(const AutonomousEvent& event) {
  return Value::object({{"id", event.id},
                        {"runId", event.run_id},
                        {"stepId", event.step_id.empty() ? Value() : Value(event.step_id)},
                        {"type", event.type},
                        {"payload", event.payload},
                        {"createdAt", event.created_at}});
}

Value autonomous_checkpoint_to_value(const AutonomousCheckpoint& checkpoint) {
  return Value::object({{"id", checkpoint.id},
                        {"runId", checkpoint.run_id},
                        {"stepId", checkpoint.step_id.empty() ? Value() : Value(checkpoint.step_id)},
                        {"name", checkpoint.name},
                        {"state", checkpoint.state},
                        {"createdAt", checkpoint.created_at}});
}

Value autonomous_run_snapshot_to_value(const AutonomousRunSnapshot& snapshot) {
  Value::Array steps;
  for (const auto& step : snapshot.steps) {
    steps.push_back(autonomous_step_to_value(step));
  }
  Value::Array events;
  for (const auto& event : snapshot.events) {
    events.push_back(autonomous_event_to_value(event));
  }
  Value::Array checkpoints;
  for (const auto& checkpoint : snapshot.checkpoints) {
    checkpoints.push_back(autonomous_checkpoint_to_value(checkpoint));
  }
  return Value::object({{"run", autonomous_run_to_value(snapshot.run)},
                        {"steps", Value(steps)},
                        {"events", Value(events)},
                        {"checkpoints", Value(checkpoints)}});
}

AutonomousRun autonomous_run_from_value(const Value& value) {
  AutonomousRun run;
  run.id = value.at("id").as_string();
  run.goal = value.at("goal").as_string();
  run.input = value.at("input");
  run.status = autonomous_run_status_from_string(value.at("status").as_string());
  run.output = value.at("output");
  run.error = value.at("error").as_string();
  run.created_at = value.at("createdAt").as_string();
  run.updated_at = value.at("updatedAt").as_string();
  run.completed_at = value.at("completedAt").as_string();
  run.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return run;
}

AutonomousStep autonomous_step_from_value(const Value& value) {
  AutonomousStep step;
  step.id = value.at("id").as_string();
  step.run_id = value.at("runId").as_string();
  step.index = static_cast<std::size_t>(value.at("index").as_integer());
  step.title = value.at("title").as_string();
  step.objective = value.at("objective").as_string();
  step.input = value.at("input");
  step.status = autonomous_step_status_from_string(value.at("status").as_string());
  step.attempts = static_cast<int>(value.at("attempts").as_integer());
  step.output = value.at("output");
  step.error = value.at("error").as_string();
  step.wait_reason = value.at("waitReason").as_string();
  step.depends_on = string_array_from_value(value.at("dependsOn"));
  step.created_at = value.at("createdAt").as_string();
  step.updated_at = value.at("updatedAt").as_string();
  step.started_at = value.at("startedAt").as_string();
  step.completed_at = value.at("completedAt").as_string();
  step.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return step;
}

AutonomousEvent autonomous_event_from_value(const Value& value) {
  AutonomousEvent event;
  event.id = value.at("id").as_string();
  event.run_id = value.at("runId").as_string();
  event.step_id = value.at("stepId").as_string();
  event.type = value.at("type").as_string();
  event.payload = value.at("payload");
  event.created_at = value.at("createdAt").as_string();
  return event;
}

AutonomousCheckpoint autonomous_checkpoint_from_value(const Value& value) {
  AutonomousCheckpoint checkpoint;
  checkpoint.id = value.at("id").as_string();
  checkpoint.run_id = value.at("runId").as_string();
  checkpoint.step_id = value.at("stepId").as_string();
  checkpoint.name = value.at("name").as_string();
  checkpoint.state = value.at("state");
  checkpoint.created_at = value.at("createdAt").as_string();
  return checkpoint;
}

}  // namespace agent
