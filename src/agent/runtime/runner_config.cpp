#include "runner_config.hpp"

namespace agent {

AgentRunnerResolvedConfig normalize_agent_runner_config(const AgentRunnerConfig& input) {
  AgentRunnerResolvedConfig config;

  config.adapter = input.model_runtime.adapter;
  config.thinking_adapter = input.model_runtime.thinking_adapter;
  config.critique_adapter = input.model_runtime.critique_adapter;
  config.model_settings = input.model_runtime.settings;

  if (input.tool_runtime.definitions) {
    config.tools = *input.tool_runtime.definitions;
  }
  config.permission_policy = input.tool_runtime.permission_policy;
  config.approval_handler = input.tool_runtime.approval_handler;
  config.tool_services = input.tool_runtime.services;
  config.lazy_tool_mode = input.tool_runtime.lazy_mode.value_or(false);
  config.forced_visible_tools = input.tool_runtime.forced_visible_tools;
  config.tool_calling_strategy =
      input.tool_runtime.calling_strategy.value_or(AgentToolCallingStrategy::TextReAct);

  if (input.context_runtime.sources) {
    config.contexts = *input.context_runtime.sources;
  }
  if (input.context_runtime.system_prompt) {
    config.system_prompt = *input.context_runtime.system_prompt;
  }
  config.max_iterations = input.context_runtime.max_iterations.value_or(8);
  config.skills = input.context_runtime.skills;
  config.default_skills = input.context_runtime.default_skills;
  config.advertise_skills = input.context_runtime.advertise_skills.value_or(true);
  config.skill_model_conflict_policy =
      input.context_runtime.skill_model_conflict_policy.value_or(SkillConflictPolicy::Error);
  config.skill_effort_conflict_policy =
      input.context_runtime.skill_effort_conflict_policy.value_or(SkillConflictPolicy::Error);
  config.skill_fork_handler = input.context_runtime.skill_fork_handler;
  config.skill_subagents = input.context_runtime.skill_subagents;

  config.memory_store = input.memory_runtime.session_store;
  config.scratch_store = input.memory_runtime.scratch_store;
  config.long_term_memory = input.memory_runtime.long_term_memory;
  config.retrieval_options = input.memory_runtime.retrieval_options;
  config.writeback_options = input.memory_runtime.writeback_options;

  config.knowledge_provider = input.knowledge_runtime.provider;
  config.knowledge_retrieval_options = input.knowledge_runtime.retrieval_options;

  config.planner = input.governance.planner;
  config.enable_planning = input.governance.enable_planning.value_or(true);
  config.execution_policies = input.governance.execution_policies;
  if (input.governance.permission_policy) {
    config.permission_policy = input.governance.permission_policy;
  }
  if (input.governance.approval_handler) {
    config.approval_handler = input.governance.approval_handler;
  }

  config.event_bus = input.observability.event_bus;
  config.hooks = input.observability.hooks;
  config.prompt_stats_buckets = input.observability.prompt_stats_buckets;
  config.context_window_tokens = input.observability.context_window_tokens;
  config.context_token_counter = input.observability.context_token_counter;

  config.max_parse_errors = input.react_runtime.max_parse_errors.value_or(2);
  config.react_prompt_mode = input.react_runtime.prompt_mode.value_or(ReActPromptMode::Managed);
  if (input.react_runtime.parser_options) {
    config.react_parser_options = *input.react_runtime.parser_options;
  }
  config.react_prompt_builder = input.react_runtime.prompt_builder;
  config.persist_react_visible_messages =
      input.react_runtime.persist_visible_messages.value_or(false);

  return config;
}

}  // namespace agent
