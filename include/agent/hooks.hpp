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

// Out-of-process hook adapter. Spawns an executable, pipes the hook payload
// (as JSON) to stdin, and reads stdout. Exit code 0 -> allow; exit code 2 ->
// block with stderr as the reason; other non-zero codes -> allow + warn.
struct ProcessHookConfig {
  std::filesystem::path executable;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::chrono::milliseconds timeout{5000};
};

enum class ProcessHookDecision {
  Allow,
  Block,
  Warn,
};

struct ProcessHookResult {
  ProcessHookDecision decision = ProcessHookDecision::Allow;
  int exit_code = 0;
  std::string stdout_text;
  std::string stderr_text;
};

// Runs a hook process synchronously given an arbitrary JSON-serialized payload.
// Returns Allow/Block/Warn. Unix-only (uses fork/exec/pipe).
ProcessHookResult run_process_hook(const ProcessHookConfig& config, const Value& payload);

// Convenience: returns a `before_tool` ToolHookContext callback that runs the
// process hook. If the hook blocks, throws ConfigurationError with the stderr
// as the reason so the executor surfaces it as a tool failure.
std::function<void(const ToolHookContext&)> make_process_tool_hook(ProcessHookConfig config);

}  // namespace agent
