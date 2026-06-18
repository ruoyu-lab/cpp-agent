#include "agent/context.hpp"
#include "agent/tools.hpp"
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

Value context_stats_metadata(Value metadata,
                             const std::string& source_id,
                             const std::string& source_title,
                             const EmbeddedContextBlock& block,
                             bool derived = false) {
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  metadata["source"] = "embedded-context";
  if (!source_id.empty()) {
    metadata["sourceId"] = source_id;
  }
  if (!source_title.empty()) {
    metadata["sourceTitle"] = source_title;
  }
  if (!block.title.empty()) {
    metadata["blockTitle"] = block.title;
  }
  if (derived) {
    metadata["derived"] = true;
  }
  return metadata;
}

std::string render_embedded_context_block(const EmbeddedContextBlock& block) {
  return "\n\n### " + block.title + "\n" + block.content;
}

Value messages_to_value(const std::vector<AgentMessage>& messages) {
  Value::Array values;
  values.reserve(messages.size());
  for (const auto& message : messages) {
    values.push_back(agent_message_to_value(message));
  }
  return Value(std::move(values));
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

std::string to_string(PromptAssemblySegmentKind kind) {
  switch (kind) {
    case PromptAssemblySegmentKind::System:
      return "system";
    case PromptAssemblySegmentKind::Preface:
      return "preface";
    case PromptAssemblySegmentKind::EmbeddedContext:
      return "embedded-context";
    case PromptAssemblySegmentKind::Conversation:
      return "conversation";
  }
  return "conversation";
}

Value prompt_assembly_segment_to_value(const PromptAssemblySegment& segment) {
  return Value::object({
      {"id", segment.id},
      {"label", segment.label},
      {"kind", to_string(segment.kind)},
      {"messages", messages_to_value(segment.messages)},
      {"metadata", segment.metadata},
  });
}

Value prompt_assembly_to_value(const PromptAssembly& assembly) {
  Value::Array segments;
  segments.reserve(assembly.segments.size());
  for (const auto& segment : assembly.segments) {
    segments.push_back(prompt_assembly_segment_to_value(segment));
  }
  return Value::object({
      {"version", assembly.version},
      {"sessionId", assembly.session_id},
      {"iteration", assembly.iteration},
      {"segments", Value(std::move(segments))},
      {"messages", messages_to_value(assembly.messages)},
      {"metadata", assembly.metadata},
  });
}

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
    const bool multiple_blocks = resolved.size() > 1;
    std::size_t block_index = 0;
    for (auto& block : resolved) {
      if (block.title.empty()) {
        block.title = source.title;
      }
      if (block.priority == 0) {
        block.priority = source.priority;
      }
      if (!block.stats_kind) {
        block.stats_kind = ContextStatsBucketKind::Context;
      }
      if (block.stats_id.empty()) {
        block.stats_id = "context." + source.id;
        if (multiple_blocks) {
          block.stats_id += "." + std::to_string(block_index);
        }
      }
      if (block.stats_label.empty()) {
        block.stats_label = block.title.empty() ? source.title : block.title;
      }
      block.stats_metadata = context_stats_metadata(std::move(block.stats_metadata),
                                                    source.id,
                                                    source.title,
                                                    block);
      if (!block.content.empty()) {
        blocks.push_back(std::move(block));
      }
      ++block_index;
    }
  }
  std::sort(blocks.begin(), blocks.end(), [](const auto& left, const auto& right) {
    return left.priority > right.priority;
  });
  return blocks;
}

EmbeddedContextAssembly EmbeddedContextManager::build_assembly(const Value& runtime) const {
  EmbeddedContextAssembly assembly;
  assembly.blocks = resolve_blocks(runtime);
  if (assembly.blocks.empty()) {
    return assembly;
  }
  constexpr std::string_view header = "Embedded context";
  assembly.stats_buckets.push_back(ContextStatsBucketInput{
      .id = "context.preamble",
      .label = "Runtime Context Preamble",
      .kind = ContextStatsBucketKind::Context,
      .text = std::string(header),
      .metadata = Value::object({{"source", "embedded-context"}, {"derived", true}}),
  });

  std::string content(header);
  Value::Array block_titles;
  Value::Array block_stats;
  for (const auto& block : assembly.blocks) {
    const std::string rendered = render_embedded_context_block(block);
    content += rendered;
    block_titles.emplace_back(block.title);
    block_stats.push_back(Value::object({
        {"id", block.stats_id},
        {"label", block.stats_label},
        {"kind", to_string(block.stats_kind.value_or(ContextStatsBucketKind::Context))},
    }));
    assembly.stats_buckets.push_back(ContextStatsBucketInput{
        .id = block.stats_id,
        .label = block.stats_label.empty() ? block.title : block.stats_label,
        .kind = block.stats_kind.value_or(ContextStatsBucketKind::Context),
        .text = rendered,
        .metadata = block.stats_metadata,
    });
  }
  assembly.message = create_message(MessageRole::System, content,
                                    Value::object({{"source", "embedded-context"},
                                                   {"blocks", Value(block_titles)},
                                                   {"statsBlocks", Value(block_stats)}}));
  return assembly;
}

std::optional<AgentMessage> EmbeddedContextManager::build_message(const Value& runtime) const {
  const auto assembly = build_assembly(runtime);
  if (assembly.message) {
    return assembly.message;
  }
  return std::nullopt;
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
