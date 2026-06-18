#include "agent/core_api.hpp"

namespace agent {

namespace {

template <typename Context, typename Selector>
std::function<void(const Context&)> merged_callback(std::shared_ptr<const std::vector<HookSet>> hooks,
                                                    Selector selector) {
  return [hooks = std::move(hooks), selector](const Context& context) {
    for (const auto& hook : *hooks) {
      const auto& callback = selector(hook);
      if (callback) {
        callback(context);
      }
    }
  };
}

}  // namespace

HookSet merge_hooks(const std::vector<HookSet>& hooks) {
  auto items = std::make_shared<std::vector<HookSet>>();
  items->reserve(hooks.size());
  for (const auto& hook : hooks) {
    items->push_back(hook);
  }

  HookSet merged;
  merged.before_run = merged_callback<RunHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.before_run;
  });
  merged.after_run = merged_callback<RunHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.after_run;
  });
  merged.on_run_error = merged_callback<RunHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.on_run_error;
  });

  merged.before_model = merged_callback<ModelHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.before_model;
  });
  merged.after_model = merged_callback<ModelHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.after_model;
  });
  merged.on_model_error = merged_callback<ModelHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.on_model_error;
  });

  merged.before_tool = merged_callback<ToolHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.before_tool;
  });
  merged.after_tool = merged_callback<ToolHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.after_tool;
  });
  merged.on_tool_error = merged_callback<ToolHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.on_tool_error;
  });

  merged.before_knowledge_retrieval =
      merged_callback<RetrievalHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.before_knowledge_retrieval;
      });
  merged.after_knowledge_retrieval =
      merged_callback<RetrievalHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.after_knowledge_retrieval;
      });
  merged.on_knowledge_retrieval_error =
      merged_callback<RetrievalHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.on_knowledge_retrieval_error;
      });

  merged.before_permission_check =
      merged_callback<PermissionHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.before_permission_check;
      });
  merged.after_permission_check =
      merged_callback<PermissionHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.after_permission_check;
      });

  merged.before_workflow = merged_callback<WorkflowHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.before_workflow;
  });
  merged.after_workflow = merged_callback<WorkflowHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.after_workflow;
  });
  merged.on_workflow_error = merged_callback<WorkflowHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.on_workflow_error;
  });

  merged.before_workflow_node =
      merged_callback<WorkflowNodeHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.before_workflow_node;
      });
  merged.after_workflow_node =
      merged_callback<WorkflowNodeHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.after_workflow_node;
      });
  merged.on_workflow_node_error =
      merged_callback<WorkflowNodeHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.on_workflow_node_error;
      });

  merged.before_fs_write = merged_callback<FsWriteHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.before_fs_write;
  });

  merged.before_child_agent = merged_callback<ChildAgentHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.before_child_agent;
  });
  merged.after_child_agent = merged_callback<ChildAgentHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.after_child_agent;
  });
  merged.on_child_agent_error = merged_callback<ChildAgentHookContext>(items, [](const HookSet& hook) -> const auto& {
    return hook.on_child_agent_error;
  });

  merged.before_skill_activation =
      merged_callback<SkillActivationHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.before_skill_activation;
      });
  merged.after_skill_activation =
      merged_callback<SkillActivationHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.after_skill_activation;
      });
  merged.on_skill_activation_error =
      merged_callback<SkillActivationHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.on_skill_activation_error;
      });

  merged.before_skills_resolve =
      merged_callback<SkillsResolveHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.before_skills_resolve;
      });
  merged.after_skills_resolve =
      merged_callback<SkillsResolveHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.after_skills_resolve;
      });
  merged.on_skills_resolve_error =
      merged_callback<SkillsResolveHookContext>(items, [](const HookSet& hook) -> const auto& {
        return hook.on_skills_resolve_error;
      });

  return merged;
}

HookSet default_logging_hook_set(HookLogSink sink) {
  auto shared_sink = std::make_shared<HookLogSink>(std::move(sink));
  auto emit = [shared_sink](HookLogSeverity severity, std::string source,
                            std::string message, Value details) {
    if (!*shared_sink) return;
    (*shared_sink)(HookLogEntry{severity, std::move(source), std::move(message), std::move(details)});
  };

  HookSet hooks;
  // Run hooks: success silent (trace), failure verbose (warn).
  hooks.after_run = [emit](const RunHookContext& ctx) {
    emit(HookLogSeverity::Trace, "after_run", "run completed",
         Value::object({{"runId", ctx.run_id}, {"traceId", ctx.trace_id}}));
  };
  hooks.on_run_error = [emit](const RunHookContext& ctx) {
    emit(HookLogSeverity::Warn, "on_run_error", "run failed",
         Value::object({{"runId", ctx.run_id}, {"traceId", ctx.trace_id}, {"error", ctx.error}}));
  };

  hooks.after_model = [emit](const ModelHookContext& ctx) {
    emit(HookLogSeverity::Trace, "after_model", "model call completed",
         Value::object({{"runId", ctx.run_id}, {"traceId", ctx.trace_id}}));
  };
  hooks.on_model_error = [emit](const ModelHookContext& ctx) {
    emit(HookLogSeverity::Warn, "on_model_error", "model call failed",
         Value::object({{"runId", ctx.run_id}, {"traceId", ctx.trace_id}, {"error", ctx.error},
                        {"request", ctx.request}}));
  };

  hooks.after_tool = [emit](const ToolHookContext& ctx) {
    emit(HookLogSeverity::Trace, "after_tool", "tool call completed",
         Value::object({{"toolName", ctx.tool_name}, {"runId", ctx.run_id}}));
  };
  hooks.on_tool_error = [emit](const ToolHookContext& ctx) {
    emit(HookLogSeverity::Warn, "on_tool_error", "tool call failed",
         Value::object({{"toolName", ctx.tool_name}, {"runId", ctx.run_id},
                        {"traceId", ctx.trace_id}, {"error", ctx.error}, {"input", ctx.input}}));
  };

  hooks.after_permission_check = [emit](const PermissionHookContext& ctx) {
    if (ctx.decision == "allow") {
      emit(HookLogSeverity::Trace, "after_permission_check", "permission allowed",
           Value::object({{"toolName", ctx.tool_name}, {"decision", ctx.decision}}));
    } else {
      emit(HookLogSeverity::Warn, "after_permission_check", "permission blocked",
           Value::object({{"toolName", ctx.tool_name}, {"decision", ctx.decision},
                          {"reason", ctx.reason}}));
    }
  };

  hooks.after_skill_activation = [emit](const SkillActivationHookContext& ctx) {
    emit(HookLogSeverity::Trace, "after_skill_activation", "skill activation completed",
         Value::object({{"skillName", ctx.skill_name},
                        {"activationSource", ctx.activation_source},
                        {"runId", ctx.run_id}}));
  };
  hooks.on_skill_activation_error = [emit](const SkillActivationHookContext& ctx) {
    emit(HookLogSeverity::Warn, "on_skill_activation_error", "skill activation failed",
         Value::object({{"skillName", ctx.skill_name},
                        {"activationSource", ctx.activation_source},
                        {"runId", ctx.run_id},
                        {"error", ctx.error}}));
  };
  hooks.after_skills_resolve = [emit](const SkillsResolveHookContext& ctx) {
    emit(HookLogSeverity::Trace, "after_skills_resolve", "skills resolved",
         Value::object({{"activeSkills", ctx.active_skills},
                        {"autoSelectedSkills", ctx.auto_selected_skills},
                        {"runId", ctx.run_id}}));
  };
  hooks.on_skills_resolve_error = [emit](const SkillsResolveHookContext& ctx) {
    emit(HookLogSeverity::Warn, "on_skills_resolve_error", "skills resolve failed",
         Value::object({{"inputText", ctx.input_text}, {"runId", ctx.run_id}, {"error", ctx.error}}));
  };

  return hooks;
}

}  // namespace agent
