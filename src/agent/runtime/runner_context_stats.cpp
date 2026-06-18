#include "internal.hpp"
#include "compaction_planner.hpp"
#include "memory_writeback.hpp"
#include "runner_kernel.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace agent {

ContextStatsSnapshot RunnerContextStats::estimate(const std::string& input,
                                                  const std::string& session_id,
                                                  ContextStatsOptions options) const {
  return runner_->estimate_context_stats_input(create_message(MessageRole::User, input), Value(input), session_id,
                                               std::move(options));
}

ContextStatsSnapshot RunnerContextStats::estimate(std::vector<MessageContentPart> input_parts,
                                                  const std::string& session_id,
                                                  ContextStatsOptions options) const {
  const std::string input_text = extract_text_content(input_parts);
  return runner_->estimate_context_stats_input(user_input_message(std::move(input_parts)), Value(input_text), session_id,
                                               std::move(options));
}

ContextStatsSnapshot RunnerContextStats::estimate(AgentMessage input_message,
                                                  const std::string& session_id,
                                                  ContextStatsOptions options) const {
  const Value input_value = agent_message_to_value(input_message);
  return runner_->estimate_context_stats_input(std::move(input_message), input_value, session_id, std::move(options));
}

std::optional<ContextStatsSnapshot> RunnerContextStats::last() const {
  return runner_->last_context_stats();
}

ContextStatsSnapshot AgentRunner::estimate_context_stats_input(AgentMessage input_message,
                                                               Value input_value,
                                                               const std::string& session_id,
                                                               ContextStatsOptions options) const {
  EventBus* event_bus = this->event_bus();
  auto invocation = resolve_runner_invocation(*kernel_->config, std::move(input_message), std::move(input_value),
                                              options.allow_skill_input_rewrite, session_id,
                                              options.model_settings, std::move(options.skill_activations),
                                              options.context, event_bus, nullptr, "agent.context.estimate");
  auto session = kernel_->memory_store->get(invocation.session_id);
  ContextStatsAssembly assembly;
  assembly.session_id = invocation.session_id;
  assembly.context_window_tokens =
      options.context_window_tokens ? options.context_window_tokens : kernel_->config->context_window_tokens;
  if (context_token_counter_configured(options.token_counter)) {
    assembly.counter = std::move(options.token_counter);
  } else if (context_token_counter_configured(kernel_->config->context_token_counter)) {
    assembly.counter = kernel_->config->context_token_counter;
  } else if (auto session_counter = session->token_counter()) {
    assembly.counter = context_token_counter_from_session_counter(std::move(session_counter));
  }

  auto append_text_bucket = [&](std::string id,
                                std::string label,
                                ContextStatsBucketKind kind,
                                std::string text,
                                Value metadata = Value::object({})) {
    if (text.empty()) {
      return;
    }
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .text = std::move(text),
        .metadata = std::move(metadata),
    });
  };

  auto append_message_bucket = [&](std::string id,
                                   std::string label,
                                   ContextStatsBucketKind kind,
                                   AgentMessage message,
                                   Value metadata = Value::object({})) {
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .messages = {std::move(message)},
        .metadata = std::move(metadata),
    });
  };

  auto append_messages_bucket = [&](std::string id,
                                    std::string label,
                                    ContextStatsBucketKind kind,
                                    std::vector<AgentMessage> messages,
                                    Value metadata = Value::object({})) {
    if (messages.empty()) {
      return;
    }
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .messages = std::move(messages),
        .metadata = std::move(metadata),
    });
  };

  auto append_tools_bucket = [&](std::string id,
                                 std::string label,
                                 ContextStatsBucketKind kind,
                                 std::vector<ChatToolDescriptor> tools,
                                 Value metadata = Value::object({})) {
    if (tools.empty()) {
      return;
    }
    assembly.buckets.push_back(ContextStatsBucketInput{
        .id = std::move(id),
        .label = std::move(label),
        .kind = kind,
        .tools = std::move(tools),
        .metadata = std::move(metadata),
    });
  };

  auto append_stats_bucket = [&](ContextStatsBucketInput bucket) {
    if (bucket.text.empty() && bucket.messages.empty() && bucket.tools.empty()) {
      return;
    }
    assembly.buckets.push_back(std::move(bucket));
  };

  auto append_tool_definition_buckets = [&](std::string source, bool include_empty_text_bucket) {
    std::vector<ChatToolDescriptor> regular_tools;
    std::vector<ChatToolDescriptor> mcp_tools;
    for (const auto& tool : kernel_->tool_registry.list()) {
      const bool is_mcp = tool.bundle == "mcp"
                          || std::find(tool.tags.begin(), tool.tags.end(), "mcp") != tool.tags.end();
      if (is_mcp) {
        mcp_tools.push_back(tool.descriptor());
      } else {
        regular_tools.push_back(tool.descriptor());
      }
    }
    if (regular_tools.empty() && mcp_tools.empty()) {
      if (include_empty_text_bucket) {
        append_text_bucket("tool_definitions", "Tool Definitions", ContextStatsBucketKind::ToolDefinitions,
                           "- No tools available.",
                           Value::object({{"derived", true}, {"source", source}, {"toolCount", 0}}));
      }
      return;
    }
    append_tools_bucket("tool_definitions", "Tool Definitions", ContextStatsBucketKind::ToolDefinitions,
                        std::move(regular_tools),
                        Value::object({{"derived", true}, {"source", source}}));
    append_tools_bucket("mcp_tools", "MCP Tools", ContextStatsBucketKind::Mcp,
                        std::move(mcp_tools),
                        Value::object({{"derived", true}, {"source", source}}));
  };

  if (kernel_->config->tool_calling_strategy == AgentToolCallingStrategy::NativeToolCalling) {
    if (!kernel_->config->prompt_stats_buckets.empty()) {
      for (auto bucket : kernel_->config->prompt_stats_buckets) {
        append_stats_bucket(std::move(bucket));
      }
    } else if (!kernel_->config->system_prompt.empty()) {
      append_message_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                            create_message(MessageRole::System, kernel_->config->system_prompt,
                                           Value::object({{"source", "runner"}})),
                            Value::object({{"source", "runner"}}));
    }
    append_tool_definition_buckets("native-tool-calling", false);
  } else if (kernel_->config->react_prompt_mode == ReActPromptMode::Managed) {
    append_text_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                       kernel_->config->system_prompt,
                       Value::object({{"derived", true}, {"source", "react-managed"}}));
    append_text_bucket("rules", "ReAct Rules", ContextStatsBucketKind::Rules,
                       render_managed_react_rules_text(),
                       Value::object({{"derived", true}, {"source", "react-managed"}}));
    append_tool_definition_buckets("react-managed", true);
  } else if (!kernel_->config->prompt_stats_buckets.empty()) {
    for (auto bucket : kernel_->config->prompt_stats_buckets) {
      append_stats_bucket(std::move(bucket));
    }
  } else if (kernel_->config->react_prompt_mode == ReActPromptMode::Custom && kernel_->config->react_prompt_builder) {
    append_message_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                          kernel_->config->react_prompt_builder->build_system_message(
                              ReActPromptBuilderInput{kernel_->config->system_prompt, kernel_->tool_registry.descriptors()}),
                          Value::object({{"source", "react-custom"}}));
  } else if (!kernel_->config->system_prompt.empty()) {
    append_message_bucket("system_prompt", "System Prompt", ContextStatsBucketKind::SystemPrompt,
                          create_message(MessageRole::System, kernel_->config->system_prompt,
                                         Value::object({{"source", "runner"}})),
                          Value::object({{"source", "runner"}}));
  }

  if (kernel_->config->advertise_skills && invocation.skill_state.available_message) {
    append_message_bucket("skills.catalog", "Skills", ContextStatsBucketKind::Skills,
                          *invocation.skill_state.available_message,
                          Value::object({{"source", "skills-catalog"}}));
  }

  for (std::size_t index = 0; index < invocation.skill_state.active_skills.size(); ++index) {
    const auto& entry = invocation.skill_state.active_skills[index];
    if (skill_requests_fork(entry)) {
      append_message_bucket("subagents." + std::to_string(index), "Subagents",
                            ContextStatsBucketKind::Subagents,
                            create_message(MessageRole::System, entry.rendered_prompt,
                                           Value::object({{"source", "skill"},
                                                          {"skill", entry.skill.manifest.name},
                                                          {"agent", entry.skill.manifest.agent},
                                                          {"estimated", true}})),
                            Value::object({{"source", "skill"},
                                           {"estimated", true},
                                           {"agent", entry.skill.manifest.agent}}));
    } else {
      append_message_bucket("skills." + std::to_string(index), "Skills", ContextStatsBucketKind::Skills,
                            skill_active_message(entry),
                            Value::object({{"source", "skill"}}));
    }
  }

  Value loop_context =
      runner_loop_context(invocation.run_hook_context_value, std::nullopt, {}, {}, Value::object({}));
  if (!loop_context.is_object()) {
    loop_context = Value::object({});
  }
  loop_context["input"] = invocation.effective_input_value.is_null() ? Value(invocation.effective_input)
                                                                     : invocation.effective_input_value;
  loop_context["iteration"] = 0;
  const auto context_assembly = kernel_->context_manager.build_assembly(loop_context);
  for (auto bucket : context_assembly.stats_buckets) {
    append_stats_bucket(std::move(bucket));
  }

  auto conversation_messages = session->get_messages();
  if (!invocation.effective_input_message.content.empty()) {
    conversation_messages.push_back(invocation.effective_input_message);
  }
  append_messages_bucket("conversation", "Conversation", ContextStatsBucketKind::Conversation,
                         std::move(conversation_messages),
                         Value::object({{"source", "session-memory"}, {"estimated", true}}));

  return agent::estimate_context_stats(assembly);
}

}  // namespace agent
