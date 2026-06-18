#pragma once

#include "agent/execution.hpp"

namespace agent {

struct HookExecutionContext {
  std::string trace_id;
  std::string run_id;
  std::string workflow_run_id;
  ExecutionTarget target = ExecutionTarget::Run;
  Value metadata = Value::object({});
};

struct RunHookContext : HookExecutionContext {
  Value input;
  Value context = Value::object({});
  Value result;
  std::string error;
};

struct ModelHookContext : HookExecutionContext {
  Value request = Value::object({});
  Value response = Value::object({});
  std::string error;
};

struct ToolHookContext : HookExecutionContext {
  std::string tool_name;
  Value tool_call;
  Value input = Value::object({});
  Value result;
  std::string error;
};

// Specialised context for filesystem-write hooks. Carries the resolved target
// path and the proposed content so a pre-hook (e.g. one that runs git.snapshot)
// can take action before the write lands.
struct FsWriteHookContext : HookExecutionContext {
  std::string path;
  std::string content;
  std::string tool_name;  // typically "fs.writeText"
};

struct RetrievalHookContext : HookExecutionContext {
  std::string query;
  std::string source;
  Value result;
  std::string error;
};

struct PermissionHookContext : HookExecutionContext {
  std::string tool_name;
  std::string decision;
  std::string reason;
};

struct WorkflowHookContext : HookExecutionContext {
  std::string workflow_id;
  Value input;
  Value result;
  std::string error;
};

struct WorkflowNodeHookContext : HookExecutionContext {
  std::string workflow_id;
  std::string node_id;
  std::string node_type;
  Value input;
  Value result;
  std::string error;
};

struct ChildAgentHookContext : HookExecutionContext {
  std::string workflow_id;
  std::string agent_id;
  Value input;
  Value result;
  std::string error;
};

struct SkillActivationHookContext : HookExecutionContext {
  std::string skill_name;
  std::string activation_source;
  std::string arguments_text;
  int priority = 0;
  bool auto_selected = false;
  Value activation = Value::object({});
  Value manifest = Value::object({});
  Value skill = Value::object({});
  std::string rendered_prompt;
  std::vector<std::string> allowed_tools;
  std::string model;
  std::string effort;
  std::string error;
};

struct SkillsResolveHookContext : HookExecutionContext {
  std::string input_text;
  Value activations = Value::array({});
  Value available_skills = Value::array({});
  Value active_skills = Value::array({});
  Value auto_selected_skills = Value::array({});
  Value allowed_tools = Value::array({});
  std::string effective_input_text;
  Value model_settings_before = Value::object({});
  Value model_settings_after = Value::object({});
  Value result = Value::object({});
  std::string error;
};

struct HookSet {
  std::function<void(const RunHookContext&)> before_run;
  std::function<void(const RunHookContext&)> after_run;
  std::function<void(const RunHookContext&)> on_run_error;

  std::function<void(const ModelHookContext&)> before_model;
  std::function<void(const ModelHookContext&)> after_model;
  std::function<void(const ModelHookContext&)> on_model_error;

  std::function<void(const ToolHookContext&)> before_tool;
  std::function<void(const ToolHookContext&)> after_tool;
  std::function<void(const ToolHookContext&)> on_tool_error;

  std::function<void(const RetrievalHookContext&)> before_knowledge_retrieval;
  std::function<void(const RetrievalHookContext&)> after_knowledge_retrieval;
  std::function<void(const RetrievalHookContext&)> on_knowledge_retrieval_error;

  std::function<void(const PermissionHookContext&)> before_permission_check;
  std::function<void(const PermissionHookContext&)> after_permission_check;

  std::function<void(const WorkflowHookContext&)> before_workflow;
  std::function<void(const WorkflowHookContext&)> after_workflow;
  std::function<void(const WorkflowHookContext&)> on_workflow_error;

  std::function<void(const WorkflowNodeHookContext&)> before_workflow_node;
  std::function<void(const WorkflowNodeHookContext&)> after_workflow_node;
  std::function<void(const WorkflowNodeHookContext&)> on_workflow_node_error;

  std::function<void(const ChildAgentHookContext&)> before_child_agent;
  std::function<void(const ChildAgentHookContext&)> after_child_agent;
  std::function<void(const ChildAgentHookContext&)> on_child_agent_error;

  std::function<void(const SkillActivationHookContext&)> before_skill_activation;
  std::function<void(const SkillActivationHookContext&)> after_skill_activation;
  std::function<void(const SkillActivationHookContext&)> on_skill_activation_error;

  std::function<void(const SkillsResolveHookContext&)> before_skills_resolve;
  std::function<void(const SkillsResolveHookContext&)> after_skills_resolve;
  std::function<void(const SkillsResolveHookContext&)> on_skills_resolve_error;

  // Pre-hook fired by `fs.writeText` (and any other fs-write tool that opts in)
  // immediately before the on-disk write happens. Not default-on; intended for
  // snapshot / audit integrations.
  std::function<void(const FsWriteHookContext&)> before_fs_write;
};

HookSet merge_hooks(const std::vector<HookSet>& hooks = {});

// Severity for default-logging hooks. Plain enum (not enum class) so callers
// can compare numerically; values intentionally ordered from quiet to loud.
enum class HookLogSeverity {
  Trace = 0,
  Info = 1,
  Warn = 2,
  Error = 3,
};

struct HookLogEntry {
  HookLogSeverity severity = HookLogSeverity::Trace;
  std::string source;     // e.g. "after_tool", "on_run_error"
  std::string message;
  Value details = Value::object({});
};

// Sink for the default logging HookSet. When unset, entries below `Info` are
// dropped silently. Pass a custom sink to capture trace-level entries.
using HookLogSink = std::function<void(const HookLogEntry&)>;

// Returns a HookSet that logs hook outcomes with the "success silent, failure
// verbose" convention: successful hook callbacks emit at Trace, failures/blocks
// emit at Warn (or Error) with the full context attached.
HookSet default_logging_hook_set(HookLogSink sink = {});

}  // namespace agent
