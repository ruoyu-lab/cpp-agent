#pragma once

#include "agent/execution.hpp"

#include <mutex>

namespace agent {

struct HookSet;

struct SkillManifest {
  std::string name;
  std::string description;
  std::string argument_hint;
  bool user_invocable = false;
  bool disable_model_invocation = false;
  std::vector<std::string> allowed_tools;
  std::string model;
  std::string effort;
  std::string context = "inline";
  std::string agent;
  std::vector<std::string> paths;
  std::string tier = "core-safe";
  Value metadata = Value::object({});
};

struct SkillDefinition {
  SkillManifest manifest;
  std::string prompt;
  std::string file_path;
  std::string directory;
  std::string source = "custom";
};

struct SkillRenderContext {
  std::string arguments_text;
  std::string session_id;
  std::string skill_dir;
  std::map<std::string, std::string> env;
};

struct ResolvedSkillUse {
  SkillDefinition skill;
  std::string rendered_prompt;
  std::string arguments_text;
};

std::string render_skill_prompt(const SkillDefinition& skill, SkillRenderContext context = {});

class SkillRegistry {
 public:
  explicit SkillRegistry(std::vector<SkillDefinition> skills = {});
  SkillRegistry(const SkillRegistry& other);
  SkillRegistry& operator=(const SkillRegistry& other);
  SkillRegistry(SkillRegistry&& other) noexcept;
  SkillRegistry& operator=(SkillRegistry&& other) noexcept;
  SkillDefinition& register_skill(SkillDefinition skill);
  [[nodiscard]] const SkillDefinition* get(const std::string& name) const;
  [[nodiscard]] bool has(const std::string& name) const;
  [[nodiscard]] std::vector<SkillDefinition> list() const;
  [[nodiscard]] std::vector<SkillDefinition> model_invocable() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, SkillDefinition> skills_;
};

enum class SkillActivationSource {
  User,
  Host,
  Model,
};
std::string to_string(SkillActivationSource source);
SkillActivationSource skill_activation_source_from_string(const std::string& value,
                                                          SkillActivationSource fallback = SkillActivationSource::Host);

struct SkillActivation {
  std::string name;
  std::string arguments_text;
  SkillActivationSource source = SkillActivationSource::Host;
  int priority = 0;
};

enum class SkillConflictPolicy {
  Error,
  HighestPriority,
  FirstWins,
  LastWins,
};
std::string to_string(SkillConflictPolicy policy);
SkillConflictPolicy skill_conflict_policy_from_string(const std::string& value,
                                                      SkillConflictPolicy fallback = SkillConflictPolicy::Error);

struct SkillResolveOptions {
  std::vector<SkillActivation> activations;
  std::string input_text;
  std::vector<std::string> paths;
  std::filesystem::path cwd;
  std::string session_id;
  ModelSettings model_settings;
  SkillConflictPolicy model_conflict = SkillConflictPolicy::Error;
  SkillConflictPolicy effort_conflict = SkillConflictPolicy::Error;
  const HookSet* hooks = nullptr;
  EventBus* event_bus = nullptr;
  TraceContext trace_context;
  Value metadata = Value::object({});
};

struct ResolvedSkillsState {
  std::optional<AgentMessage> available_message;
  std::vector<AgentMessage> active_messages;
  std::vector<ResolvedSkillUse> active_skills;
  std::vector<std::string> auto_selected_skills;
  std::vector<std::string> allowed_tools;
  std::string effective_input_text;
  ModelSettings model_settings;
};

struct ParsedSlashActivations {
  std::vector<SkillActivation> activations;
  std::string stripped_input;
};
ParsedSlashActivations parse_slash_activations(const std::string& input, const SkillRegistry& registry);

ResolvedSkillsState resolve_skills_state(const SkillRegistry* registry, SkillResolveOptions options);

}  // namespace agent
