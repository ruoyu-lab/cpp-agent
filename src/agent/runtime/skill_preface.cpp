#include "internal.hpp"

#include <utility>

namespace agent {

bool skill_requests_fork(const ResolvedSkillUse& entry) {
  return entry.skill.manifest.context == "fork" || !entry.skill.manifest.agent.empty();
}

AgentMessage skill_active_message(const ResolvedSkillUse& entry) {
  return create_message(MessageRole::System, entry.rendered_prompt,
                        Value::object({{"source", "skill"},
                                       {"skill", entry.skill.manifest.name},
                                       {"argumentsText", entry.arguments_text}}));
}

std::string skill_fork_child_input(const ResolvedSkillUse& entry, const std::string& input_text) {
  std::string child_input = entry.rendered_prompt;
  if (!trim_copy(input_text).empty()) {
    child_input += "\n\nUser task:\n" + input_text;
  }
  return child_input;
}

std::string skill_fork_session_id(const std::string& session_id, const ResolvedSkillUse& entry,
                                  const std::string& agent_name) {
  std::string fork_session = session_id.empty() ? std::string("default") : session_id;
  fork_session += ":skill:" + entry.skill.manifest.name;
  if (!agent_name.empty()) {
    fork_session += ":" + agent_name;
  }
  return fork_session;
}

std::optional<SkillForkExecution> execute_skill_fork(AgentRunner* owner,
                                                     const AgentRunnerResolvedConfig& config,
                                                     const ResolvedSkillUse& entry,
                                                     const std::string& input_text,
                                                     const std::string& session_id,
                                                     const ModelSettings& model_settings,
                                                     const Value& context) {
  const std::string agent_name = entry.skill.manifest.agent;
  const auto child_session_id = skill_fork_session_id(session_id, entry, agent_name);
  SkillForkRequest request{
      .skill = entry,
      .input_text = input_text,
      .session_id = child_session_id,
      .model_settings = model_settings,
      .context = context,
  };

  if (!agent_name.empty()) {
    const auto found = config.skill_subagents.find(agent_name);
    if (found != config.skill_subagents.end() && found->second) {
      if (found->second == owner) {
        throw ConfigurationError("Skill \"" + entry.skill.manifest.name
                                 + "\" cannot fork into the owning AgentRunner.");
      }
      auto child_result = found->second->execution().run(skill_fork_child_input(entry, input_text),
                                                         child_session_id,
                                                         model_settings, {}, {}, {}, context);
      return SkillForkExecution{
          .use = entry,
          .result = SkillForkResult{
              .text = child_result.text,
              .metadata = Value::object({{"agent", agent_name},
                                         {"sessionId", child_result.session_id},
                                         {"iterations", static_cast<double>(child_result.iteration_count)}}),
          },
          .agent = agent_name,
          .session_id = child_result.session_id,
      };
    }
  }

  if (config.skill_fork_handler) {
    auto result = config.skill_fork_handler(request);
    if (result) {
      return SkillForkExecution{
          .use = entry,
          .result = std::move(*result),
          .agent = agent_name,
          .session_id = child_session_id,
      };
    }
  }

  return std::nullopt;
}

AgentMessage skill_fork_result_message(const SkillForkExecution& execution) {
  std::string text = "Forked skill \"" + execution.use.skill.manifest.name + "\" result";
  if (!execution.agent.empty()) {
    text += " from agent \"" + execution.agent + "\"";
  }
  text += ":\n" + execution.result.text;

  return create_message(MessageRole::System, text,
                        Value::object({{"source", "skill-fork-result"},
                                       {"skill", execution.use.skill.manifest.name},
                                       {"agent", execution.agent},
                                       {"sessionId", execution.session_id},
                                       {"argumentsText", execution.use.arguments_text},
                                       {"result", execution.result.metadata}}));
}

SkillPrefaceBuildResult build_skill_preface_messages(AgentRunner* owner,
                                                     const AgentRunnerResolvedConfig& config,
                                                     const ResolvedSkillsState& state,
                                                     const std::string& input_text,
                                                     const std::string& session_id,
                                                     const Value& context) {
  SkillPrefaceBuildResult result;
  result.messages.reserve(state.active_skills.size());
  for (const auto& entry : state.active_skills) {
    if (skill_requests_fork(entry)) {
      auto fork_execution = execute_skill_fork(owner, config, entry, input_text, session_id,
                                               state.model_settings, context);
      if (fork_execution) {
        result.messages.push_back(skill_fork_result_message(*fork_execution));
        result.fork_executions.push_back(std::move(*fork_execution));
        continue;
      }
    }
    result.messages.push_back(skill_active_message(entry));
  }
  return result;
}

Value skill_services_value(const ResolvedSkillsState& state,
                           const std::vector<SkillForkExecution>& fork_executions) {
  Value::Array active;
  active.reserve(state.active_skills.size());
  std::vector<std::string> names;
  names.reserve(state.active_skills.size());
  for (const auto& entry : state.active_skills) {
    names.push_back(entry.skill.manifest.name);
    active.emplace_back(Value::object({
        {"name", entry.skill.manifest.name},
        {"description", entry.skill.manifest.description},
        {"argumentsText", entry.arguments_text},
    }));
  }
  Value::Array fork_results;
  fork_results.reserve(fork_executions.size());
  for (const auto& execution : fork_executions) {
    fork_results.emplace_back(Value::object({
        {"name", execution.use.skill.manifest.name},
        {"agent", execution.agent},
        {"sessionId", execution.session_id},
        {"text", execution.result.text},
        {"metadata", execution.result.metadata},
    }));
  }
  return Value::object({
      {"names", string_array_value(names)},
      {"autoSelected", string_array_value(state.auto_selected_skills)},
      {"allowedTools", string_array_value(state.allowed_tools)},
      {"active", Value(std::move(active))},
      {"forkResults", Value(std::move(fork_results))},
  });
}

ToolExecutionServices runner_runtime_services(SessionMemory* session,
                                               const std::string& session_id,
                                               LongTermMemoryPort* long_term_memory,
                                               KnowledgeContextProvider* knowledge_provider,
                                               const Value& context,
                                               const std::optional<Value>& skills) {
  ToolExecutionServices services;
  services.service_container.set(kToolServiceSessionMemory, session);
  services.service_container.set(kToolServiceLongTermMemory, long_term_memory);
  services.service_container.set(kToolServiceKnowledgeProvider, knowledge_provider);
  services.values = Value::object({
      {"sessionId", session_id},
      {"session", Value::object({
                      {"sessionId", session_id},
                      {"available", session != nullptr},
                  })},
  });
  if (long_term_memory || knowledge_provider) {
    services.values["knowledge"] = Value::object({
        {"longTermMemoryAvailable", long_term_memory != nullptr},
        {"knowledgeProviderAvailable", knowledge_provider != nullptr},
    });
  }
  if (context.is_object() && !context.as_object().empty()) {
    services.values["context"] = context;
  }
  if (skills) {
    services.values["skills"] = *skills;
  }
  return services;
}

}  // namespace agent
