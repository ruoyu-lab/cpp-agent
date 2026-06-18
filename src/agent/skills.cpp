#include "agent/skills_core.hpp"
#include "agent/hooks.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <regex>
#include <sstream>

namespace agent {

namespace {

std::vector<std::string> tokenize_skill_arguments(const std::string& value) {
  std::vector<std::string> tokens;
  std::string current;
  int depth = 0;
  for (const char ch : trim_copy(value)) {
    if (ch == '(') {
      depth += 1;
      current.push_back(ch);
      continue;
    }
    if (ch == ')') {
      depth = std::max(0, depth - 1);
      current.push_back(ch);
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) && depth == 0) {
      const auto token = trim_copy(current);
      if (!token.empty()) {
        tokens.push_back(token);
      }
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  const auto token = trim_copy(current);
  if (!token.empty()) {
    tokens.push_back(token);
  }
  return tokens;
}

bool is_env_placeholder_name(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isalnum(ch) || ch == '_';
  });
}

std::string skill_env_value(const std::map<std::string, std::string>& env, const std::string& key) {
  const auto found = env.find(key);
  if (found != env.end()) {
    return found->second;
  }
  const char* value = std::getenv(key.c_str());
  return value ? std::string(value) : std::string();
}

std::string replace_skill_env_placeholders(std::string rendered,
                                           const std::map<std::string, std::string>& env) {
  std::string output;
  output.reserve(rendered.size());
  for (std::size_t index = 0; index < rendered.size();) {
    if (rendered[index] != '$' || index + 2 >= rendered.size() || rendered[index + 1] != '{') {
      output.push_back(rendered[index++]);
      continue;
    }
    const auto end = rendered.find('}', index + 2);
    if (end == std::string::npos) {
      output.push_back(rendered[index++]);
      continue;
    }
    const auto key = rendered.substr(index + 2, end - index - 2);
    if (!is_env_placeholder_name(key)) {
      output.append(rendered.substr(index, end - index + 1));
      index = end + 1;
      continue;
    }
    output += skill_env_value(env, key);
    index = end + 1;
  }
  return output;
}

void replace_all(std::string& text, const std::string& needle, const std::string& replacement) {
  if (needle.empty()) {
    return;
  }
  std::size_t pos = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    text.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
}

bool is_skill_selection_stop_word(const std::string& token) {
  static const std::set<std::string> stop_words = {
      "about",   "after",  "agent",  "also",   "and",    "are",    "before", "can",    "could",
      "for",     "from",   "help",   "into",   "make",   "please", "project", "run",   "skill",
      "task",    "that",   "the",    "this",   "tool",   "use",    "using",  "with",  "would",
      "you",     "your",
  };
  return token.size() < 3 || stop_words.contains(token);
}

std::string singular_skill_token(std::string token) {
  if (token.size() > 4 && token.ends_with("ies")) {
    token.resize(token.size() - 3);
    token += "y";
    return token;
  }
  if (token.size() > 4 && token.ends_with("es") && !token.ends_with("ses")) {
    token.resize(token.size() - 2);
    return token;
  }
  if (token.size() > 3 && token.back() == 's' && !token.ends_with("ss")) {
    token.pop_back();
  }
  return token;
}

std::set<std::string> skill_selection_token_set(const std::string& text) {
  std::set<std::string> result;
  for (auto token : tokenize(text)) {
    if (is_skill_selection_stop_word(token)) {
      continue;
    }
    result.insert(token);
    const auto singular = singular_skill_token(token);
    if (!is_skill_selection_stop_word(singular)) {
      result.insert(singular);
    }
  }
  return result;
}

std::vector<std::string> meaningful_skill_tokens(const std::string& text) {
  std::vector<std::string> result;
  for (auto token : tokenize(text)) {
    token = singular_skill_token(std::move(token));
    if (!is_skill_selection_stop_word(token)) {
      result.push_back(std::move(token));
    }
  }
  return result;
}

std::string join_tokens_for_selection(const std::vector<std::string>& tokens) {
  std::string joined;
  for (const auto& token : tokens) {
    if (!joined.empty()) {
      joined += ' ';
    }
    joined += token;
  }
  return joined;
}

bool token_phrase_present(const std::string& normalized_input, const std::vector<std::string>& tokens) {
  if (tokens.empty()) {
    return false;
  }
  return (" " + normalized_input + " ").find(" " + join_tokens_for_selection(tokens) + " ") != std::string::npos;
}

int score_skill_selection(const SkillDefinition& skill, const std::set<std::string>& input_tokens,
                          const std::string& normalized_input) {
  const auto name_tokens = meaningful_skill_tokens(skill.manifest.name);
  const auto description_tokens = meaningful_skill_tokens(skill.manifest.description);
  const auto argument_tokens = meaningful_skill_tokens(skill.manifest.argument_hint);

  int score = 0;
  int matched_name_tokens = 0;
  for (const auto& token : name_tokens) {
    if (input_tokens.contains(token)) {
      score += 3;
      matched_name_tokens += 1;
    }
  }
  if (!name_tokens.empty() && matched_name_tokens == static_cast<int>(name_tokens.size())) {
    score += 4;
  }
  if (token_phrase_present(normalized_input, name_tokens)) {
    score += 6;
  }

  int description_matches = 0;
  for (const auto& token : description_tokens) {
    if (input_tokens.contains(token)) {
      description_matches += 1;
    }
  }
  score += std::min(description_matches, 6);

  int argument_matches = 0;
  for (const auto& token : argument_tokens) {
    if (input_tokens.contains(token)) {
      argument_matches += 1;
    }
  }
  score += std::min(argument_matches, 3);
  return score;
}

std::string normalize_skill_match_path(std::string path, const std::filesystem::path& cwd) {
  path = trim_copy(std::move(path));
  if (path.empty()) {
    return {};
  }
  std::filesystem::path fs_path(path);
  if (!cwd.empty()) {
    std::error_code error;
    if (fs_path.is_absolute()) {
      auto relative_path = std::filesystem::relative(fs_path, cwd, error);
      if (!error && !relative_path.empty()) {
        const auto text = relative_path.string();
        if (text.rfind("..", 0) != 0) {
          path = text;
        }
      }
    } else if (path.rfind("./", 0) == 0 || path.rfind(".\\", 0) == 0) {
      path = (cwd / fs_path).lexically_normal().lexically_relative(cwd).string();
    }
  }
  std::replace(path.begin(), path.end(), '\\', '/');
  while (path.rfind("./", 0) == 0) {
    path.erase(0, 2);
  }
  while (path.find("//") != std::string::npos) {
    path.replace(path.find("//"), 2, "/");
  }
  while (path.size() > 1 && path.back() == '/') {
    path.pop_back();
  }
  return path;
}

std::string glob_to_regex_source(const std::string& pattern) {
  std::string source = "^";
  for (std::size_t index = 0; index < pattern.size(); ++index) {
    const char ch = pattern[index];
    if (ch == '*') {
      const bool is_double = index + 1 < pattern.size() && pattern[index + 1] == '*';
      if (is_double && index + 2 < pattern.size() && pattern[index + 2] == '/') {
        source += "(?:.*/)?";
        index += 2;
      } else if (is_double) {
        source += ".*";
        index += 1;
      } else {
        source += "[^/]*";
      }
      continue;
    }
    if (ch == '?') {
      source += "[^/]";
      continue;
    }
    if (std::string(".+^${}()|[]\\").find(ch) != std::string::npos) {
      source += '\\';
    }
    source += ch;
  }
  source += "$";
  return source;
}

bool skill_path_matches_pattern(const std::string& path,
                                const std::string& pattern,
                                const std::filesystem::path& cwd) {
  const auto normalized_path = normalize_skill_match_path(path, cwd);
  const auto normalized_pattern = normalize_skill_match_path(pattern, cwd);
  if (normalized_path.empty() || normalized_pattern.empty()) {
    return false;
  }
  if (normalized_pattern.find('*') == std::string::npos &&
      normalized_pattern.find('?') == std::string::npos) {
    return normalized_path == normalized_pattern ||
           normalized_path.rfind(normalized_pattern + "/", 0) == 0;
  }
  return std::regex_match(normalized_path, std::regex(glob_to_regex_source(normalized_pattern)));
}

std::vector<std::string> select_path_triggered_skills(const SkillRegistry* registry,
                                                      const std::set<std::string>& requested,
                                                      const std::vector<std::string>& paths,
                                                      const std::filesystem::path& cwd) {
  if (!registry || paths.empty()) {
    return {};
  }
  std::vector<std::string> selected;
  for (const auto& skill : registry->model_invocable()) {
    if (requested.contains(skill.manifest.name) || skill.manifest.paths.empty()) {
      continue;
    }
    bool matched = false;
    for (const auto& pattern : skill.manifest.paths) {
      for (const auto& path : paths) {
        if (skill_path_matches_pattern(path, pattern, cwd)) {
          matched = true;
          break;
        }
      }
      if (matched) {
        break;
      }
    }
    if (matched) {
      selected.push_back(skill.manifest.name);
    }
  }
  return selected;
}

std::vector<std::string> auto_select_skills(const SkillRegistry* registry, const std::set<std::string>& requested,
                                            const std::string& input_text, const ModelSettings& model_settings,
                                            const std::string& invoked_skill) {
  constexpr int kMinimumSkillSelectionScore = 5;
  constexpr std::size_t kMaxAutoSelectedSkills = 3;
  if (!registry || !invoked_skill.empty() || trim_copy(input_text).empty()) {
    return {};
  }

  const auto input_tokens = skill_selection_token_set(input_text);
  if (input_tokens.empty()) {
    return {};
  }
  const auto normalized_input = join_tokens_for_selection(meaningful_skill_tokens(input_text));

  struct Candidate {
    std::string name;
    int score = 0;
  };

  std::vector<Candidate> candidates;
  for (const auto& skill : registry->model_invocable()) {
    if (requested.contains(skill.manifest.name)) {
      continue;
    }
    const int score = score_skill_selection(skill, input_tokens, normalized_input);
    if (score >= kMinimumSkillSelectionScore) {
      candidates.push_back(Candidate{skill.manifest.name, score});
    }
  }
  std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    return left.name < right.name;
  });

  std::string selected_model;
  std::string selected_effort;
  for (const auto& name : requested) {
    const auto* skill = registry->get(name);
    if (!skill) {
      continue;
    }
    if (!skill->manifest.model.empty()) {
      selected_model = selected_model.empty() ? skill->manifest.model : selected_model;
    }
    if (!skill->manifest.effort.empty()) {
      selected_effort = selected_effort.empty() ? skill->manifest.effort : selected_effort;
    }
  }
  if (!model_settings.model.empty()) {
    selected_model = model_settings.model;
  }
  if (model_settings.reasoning && std::holds_alternative<std::string>(model_settings.reasoning->budget)) {
    selected_effort = std::get<std::string>(model_settings.reasoning->budget);
  }

  std::vector<std::string> selected;
  for (const auto& candidate : candidates) {
    if (selected.size() >= kMaxAutoSelectedSkills) {
      break;
    }
    const auto* skill = registry->get(candidate.name);
    if (!skill) {
      continue;
    }
    if (!selected_model.empty() && !skill->manifest.model.empty() && skill->manifest.model != selected_model) {
      continue;
    }
    if (!selected_effort.empty() && !skill->manifest.effort.empty() && skill->manifest.effort != selected_effort) {
      continue;
    }
    if (selected_model.empty() && !skill->manifest.model.empty()) {
      selected_model = skill->manifest.model;
    }
    if (selected_effort.empty() && !skill->manifest.effort.empty()) {
      selected_effort = skill->manifest.effort;
    }
    selected.push_back(candidate.name);
  }
  std::sort(selected.begin(), selected.end());
  return selected;
}

}  // namespace

std::string render_skill_prompt(const SkillDefinition& skill, SkillRenderContext context) {
  std::string rendered = skill.prompt;
  const std::string skill_dir = context.skill_dir.empty() ? skill.directory : context.skill_dir;
  const std::vector<std::string> args = tokenize_skill_arguments(context.arguments_text);
  for (std::size_t reverse = args.size(); reverse > 0; --reverse) {
    const auto index = reverse - 1;
    replace_all(rendered, "$ARGUMENTS[" + std::to_string(index) + "]", args[index]);
  }
  for (std::size_t reverse = args.size(); reverse > 0; --reverse) {
    const auto index = reverse - 1;
    replace_all(rendered, "$" + std::to_string(index + 1), args[index]);
  }
  replace_all(rendered, "$ARGUMENTS", context.arguments_text);
  replace_all(rendered, "${CLAUDE_SESSION_ID}", context.session_id);
  replace_all(rendered, "${CLAUDE_SKILL_DIR}", skill_dir);
  return trim_copy(replace_skill_env_placeholders(std::move(rendered), context.env));
}

SkillRegistry::SkillRegistry(std::vector<SkillDefinition> skills) {
  for (auto& skill : skills) {
    register_skill(std::move(skill));
  }
}

SkillRegistry::SkillRegistry(const SkillRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  skills_ = other.skills_;
}

SkillRegistry& SkillRegistry::operator=(const SkillRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  skills_ = other.skills_;
  return *this;
}

SkillRegistry::SkillRegistry(SkillRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  skills_ = std::move(other.skills_);
}

SkillRegistry& SkillRegistry::operator=(SkillRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  skills_ = std::move(other.skills_);
  return *this;
}

SkillDefinition& SkillRegistry::register_skill(SkillDefinition skill) {
  if (skill.manifest.name.empty()) {
    throw ConfigurationError("Skill definition requires manifest.name.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto name = skill.manifest.name;
  skills_[name] = std::move(skill);
  return skills_.at(name);
}

const SkillDefinition* SkillRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = skills_.find(name);
  return found == skills_.end() ? nullptr : &found->second;
}

bool SkillRegistry::has(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skills_.contains(name);
}

std::vector<SkillDefinition> SkillRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SkillDefinition> skills;
  for (const auto& [_, skill] : skills_) {
    skills.push_back(skill);
  }
  return skills;
}

std::vector<SkillDefinition> SkillRegistry::model_invocable() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<SkillDefinition> skills;
  for (const auto& [_, skill] : skills_) {
    if (!skill.manifest.disable_model_invocation) {
      skills.push_back(skill);
    }
  }
  return skills;
}

std::string to_string(SkillActivationSource source) {
  switch (source) {
    case SkillActivationSource::User:
      return "user";
    case SkillActivationSource::Host:
      return "host";
    case SkillActivationSource::Model:
      return "model";
  }
  return "host";
}

SkillActivationSource skill_activation_source_from_string(const std::string& value,
                                                          SkillActivationSource fallback) {
  if (value == "user") return SkillActivationSource::User;
  if (value == "host") return SkillActivationSource::Host;
  if (value == "model") return SkillActivationSource::Model;
  return fallback;
}

std::string to_string(SkillConflictPolicy policy) {
  switch (policy) {
    case SkillConflictPolicy::Error:
      return "error";
    case SkillConflictPolicy::HighestPriority:
      return "highest-priority";
    case SkillConflictPolicy::FirstWins:
      return "first-wins";
    case SkillConflictPolicy::LastWins:
      return "last-wins";
  }
  return "error";
}

SkillConflictPolicy skill_conflict_policy_from_string(const std::string& value,
                                                      SkillConflictPolicy fallback) {
  if (value == "error") return SkillConflictPolicy::Error;
  if (value == "highest-priority" || value == "highestPriority") return SkillConflictPolicy::HighestPriority;
  if (value == "first-wins" || value == "firstWins") return SkillConflictPolicy::FirstWins;
  if (value == "last-wins" || value == "lastWins") return SkillConflictPolicy::LastWins;
  return fallback;
}

namespace {

// Returns the length of the leading whitespace prefix of `text`.
std::size_t leading_whitespace(const std::string& text) {
  std::size_t index = 0;
  while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) {
    index += 1;
  }
  return index;
}

}  // namespace

ParsedSlashActivations parse_slash_activations(const std::string& input, const SkillRegistry& registry) {
  ParsedSlashActivations result;
  result.stripped_input = input;

  const std::size_t first = leading_whitespace(input);
  if (first >= input.size() || input[first] != '/') {
    return result;
  }

  std::vector<std::pair<std::string, std::size_t>> consumed;  // name, end position (exclusive)
  std::size_t cursor = first;
  while (cursor < input.size() && input[cursor] == '/') {
    const std::size_t name_start = cursor + 1;
    std::size_t name_end = name_start;
    while (name_end < input.size() &&
           !std::isspace(static_cast<unsigned char>(input[name_end]))) {
      name_end += 1;
    }
    if (name_end == name_start) {
      break;  // bare slash, treat as literal
    }
    const std::string name = input.substr(name_start, name_end - name_start);
    const auto* skill = registry.get(name);
    if (!skill || !skill->manifest.user_invocable) {
      break;  // unresolved or not user-invocable — stop scanning, keep literal
    }
    consumed.emplace_back(name, name_end);
    // Skip whitespace to next token.
    std::size_t next = name_end;
    while (next < input.size() && std::isspace(static_cast<unsigned char>(input[next]))) {
      next += 1;
    }
    cursor = next;
  }

  if (consumed.empty()) {
    return result;
  }

  const std::size_t tail_start = cursor;
  std::string remainder = tail_start < input.size() ? input.substr(tail_start) : std::string{};
  remainder = trim_copy(remainder);

  for (auto& [name, _] : consumed) {
    SkillActivation activation;
    activation.name = name;
    activation.source = SkillActivationSource::User;
    activation.arguments_text = "";
    result.activations.push_back(std::move(activation));
  }

  if (consumed.size() == 1 && !remainder.empty()) {
    result.activations.front().arguments_text = remainder;
    result.stripped_input = remainder;
  } else {
    result.stripped_input = remainder;
  }
  return result;
}

namespace {

struct ActivationRecord {
  std::string name;
  std::string arguments_text;
  SkillActivationSource source = SkillActivationSource::Host;
  int priority = 0;
  std::size_t order = 0;  // position in the final activations list
  bool auto_selected = false;
};

Value string_list_value(const std::vector<std::string>& values) {
  Value::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.emplace_back(value);
  }
  return Value(std::move(array));
}

Value skill_manifest_value(const SkillManifest& manifest) {
  return Value::object({
      {"name", manifest.name},
      {"description", manifest.description},
      {"argumentHint", manifest.argument_hint},
      {"userInvocable", manifest.user_invocable},
      {"disableModelInvocation", manifest.disable_model_invocation},
      {"allowedTools", string_list_value(manifest.allowed_tools)},
      {"model", manifest.model},
      {"effort", manifest.effort},
      {"context", manifest.context},
      {"agent", manifest.agent},
      {"paths", string_list_value(manifest.paths)},
      {"tier", manifest.tier},
      {"metadata", manifest.metadata},
  });
}

Value skill_definition_value(const SkillDefinition& skill) {
  return Value::object({
      {"manifest", skill_manifest_value(skill.manifest)},
      {"prompt", skill.prompt},
      {"filePath", skill.file_path},
      {"directory", skill.directory},
      {"source", skill.source},
  });
}

Value activation_record_value(const ActivationRecord& record) {
  return Value::object({
      {"name", record.name},
      {"argumentsText", record.arguments_text},
      {"source", to_string(record.source)},
      {"priority", record.priority},
      {"order", static_cast<long long>(record.order)},
      {"autoSelected", record.auto_selected},
  });
}

Value skill_activations_value(const std::vector<SkillActivation>& activations) {
  Value::Array array;
  array.reserve(activations.size());
  for (std::size_t index = 0; index < activations.size(); ++index) {
    const auto& activation = activations[index];
    array.push_back(Value::object({
        {"name", activation.name},
        {"argumentsText", activation.arguments_text},
        {"source", to_string(activation.source)},
        {"priority", activation.priority},
        {"order", static_cast<long long>(index)},
    }));
  }
  return Value(std::move(array));
}

Value available_skills_value(const std::vector<SkillDefinition>& skills) {
  Value::Array array;
  array.reserve(skills.size());
  for (const auto& skill : skills) {
    array.push_back(Value::object({
        {"name", skill.manifest.name},
        {"description", skill.manifest.description},
        {"argumentHint", skill.manifest.argument_hint},
        {"userInvocable", skill.manifest.user_invocable},
        {"modelInvocable", !skill.manifest.disable_model_invocation},
        {"tier", skill.manifest.tier},
    }));
  }
  return Value(std::move(array));
}

Value active_skills_value(const std::vector<ResolvedSkillUse>& skills) {
  Value::Array array;
  array.reserve(skills.size());
  for (const auto& entry : skills) {
    array.push_back(Value::object({
        {"name", entry.skill.manifest.name},
        {"argumentsText", entry.arguments_text},
        {"manifest", skill_manifest_value(entry.skill.manifest)},
        {"renderedPrompt", entry.rendered_prompt},
    }));
  }
  return Value(std::move(array));
}

Value resolved_skills_state_value(const ResolvedSkillsState& state) {
  return Value::object({
      {"availableMessage", state.available_message ? agent_message_to_value(*state.available_message) : Value()},
      {"activeMessages", [&] {
         Value::Array messages;
         messages.reserve(state.active_messages.size());
         for (const auto& message : state.active_messages) {
           messages.push_back(agent_message_to_value(message));
         }
         return Value(std::move(messages));
       }()},
      {"activeSkills", active_skills_value(state.active_skills)},
      {"autoSelectedSkills", string_list_value(state.auto_selected_skills)},
      {"allowedTools", string_list_value(state.allowed_tools)},
      {"effectiveInputText", state.effective_input_text},
      {"modelSettings", model_settings_to_json_value(state.model_settings)},
  });
}

void populate_skill_hook_base(HookExecutionContext& context, const SkillResolveOptions& options) {
  context.target = ExecutionTarget::Skill;
  context.trace_id = options.trace_context.trace_id;
  context.run_id = options.trace_context.run_id;
  context.workflow_run_id = options.trace_context.workflow_run_id;
  context.metadata = options.metadata;
}

SkillActivationHookContext skill_activation_hook_context(const SkillResolveOptions& options,
                                                        const ActivationRecord& record,
                                                        const SkillDefinition& skill,
                                                        std::string rendered_prompt = {},
                                                        std::string error = {}) {
  SkillActivationHookContext context;
  populate_skill_hook_base(context, options);
  context.skill_name = skill.manifest.name;
  context.activation_source = to_string(record.source);
  context.arguments_text = record.arguments_text;
  context.priority = record.priority;
  context.auto_selected = record.auto_selected;
  context.activation = activation_record_value(record);
  context.manifest = skill_manifest_value(skill.manifest);
  context.skill = skill_definition_value(skill);
  context.rendered_prompt = std::move(rendered_prompt);
  context.allowed_tools = skill.manifest.allowed_tools;
  context.model = skill.manifest.model;
  context.effort = skill.manifest.effort;
  context.error = std::move(error);
  return context;
}

SkillsResolveHookContext skills_resolve_hook_context(const SkillResolveOptions& options,
                                                     const std::vector<SkillActivation>& activations,
                                                     const std::vector<SkillDefinition>& available,
                                                     const ResolvedSkillsState& state,
                                                     std::string error = {}) {
  SkillsResolveHookContext context;
  populate_skill_hook_base(context, options);
  context.input_text = options.input_text;
  context.activations = skill_activations_value(activations);
  context.available_skills = available_skills_value(available);
  context.active_skills = active_skills_value(state.active_skills);
  context.auto_selected_skills = string_list_value(state.auto_selected_skills);
  context.allowed_tools = string_list_value(state.allowed_tools);
  context.effective_input_text = state.effective_input_text;
  context.model_settings_before = model_settings_to_json_value(options.model_settings);
  context.model_settings_after = model_settings_to_json_value(state.model_settings);
  context.result = resolved_skills_state_value(state);
  context.error = std::move(error);
  return context;
}

void publish_skill_event(const SkillResolveOptions& options, const std::string& category, Value payload) {
  if (options.event_bus) {
    options.event_bus->publish(category, ExecutionTarget::Skill, std::move(payload), options.trace_context);
  }
}

Value skill_activation_event_payload(const SkillActivationHookContext& context) {
  return Value::object({
      {"skillName", context.skill_name},
      {"activationSource", context.activation_source},
      {"argumentsText", context.arguments_text},
      {"priority", context.priority},
      {"autoSelected", context.auto_selected},
      {"activation", context.activation},
      {"manifest", context.manifest},
      {"model", context.model},
      {"effort", context.effort},
      {"allowedTools", string_list_value(context.allowed_tools)},
      {"renderedPrompt", context.rendered_prompt},
      {"error", context.error},
  });
}

Value skills_resolve_event_payload(const SkillsResolveHookContext& context) {
  return Value::object({
      {"inputText", context.input_text},
      {"activations", context.activations},
      {"availableSkills", context.available_skills},
      {"activeSkills", context.active_skills},
      {"autoSelectedSkills", context.auto_selected_skills},
      {"allowedTools", context.allowed_tools},
      {"effectiveInputText", context.effective_input_text},
      {"modelSettingsBefore", context.model_settings_before},
      {"modelSettingsAfter", context.model_settings_after},
      {"result", context.result},
      {"error", context.error},
  });
}

// Picks a winner from `entries` according to `policy`. Each entry is
// `(value, priority, order)`. Throws ConfigurationError if `policy ==
// Error` and the unique values are more than 1.
std::string resolve_conflict(SkillConflictPolicy policy,
                             const std::vector<std::tuple<std::string, int, std::size_t>>& entries,
                             const std::string& kind) {
  if (entries.empty()) {
    return {};
  }
  std::set<std::string> unique;
  for (const auto& [value, _, __] : entries) {
    unique.insert(value);
  }
  if (unique.size() <= 1) {
    return std::get<0>(entries.front());
  }
  switch (policy) {
    case SkillConflictPolicy::Error: {
      std::string joined;
      for (const auto& value : unique) {
        if (!joined.empty()) joined += ", ";
        joined += value;
      }
      if (kind == "models") {
        throw ConfigurationError("Active skills request conflicting models: " + joined + ".");
      }
      throw ConfigurationError("Active skills request conflicting reasoning effort: " + joined + ".");
    }
    case SkillConflictPolicy::HighestPriority: {
      const auto* best = &entries.front();
      for (const auto& entry : entries) {
        if (std::get<1>(entry) > std::get<1>(*best) ||
            (std::get<1>(entry) == std::get<1>(*best) && std::get<2>(entry) < std::get<2>(*best))) {
          best = &entry;
        }
      }
      return std::get<0>(*best);
    }
    case SkillConflictPolicy::FirstWins: {
      const auto* best = &entries.front();
      for (const auto& entry : entries) {
        if (std::get<2>(entry) < std::get<2>(*best)) {
          best = &entry;
        }
      }
      return std::get<0>(*best);
    }
    case SkillConflictPolicy::LastWins: {
      const auto* best = &entries.front();
      for (const auto& entry : entries) {
        if (std::get<2>(entry) > std::get<2>(*best)) {
          best = &entry;
        }
      }
      return std::get<0>(*best);
    }
  }
  return {};
}

}  // namespace

ResolvedSkillsState resolve_skills_state(const SkillRegistry* registry, SkillResolveOptions options) {
  ResolvedSkillsState state;
  state.model_settings = options.model_settings;
  std::vector<SkillActivation> observed_activations = options.activations;
  std::vector<SkillDefinition> available;

  try {
    auto before_context = skills_resolve_hook_context(options, observed_activations, available, state);
    publish_skill_event(options, "skills.resolve.started", skills_resolve_event_payload(before_context));
    if (options.hooks && options.hooks->before_skills_resolve) {
      options.hooks->before_skills_resolve(before_context);
    }

    if (!registry) {
      auto after_context = skills_resolve_hook_context(options, observed_activations, available, state);
      if (options.hooks && options.hooks->after_skills_resolve) {
        options.hooks->after_skills_resolve(after_context);
      }
      publish_skill_event(options, "skills.resolve.completed", skills_resolve_event_payload(after_context));
      return state;
    }

    // 1. Run the slash parser. Prepend its activations so that names not already
    // covered by explicit caller activations become visible to dedup.
    ParsedSlashActivations parsed = parse_slash_activations(options.input_text, *registry);
    std::set<std::string> existing_names;
    for (const auto& act : options.activations) {
      existing_names.insert(act.name);
    }
    std::vector<SkillActivation> merged;
    merged.reserve(parsed.activations.size() + options.activations.size());
    for (auto& act : parsed.activations) {
      if (existing_names.count(act.name)) {
        continue;  // explicit caller wins
      }
      merged.push_back(std::move(act));
    }
    for (auto& act : options.activations) {
      merged.push_back(std::move(act));
    }
    observed_activations = merged;

    // 2. Dedup by name, preserving FIRST occurrence's source/args/priority.
    std::vector<ActivationRecord> records;
    std::set<std::string> seen;
    for (auto& act : merged) {
      if (act.name.empty()) continue;
      if (!seen.insert(act.name).second) continue;
      ActivationRecord rec;
      rec.name = act.name;
      rec.arguments_text = act.arguments_text;
      rec.source = act.source;
      rec.priority = act.priority;
      rec.order = records.size();
      records.push_back(std::move(rec));
    }

    std::vector<std::string> path_selected_skills =
        select_path_triggered_skills(registry, seen, options.paths, options.cwd);
    for (const auto& name : path_selected_skills) {
      if (!seen.insert(name).second) {
        continue;
      }
      ActivationRecord rec;
      rec.name = name;
      rec.source = SkillActivationSource::Model;
      rec.priority = 0;
      rec.order = records.size();
      rec.auto_selected = true;
      records.push_back(std::move(rec));
      observed_activations.push_back(SkillActivation{
          .name = name,
          .arguments_text = "",
          .source = SkillActivationSource::Model,
          .priority = 0,
      });
    }

    auto begin_activation = [&](ActivationRecord rec,
                                std::vector<ActivationRecord>& active_records,
                                std::set<std::string>& active_names) {
      const auto* skill = registry->get(rec.name);
      if (!skill) {
        return;  // unknown — drop silently
      }

      auto hook_context = skill_activation_hook_context(options, rec, *skill);
      publish_skill_event(options, "skill.activation.started", skill_activation_event_payload(hook_context));
      try {
        if (options.hooks && options.hooks->before_skill_activation) {
          options.hooks->before_skill_activation(hook_context);
        }
        if (rec.source == SkillActivationSource::User && !skill->manifest.user_invocable) {
          throw ConfigurationError("Skill \"" + rec.name +
                                   "\" is not user-invocable; refusing user-source activation.");
        }
      } catch (const std::exception& error) {
        auto error_context = skill_activation_hook_context(options, rec, *skill, {}, error.what());
        if (options.hooks && options.hooks->on_skill_activation_error) {
          options.hooks->on_skill_activation_error(error_context);
        }
        publish_skill_event(options, "skill.activation.failed", skill_activation_event_payload(error_context));
        throw;
      }

      active_names.insert(rec.name);
      active_records.push_back(std::move(rec));
    };

    // 3. User-source enforcement & unknown-skill drop.
    std::vector<ActivationRecord> active_records;
    active_records.reserve(records.size());
    std::set<std::string> active_names;
    bool any_user_from_parser = !parsed.activations.empty();
    for (auto& rec : records) {
      begin_activation(std::move(rec), active_records, active_names);
    }

    // 4. Auto-select. Treat any User-source activation as the "invoked" skill so
    // auto-selection stays consistent with the legacy slash-command behavior.
    std::string invoked_skill;
    for (const auto& rec : active_records) {
      if (rec.source == SkillActivationSource::User) {
        invoked_skill = rec.name;
        break;
      }
    }
    state.auto_selected_skills = path_selected_skills;
    for (const auto& name : auto_select_skills(registry, active_names, options.input_text, options.model_settings, invoked_skill)) {
      if (std::find(state.auto_selected_skills.begin(), state.auto_selected_skills.end(), name) ==
          state.auto_selected_skills.end()) {
        state.auto_selected_skills.push_back(name);
      }
    }
    for (const auto& name : state.auto_selected_skills) {
      if (active_names.count(name)) continue;
      ActivationRecord rec;
      rec.name = name;
      rec.source = SkillActivationSource::Model;
      rec.priority = 0;
      rec.order = active_records.size();
      rec.auto_selected = true;
      begin_activation(std::move(rec), active_records, active_names);
    }

    // 5. Available catalog message (same as legacy behavior).
    available = registry->model_invocable();
    if (!available.empty()) {
      std::string catalog = "Available Anthropic-style skills";
      Value::Array names;
      for (const auto& skill : available) {
        catalog += "\n- " + skill.manifest.name + ": " + skill.manifest.description;
        if (!skill.manifest.argument_hint.empty()) {
          catalog += " | args: " + skill.manifest.argument_hint;
        }
        names.emplace_back(skill.manifest.name);
      }
      state.available_message = create_message(MessageRole::System, catalog,
                                               Value::object({{"source", "skills-catalog"}, {"skills", Value(names)}}));
    }

    // 6. Render & collect model/effort signals.
    std::vector<std::tuple<std::string, int, std::size_t>> model_entries;
    std::vector<std::tuple<std::string, int, std::size_t>> effort_entries;
    for (const auto& rec : active_records) {
      const auto* skill = registry->get(rec.name);
      if (!skill) continue;
      try {
        SkillRenderContext render_context;
        render_context.arguments_text = rec.arguments_text;
        render_context.session_id = options.session_id;
        render_context.skill_dir = skill->directory;
        const std::string prompt = render_skill_prompt(*skill, render_context);
        state.active_skills.push_back(ResolvedSkillUse{*skill, prompt, rec.arguments_text});
        state.active_messages.push_back(create_message(MessageRole::System, prompt,
                                                       Value::object({{"source", "skill"},
                                                                      {"skill", skill->manifest.name},
                                                                      {"argumentsText", rec.arguments_text},
                                                                      {"activationSource", to_string(rec.source)}})));
        state.allowed_tools.insert(state.allowed_tools.end(), skill->manifest.allowed_tools.begin(),
                                   skill->manifest.allowed_tools.end());
        if (!skill->manifest.model.empty()) {
          model_entries.emplace_back(skill->manifest.model, rec.priority, rec.order);
        }
        if (!skill->manifest.effort.empty()) {
          effort_entries.emplace_back(skill->manifest.effort, rec.priority, rec.order);
        }

        auto hook_context = skill_activation_hook_context(options, rec, *skill, prompt);
        if (options.hooks && options.hooks->after_skill_activation) {
          options.hooks->after_skill_activation(hook_context);
        }
        publish_skill_event(options, "skill.activation.completed", skill_activation_event_payload(hook_context));
      } catch (const std::exception& error) {
        auto error_context = skill_activation_hook_context(options, rec, *skill, {}, error.what());
        if (options.hooks && options.hooks->on_skill_activation_error) {
          options.hooks->on_skill_activation_error(error_context);
        }
        publish_skill_event(options, "skill.activation.failed", skill_activation_event_payload(error_context));
        throw;
      }
    }
    std::sort(state.allowed_tools.begin(), state.allowed_tools.end());
    state.allowed_tools.erase(std::unique(state.allowed_tools.begin(), state.allowed_tools.end()),
                              state.allowed_tools.end());

    // 7. Model conflict resolution (caller's explicit model wins).
    if (state.model_settings.model.empty() && !model_entries.empty()) {
      const std::string winner = resolve_conflict(options.model_conflict, model_entries, "models");
      if (!winner.empty()) {
        state.model_settings.model = winner;
      }
    }

    // 8. Effort conflict resolution (caller's explicit reasoning wins).
    if (!state.model_settings.reasoning && !effort_entries.empty()) {
      const std::string effort = resolve_conflict(options.effort_conflict, effort_entries, "effort");
      if (!effort.empty()) {
        ReasoningSettings reasoning;
        reasoning.budget = effort == "minimal" ? std::string("low")
                          : effort == "max"    ? std::string("high")
                                                : effort;
        state.model_settings.reasoning = reasoning;
      }
    }

    // 9. effective_input_text — only when the parser actually changed the input.
    if (any_user_from_parser && parsed.stripped_input != options.input_text) {
      state.effective_input_text = parsed.stripped_input;
    }

    auto after_context = skills_resolve_hook_context(options, observed_activations, available, state);
    if (options.hooks && options.hooks->after_skills_resolve) {
      options.hooks->after_skills_resolve(after_context);
    }
    publish_skill_event(options, "skills.resolve.completed", skills_resolve_event_payload(after_context));
    return state;
  } catch (const std::exception& error) {
    auto error_context = skills_resolve_hook_context(options, observed_activations, available, state, error.what());
    if (options.hooks && options.hooks->on_skills_resolve_error) {
      options.hooks->on_skills_resolve_error(error_context);
    }
    publish_skill_event(options, "skills.resolve.failed", skills_resolve_event_payload(error_context));
    throw;
  }
}
}  // namespace agent
