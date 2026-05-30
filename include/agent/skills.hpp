#pragma once

#include "agent/model.hpp"

#include <mutex>

namespace agent {

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

SkillDefinition parse_anthropic_skill_markdown(const std::string& markdown, std::string file_path,
                                               std::string source = "custom");
std::string render_skill_prompt(const SkillDefinition& skill, SkillRenderContext context = {});
SkillDefinition load_skill_file(const std::filesystem::path& file_path, std::string source = "custom");
std::vector<SkillDefinition> load_skill_directory(const std::filesystem::path& directory,
                                                  std::string source = "custom");
std::vector<std::filesystem::path> collect_project_skill_directories(const std::filesystem::path& cwd);

struct AnthropicSkillRegistryLoadOptions {
  std::filesystem::path cwd = std::filesystem::current_path();
  bool include_user = true;
  std::filesystem::path user_home;
};

class SkillRegistry;
SkillRegistry load_anthropic_skill_registry(AnthropicSkillRegistryLoadOptions options = {});

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

// Identifies who triggered a skill activation. The runner uses this to gate
// `user_invocable`-only skills (User source must satisfy the manifest flag).
enum class SkillActivationSource {
  User,    // user typed "/name" or clicked a chip — must be user_invocable
  Host,    // host code injected (default skills, autonomous loop, workflow node)
  Model,   // model-side auto-selected
};
std::string to_string(SkillActivationSource source);
SkillActivationSource skill_activation_source_from_string(const std::string& value,
                                                          SkillActivationSource fallback = SkillActivationSource::Host);

// A single activation request. `name` must match a registered skill or it is
// dropped silently during resolution.
struct SkillActivation {
  std::string name;
  std::string arguments_text;
  SkillActivationSource source = SkillActivationSource::Host;
  // Used by HighestPriority conflict policy; higher wins.
  int priority = 0;
};

// Strategy for picking a winner when multiple active skills request different
// models or reasoning effort levels.
enum class SkillConflictPolicy {
  Error,           // throw ConfigurationError when activations disagree
  HighestPriority, // pick the SkillActivation with max priority (stable on ties)
  FirstWins,       // first activation in the activations list wins
  LastWins,        // last activation in the activations list wins
};
std::string to_string(SkillConflictPolicy policy);
SkillConflictPolicy skill_conflict_policy_from_string(const std::string& value,
                                                      SkillConflictPolicy fallback = SkillConflictPolicy::Error);

struct SkillResolveOptions {
  std::vector<SkillActivation> activations;
  std::string input_text;
  std::string session_id;
  ModelSettings model_settings;
  SkillConflictPolicy model_conflict = SkillConflictPolicy::Error;
  SkillConflictPolicy effort_conflict = SkillConflictPolicy::Error;
};

struct ResolvedSkillsState {
  std::optional<AgentMessage> available_message;
  std::vector<AgentMessage> active_messages;
  std::vector<ResolvedSkillUse> active_skills;
  std::vector<std::string> auto_selected_skills;
  std::vector<std::string> allowed_tools;
  // Set when the slash parser stripped command tokens from the front of the
  // input. Empty otherwise — callers should fall back to the original input.
  std::string effective_input_text;
  ModelSettings model_settings;
};

// Multi-slash parser.
// - Scans leading whitespace, then consumes a run of `/<token>` separated by
//   whitespace. Each `/<token>` becomes a `SkillActivation{source=User,
//   args=""}` if the registry knows it AND the skill is user_invocable.
//   Tokens that don't resolve stop the scan (the slash is treated as literal
//   text — parser does not silently drop user typing).
// - The remainder (post the resolved run) is `stripped_input`. Leading and
//   trailing whitespace are trimmed.
// - Special case: if exactly one `/<token>` was consumed and the remainder is
//   non-empty, the remainder becomes BOTH the activation's `arguments_text`
//   AND `stripped_input`. This preserves the historical single-slash CLI
//   ergonomic (`/audit src tests` activates `audit` with `src tests`).
// - Returns empty activations and the original input if the very first
//   non-whitespace char is not '/'.
struct ParsedSlashActivations {
  std::vector<SkillActivation> activations;
  std::string stripped_input;
};
ParsedSlashActivations parse_slash_activations(const std::string& input, const SkillRegistry& registry);

// Resolves the prompt-time skill state from a structured options bag.
//
// Resolution order:
//   1. The slash parser consumes leading `/<name>` tokens from
//      `options.input_text` and prepends the resulting `User` activations to
//      `options.activations` (any name already present is left alone — first
//      occurrence wins).
//   2. Activations are deduped by name, preserving the first occurrence's
//      source/priority/arguments.
//   3. User-source activations whose registry entry is not user_invocable
//      throw `ConfigurationError` containing "not user-invocable".
//   4. Unknown skill names are silently dropped.
//   5. `auto_select_skills` appends additional `Model`-source activations.
//   6. Each surviving activation is rendered as a System message and its
//      `allowed_tools` are merged.
//   7. Model and effort conflicts are resolved per `options.model_conflict`
//      and `options.effort_conflict`. If the caller already set
//      `options.model_settings.model` or `options.model_settings.reasoning`,
//      the explicit caller value wins and conflict resolution is skipped for
//      that field.
ResolvedSkillsState resolve_skills_state(const SkillRegistry* registry, SkillResolveOptions options);

}  // namespace agent
