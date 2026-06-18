#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace agent {

AgentMessage user_input_message(std::vector<MessageContentPart> input_parts) {
  return create_message(MessageRole::User, std::move(input_parts));
}

bool value_has_fields(const Value& value) {
  return !value.is_object() || !value.as_object().empty();
}

bool has_durable_input_message(const AgentMessage& message) {
  return message.role != MessageRole::User || !message.content.empty() || !message.name.empty()
         || !message.tool_call_id.empty() || !message.tool_calls.empty() || value_has_fields(message.metadata);
}

AgentMessage input_message_from_durable_state(const AgentLoopDurableState& state) {
  if (has_durable_input_message(state.input_message)) {
    return state.input_message;
  }
  if (!state.input_parts.empty()) {
    return user_input_message(state.input_parts);
  }
  if (!state.input_text.empty()) {
    return create_message(MessageRole::User, state.input_text);
  }
  return {};
}

Value input_value_for_effective_input(const Value& input_value,
                                      const std::string& effective_input) {
  return input_value.is_string() ? Value(effective_input) : input_value;
}

std::vector<MessageContentPart> loop_input_parts_for_effective_input(
    const std::vector<MessageContentPart>& input_parts,
    const std::string& input_text,
    const std::string& effective_input) {
  if (effective_input != input_text) {
    std::vector<MessageContentPart> rewritten_parts;
    rewritten_parts.reserve(input_parts.size() + 1);
    bool wrote_effective_text = false;
    for (const auto& part : input_parts) {
      if (part.type == ContentPartType::Text) {
        if (!wrote_effective_text && !effective_input.empty()) {
          rewritten_parts.push_back(text_part(effective_input));
          wrote_effective_text = true;
        }
        continue;
      }
      rewritten_parts.push_back(part);
    }
    if (!wrote_effective_text && !effective_input.empty()) {
      rewritten_parts.insert(rewritten_parts.begin(), text_part(effective_input));
    }
    return rewritten_parts;
  }
  return input_parts;
}

AgentMessage input_message_for_effective_input(const AgentMessage& input_message,
                                               const std::string& input_text,
                                               const std::string& effective_input) {
  AgentMessage next = input_message;
  next.content = loop_input_parts_for_effective_input(input_message.content, input_text, effective_input);
  return next;
}


RunnerResolvedInvocation resolve_runner_invocation(const AgentRunnerResolvedConfig& config,
                                                   AgentMessage input_message,
                                                   Value input_value,
                                                   bool allow_skill_input_rewrite,
                                                   const std::string& session_id,
                                                   const ModelSettings& model_settings,
                                                   std::vector<SkillActivation> skill_activations,
                                                   Value context,
                                                   EventBus* event_bus,
                                                   const AgentRunnerDurableState* resume_state,
                                                   std::string span_name) {
  if (resume_state) {
    if (has_durable_input_message(resume_state->input_message)) {
      input_message = resume_state->input_message;
    } else if (!resume_state->input_parts.empty()) {
      input_message = user_input_message(resume_state->input_parts);
    }
    input_value = resume_state->input_value.is_null()
                      ? agent_message_to_value(input_message)
                      : resume_state->input_value;
  }

  RunnerResolvedInvocation invocation;
  invocation.input_message = std::move(input_message);
  invocation.input_value = std::move(input_value);
  invocation.input = extract_text_content(invocation.input_message.content);
  invocation.input_parts = invocation.input_message.content;
  invocation.run_input_value = invocation.input_value.is_null() ? Value(invocation.input) : invocation.input_value;
  invocation.session_id =
      resume_state && !resume_state->session_id.empty() ? resume_state->session_id : session_id;
  invocation.run_id = resume_state && !resume_state->run_id.empty() ? resume_state->run_id : generate_uuid();
  invocation.run_hook_context_value = std::move(context);
  invocation.run_trace = runner_trace_context(invocation.run_hook_context_value, invocation.run_id,
                                              std::move(span_name));
  invocation.base_model_settings =
      model_settings_empty(model_settings) ? config.model_settings : model_settings;

  SkillResolveOptions skill_opts;
  skill_opts.input_text = invocation.input;
  skill_opts.session_id = invocation.session_id;
  skill_opts.model_settings = invocation.base_model_settings;
  skill_opts.model_conflict = config.skill_model_conflict_policy;
  skill_opts.effort_conflict = config.skill_effort_conflict_policy;
  skill_opts.hooks = &config.hooks;
  skill_opts.event_bus = event_bus;
  skill_opts.trace_context = invocation.run_trace;
  skill_opts.metadata = Value::object({{"sessionId", invocation.session_id},
                                      {"runId", invocation.run_id}});
  for (const auto& name : config.default_skills) {
    skill_opts.activations.push_back(SkillActivation{name, "", SkillActivationSource::Host, 0});
  }
  for (auto& activation : skill_activations) {
    skill_opts.activations.push_back(std::move(activation));
  }
  invocation.skill_state = resolve_skills_state(config.skills.get(), std::move(skill_opts));

  const std::string resolved_effective_input =
      allow_skill_input_rewrite && !invocation.skill_state.effective_input_text.empty()
          ? invocation.skill_state.effective_input_text
          : invocation.input;
  invocation.effective_input =
      resume_state && !resume_state->effective_input.empty() ? resume_state->effective_input
                                                             : resolved_effective_input;
  invocation.input_text =
      resume_state && !resume_state->input_text.empty() ? resume_state->input_text
                                                        : invocation.effective_input;
  invocation.effective_input_value =
      resume_state && !resume_state->effective_input_value.is_null()
          ? resume_state->effective_input_value
          : input_value_for_effective_input(invocation.run_input_value, invocation.effective_input);
  invocation.effective_input_message =
      resume_state && has_durable_input_message(resume_state->effective_input_message)
          ? resume_state->effective_input_message
          : (allow_skill_input_rewrite
                 ? input_message_for_effective_input(invocation.input_message, invocation.input,
                                                     invocation.effective_input)
                 : invocation.input_message);
  invocation.effective_model_settings =
      resume_state && !model_settings_empty(resume_state->model_settings)
          ? resume_state->model_settings
          : invocation.skill_state.model_settings;
  invocation.knowledge_query = resolve_runner_knowledge_query(invocation.effective_input_message);
  if (invocation.effective_input_value.is_string() && !trim_copy(invocation.effective_input).empty()) {
    invocation.knowledge_query = text_knowledge_query(invocation.effective_input);
  }
  return invocation;
}


}  // namespace agent
