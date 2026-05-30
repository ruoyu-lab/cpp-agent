#include "agent/agent.hpp"
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

std::string plan_string_field(const Value& value, const std::string& primary,
                              const std::string& secondary = {}) {
  std::string result = value.at(primary).as_string();
  if (result.empty() && !secondary.empty()) {
    result = value.at(secondary).as_string();
  }
  return result;
}

std::vector<std::string> plan_string_array(const Value& value) {
  std::vector<std::string> result;
  if (value.is_string()) {
    const auto text = value.as_string();
    if (!text.empty()) {
      result.push_back(text);
    }
    return result;
  }
  if (!value.is_array()) {
    return result;
  }
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      result.push_back(text);
    }
  }
  return result;
}

Value string_vector_value(const std::vector<std::string>& values) {
  Value::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return Value(std::move(array));
}

std::string truncate_plan_text(const std::string& value, std::size_t max_size = 700) {
  if (value.size() <= max_size) {
    return value;
  }
  return value.substr(0, max_size) + "...";
}

std::optional<std::string> extract_json_object_text(const std::string& text) {
  const auto trimmed = trim_copy(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  const auto fence = trimmed.find("```");
  if (fence != std::string::npos) {
    auto start = fence + 3;
    const auto line_end = trimmed.find('\n', start);
    if (line_end != std::string::npos) {
      const auto label = trim_copy(trimmed.substr(start, line_end - start));
      start = line_end + 1;
      const auto end = trimmed.find("```", start);
      if (end != std::string::npos && (label.empty() || label == "json" || label == "JSON")) {
        return trimmed.substr(start, end - start);
      }
    }
  }

  const auto start = trimmed.find('{');
  const auto end = trimmed.rfind('}');
  if (start == std::string::npos || end == std::string::npos || end < start) {
    return std::nullopt;
  }
  return trimmed.substr(start, end - start + 1);
}

std::string build_model_planner_prompt(const PlannerParams& params, std::size_t max_steps) {
  std::ostringstream out;
  out << "Create a concise execution plan for the task.\n\n";
  out << "Task:\n" << params.input << "\n\n";

  out << "Available tools:\n";
  if (params.tools) {
    const auto tools = params.tools->list();
    if (tools.empty()) {
      out << "- none\n";
    } else {
      for (const auto& tool : tools) {
        out << "- " << tool.name;
        if (!tool.description.empty()) {
          out << ": " << tool.description;
        }
        out << "\n";
      }
    }
  } else {
    out << "- none\n";
  }

  if (!params.memory_hits.empty()) {
    out << "\nRelevant memory:\n";
    for (std::size_t index = 0; index < params.memory_hits.size(); ++index) {
      const auto& hit = params.memory_hits[index];
      out << index + 1 << ". " << truncate_plan_text(hit.content) << " (score=" << hit.score << ")\n";
    }
  }

  if (!params.knowledge_hits.empty()) {
    out << "\nKnowledge hits:\n";
    for (std::size_t index = 0; index < params.knowledge_hits.size(); ++index) {
      const auto& hit = params.knowledge_hits[index];
      out << index + 1 << ". [" << hit.citation.title << "] "
          << truncate_plan_text(hit.citation.snippet.empty() ? hit.chunk.content : hit.citation.snippet)
          << " (score=" << hit.score << ")\n";
    }
  }

  if (params.session) {
    const auto snapshot = params.session->snapshot();
    if (!snapshot.summary.empty()) {
      out << "\nSession summary:\n" << truncate_plan_text(snapshot.summary) << "\n";
    }
  }

  out << "\nReturn JSON only in this shape:\n";
  out << "{\"goal\":\"...\",\"steps\":[{\"id\":\"step_1\",\"title\":\"...\","
         "\"description\":\"...\",\"toolName\":\"optional\",\"dependsOn\":[]}],\"notes\":[\"...\"]}\n";
  out << "Use at most " << std::max<std::size_t>(1, max_steps)
      << " steps. If no useful plan is needed, return {\"steps\":[]}.";
  return out.str();
}

}  // namespace

EmbeddedContextManager::EmbeddedContextManager(std::vector<ContextSource> sources) {
  for (auto& source : sources) {
    register_source(std::move(source));
  }
}

EmbeddedContextManager::EmbeddedContextManager(const EmbeddedContextManager& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  sources_ = other.sources_;
}

EmbeddedContextManager& EmbeddedContextManager::operator=(const EmbeddedContextManager& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  sources_ = other.sources_;
  return *this;
}

EmbeddedContextManager::EmbeddedContextManager(EmbeddedContextManager&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  sources_ = std::move(other.sources_);
}

EmbeddedContextManager& EmbeddedContextManager::operator=(EmbeddedContextManager&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  sources_ = std::move(other.sources_);
  return *this;
}

ContextSource& EmbeddedContextManager::register_source(ContextSource source) {
  if (source.id.empty()) {
    throw ConfigurationError("Context source id is required.");
  }
  if (source.title.empty()) {
    source.title = source.id;
  }
  if (!source.resolve) {
    throw ConfigurationError("Context source \"" + source.id + "\" requires a resolver.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  sources_.push_back(std::move(source));
  std::sort(sources_.begin(), sources_.end(), [](const auto& left, const auto& right) {
    return left.priority > right.priority;
  });
  return sources_.front();
}

std::vector<EmbeddedContextBlock> EmbeddedContextManager::resolve_blocks(const Value& runtime) const {
  std::vector<ContextSource> sources;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sources = sources_;
  }
  std::vector<EmbeddedContextBlock> blocks;
  for (const auto& source : sources) {
    auto resolved = source.resolve(runtime);
    for (auto& block : resolved) {
      if (block.title.empty()) {
        block.title = source.title;
      }
      if (block.priority == 0) {
        block.priority = source.priority;
      }
      if (!block.content.empty()) {
        blocks.push_back(std::move(block));
      }
    }
  }
  std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) {
    return left.priority > right.priority;
  });
  return blocks;
}

std::optional<AgentMessage> EmbeddedContextManager::build_message(const Value& runtime) const {
  const auto blocks = resolve_blocks(runtime);
  if (blocks.empty()) {
    return std::nullopt;
  }
  std::string content = "Embedded context";
  Value::Array block_ids;
  for (const auto& block : blocks) {
    content += "\n\n### " + block.title + "\n" + block.content;
    block_ids.emplace_back(block.title);
  }
  return create_message(MessageRole::System, content,
                        Value::object({{"source", "embedded-context"}, {"blocks", Value(block_ids)}}));
}

StaticPlanner::StaticPlanner(ExecutionPlan plan) : plan_(std::move(plan)) {}

StaticPlanner::StaticPlanner(PlannerHandler handler) : handler_(std::move(handler)) {}

std::optional<ExecutionPlan> StaticPlanner::plan(const PlannerParams& params) {
  if (params.cancellation) {
    params.cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  if (handler_) {
    auto generated = handler_(params);
    if (params.cancellation) {
      params.cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    if (generated && generated->steps.empty()) {
      return std::nullopt;
    }
    return generated;
  }
  if (!plan_ || plan_->steps.empty()) {
    return std::nullopt;
  }
  return plan_;
}

ModelPlanner::ModelPlanner(ModelPlannerConfig config) : config_(std::move(config)) {
  if (!config_.model) {
    throw ConfigurationError("ModelPlanner requires a model adapter.");
  }
}

std::optional<ExecutionPlan> ModelPlanner::plan(const PlannerParams& params) {
  if (params.cancellation) {
    params.cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  const auto response = config_.model->generate(GenerateParams{
      .messages = {
          create_message(MessageRole::System, config_.system_prompt,
                         Value::object({{"source", "planner"}})),
          create_message(MessageRole::User, build_model_planner_prompt(params, config_.max_steps),
                         Value::object({{"source", "planner"}})),
      },
      .tools = {},
      .settings = config_.model->resolve_settings(config_.model_settings),
      .cancellation = params.cancellation,
  });
  if (params.cancellation) {
    params.cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
  const auto text = response.text.empty() ? extract_text_content(response.content) : response.text;
  return parse_execution_plan_text(text);
}

std::optional<ExecutionPlan> normalize_execution_plan(const Value& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }

  const auto& raw_steps = value.at("steps");
  if (!raw_steps.is_array()) {
    return std::nullopt;
  }

  ExecutionPlan plan;
  plan.goal = plan_string_field(value, "goal");
  if (plan.goal.empty()) {
    plan.goal = "Execute the task safely and efficiently.";
  }
  plan.notes = plan_string_array(value.at("notes"));
  plan.updated_at = plan_string_field(value, "updatedAt", "updated_at");
  if (plan.updated_at.empty()) {
    plan.updated_at = now_iso8601();
  }

  for (std::size_t index = 0; index < raw_steps.as_array().size(); ++index) {
    const auto& item = raw_steps.as_array()[index];
    PlanStep step;
    if (item.is_string()) {
      step.title = item.as_string();
    } else if (item.is_object()) {
      step.id = plan_string_field(item, "id");
      step.title = plan_string_field(item, "title");
      step.description = plan_string_field(item, "description");
      step.tool_name = plan_string_field(item, "toolName", "tool_name");
      step.depends_on = plan_string_array(item.at("dependsOn").is_null() ? item.at("depends_on")
                                                                         : item.at("dependsOn"));
    } else {
      continue;
    }

    if (step.id.empty()) {
      step.id = "step_" + std::to_string(plan.steps.size() + 1);
    }
    if (step.title.empty()) {
      step.title = "Step " + std::to_string(plan.steps.size() + 1);
    }
    plan.steps.push_back(std::move(step));
  }

  if (plan.steps.empty()) {
    return std::nullopt;
  }
  return plan;
}

Value execution_plan_to_value(const ExecutionPlan& plan) {
  Value::Array steps;
  steps.reserve(plan.steps.size());
  for (const auto& step : plan.steps) {
    steps.push_back(Value::object({
        {"id", step.id},
        {"title", step.title},
        {"description", step.description},
        {"toolName", step.tool_name},
        {"dependsOn", string_vector_value(step.depends_on)},
    }));
  }
  return Value::object({
      {"goal", plan.goal},
      {"steps", Value(std::move(steps))},
      {"notes", string_vector_value(plan.notes)},
      {"updatedAt", plan.updated_at},
  });
}

std::optional<ExecutionPlan> parse_execution_plan_text(const std::string& text) {
  const auto candidate = extract_json_object_text(text);
  if (!candidate) {
    return std::nullopt;
  }
  try {
    return normalize_execution_plan(parse_json(*candidate));
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string render_execution_plan(const ExecutionPlan& plan) {
  std::ostringstream out;
  out << "Execution plan\nGoal: " << plan.goal << "\n\nSteps:";
  for (std::size_t index = 0; index < plan.steps.size(); ++index) {
    const auto& step = plan.steps[index];
    out << "\n" << index + 1 << ". " << step.title;
    if (!step.description.empty()) {
      out << "\n   Description: " << step.description;
    }
    if (!step.tool_name.empty()) {
      out << "\n   Tool: " << step.tool_name;
    }
    if (!step.depends_on.empty()) {
      out << "\n   Depends on: ";
      for (std::size_t dep = 0; dep < step.depends_on.size(); ++dep) {
        if (dep > 0) {
          out << ", ";
        }
        out << step.depends_on[dep];
      }
    }
  }
  if (!plan.notes.empty()) {
    out << "\n\nNotes:";
    for (std::size_t index = 0; index < plan.notes.size(); ++index) {
      out << "\n" << index + 1 << ". " << plan.notes[index];
    }
  }
  return out.str();
}

AgentMessage create_plan_message(const ExecutionPlan& plan) {
  return create_message(MessageRole::System, render_execution_plan(plan),
                        Value::object({{"source", "planner"}, {"planGoal", plan.goal},
                                       {"planSteps", plan.steps.size()}}));
}
}  // namespace agent
