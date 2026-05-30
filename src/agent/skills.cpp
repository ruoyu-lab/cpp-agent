#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace agent {

namespace {

bool path_exists(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::exists(path, error);
}

bool path_is_directory(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_directory(path, error);
}

std::filesystem::path absolute_normalized_path(const std::filesystem::path& path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    absolute = path;
  }
  return absolute.lexically_normal();
}

std::string unquote_scalar(std::string value) {
  value = trim_copy(std::move(value));
  if ((value.size() >= 2 && value.front() == '"' && value.back() == '"') ||
      (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')) {
    return value.substr(1, value.size() - 2);
  }
  return value;
}

Value parse_frontmatter_scalar_value(std::string value) {
  value = trim_copy(std::move(value));
  if (value.empty()) {
    return "";
  }
  if (value == "true") {
    return true;
  }
  if (value == "false") {
    return false;
  }
  if ((value.size() >= 2 && value.front() == '"' && value.back() == '"') ||
      (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')) {
    return value.substr(1, value.size() - 2);
  }
  if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
    Value::Array values;
    std::string current;
    for (const char ch : value.substr(1, value.size() - 2)) {
      if (ch == ',') {
        const auto item = trim_copy(current);
        if (!item.empty()) {
          values.push_back(parse_frontmatter_scalar_value(item));
        }
        current.clear();
      } else {
        current.push_back(ch);
      }
    }
    const auto item = trim_copy(current);
    if (!item.empty()) {
      values.push_back(parse_frontmatter_scalar_value(item));
    }
    return Value(std::move(values));
  }
  return value;
}

std::vector<std::string> split_comma_list(std::string value) {
  value = trim_copy(std::move(value));
  if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
    value = value.substr(1, value.size() - 2);
  }
  std::vector<std::string> items;
  std::string current;
  for (const char ch : value) {
    if (ch == ',') {
      const auto item = unquote_scalar(current);
      if (!item.empty()) {
        items.push_back(item);
      }
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  const auto item = unquote_scalar(current);
  if (!item.empty()) {
    items.push_back(item);
  }
  return items;
}

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

std::string frontmatter_value_to_string(const Value& value) {
  if (value.is_string()) {
    return value.as_string();
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (value.is_number()) {
    return value.stringify();
  }
  return {};
}

std::vector<std::string> string_array_from_frontmatter_value(const Value& value,
                                                             bool tokenize_tools = false) {
  std::vector<std::string> items;
  if (value.is_array()) {
    for (const auto& item : value.as_array()) {
      const auto text = frontmatter_value_to_string(item);
      if (!text.empty()) {
        items.push_back(text);
      }
    }
    return items;
  }
  if (value.is_string()) {
    return tokenize_tools ? tokenize_skill_arguments(value.as_string())
                          : split_comma_list(value.as_string());
  }
  return items;
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

Value metadata_value_from_frontmatter(const std::map<std::string, Value>& values) {
  static const std::set<std::string> known = {
      "name",
      "description",
      "argumentHint",
      "argument-hint",
      "userInvocable",
      "user-invocable",
      "disableModelInvocation",
      "disable-model-invocation",
      "allowedTools",
      "allowed-tools",
      "tools",
      "model",
      "effort",
      "context",
      "agent",
      "paths",
      "tier",
  };

  Value::Object object;
  for (const auto& [key, value] : values) {
    if (known.contains(key)) {
      continue;
    }
    object[key] = value;
  }
  return Value(std::move(object));
}

std::filesystem::path default_user_home() {
  if (const char* home = std::getenv("HOME"); home && *home) {
    return home;
  }
  if (const char* profile = std::getenv("USERPROFILE"); profile && *profile) {
    return profile;
  }
  return {};
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

SkillDefinition parse_anthropic_skill_markdown(const std::string& markdown, std::string file_path,
                                               std::string source) {
  std::string normalized = markdown;
  std::replace(normalized.begin(), normalized.end(), '\r', '\n');
  std::string frontmatter;
  std::string body = normalized;
  if (normalized.rfind("---\n", 0) == 0) {
    const auto end = normalized.find("\n---\n", 4);
    if (end == std::string::npos) {
      throw ConfigurationError("Skill frontmatter is missing a closing --- line.");
    }
    frontmatter = normalized.substr(4, end - 4);
    body = normalized.substr(end + 5);
  }

  std::map<std::string, std::string> scalars;
  std::map<std::string, std::vector<std::string>> lists;
  std::map<std::string, Value> parsed_values;
  std::istringstream stream(frontmatter);
  std::string line;
  std::string active_list;
  while (std::getline(stream, line)) {
    const std::string trimmed = trim_copy(line);
    if (trimmed.empty() || trimmed[0] == '#') {
      continue;
    }
    if (!active_list.empty() && trimmed.rfind("- ", 0) == 0) {
      lists[active_list].push_back(unquote_scalar(trimmed.substr(2)));
      parsed_values[active_list].as_array().push_back(parse_frontmatter_scalar_value(trimmed.substr(2)));
      continue;
    }
    active_list.clear();
    const auto colon = line.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    const std::string key = trim_copy(line.substr(0, colon));
    std::string value = trim_copy(line.substr(colon + 1));
    if (value.empty()) {
      active_list = key;
      lists[key] = {};
      parsed_values[key] = Value::array({});
      continue;
    }
    scalars[key] = unquote_scalar(value);
    parsed_values[key] = parse_frontmatter_scalar_value(value);
  }

  const std::filesystem::path path(file_path);
  const auto directory = path.parent_path().string();
  const std::string derived_name = path.parent_path().filename().empty()
                                       ? path.stem().string()
                                       : path.parent_path().filename().string();
  const std::string prompt = trim_copy(body);
  if (prompt.empty()) {
    throw ConfigurationError("Skill \"" + file_path + "\" has an empty body.");
  }

  SkillManifest manifest;
  manifest.name = scalars.contains("name") && !trim_copy(scalars["name"]).empty() ? trim_copy(scalars["name"])
                                                                                  : derived_name;
  manifest.description = scalars.contains("description") ? trim_copy(scalars["description"]) : "";
  if (manifest.description.empty()) {
    std::istringstream body_lines(prompt);
    std::getline(body_lines, manifest.description);
    manifest.description = trim_copy(manifest.description);
  }
  manifest.argument_hint = scalars.contains("argumentHint") ? scalars["argumentHint"] : scalars["argument-hint"];
  manifest.user_invocable = parsed_values["userInvocable"].as_bool(false) ||
                             parsed_values["user-invocable"].as_bool(false);
  manifest.disable_model_invocation =
      parsed_values["disableModelInvocation"].as_bool(false) ||
      parsed_values["disable-model-invocation"].as_bool(false);
  if (parsed_values.contains("allowedTools")) {
    manifest.allowed_tools = string_array_from_frontmatter_value(parsed_values["allowedTools"], true);
  } else if (parsed_values.contains("allowed-tools")) {
    manifest.allowed_tools = string_array_from_frontmatter_value(parsed_values["allowed-tools"], true);
  } else if (parsed_values.contains("tools")) {
    manifest.allowed_tools = string_array_from_frontmatter_value(parsed_values["tools"], true);
  }
  manifest.model = scalars["model"];
  manifest.effort = scalars["effort"];
  manifest.context = scalars["context"] == "fork" ? "fork" : "inline";
  manifest.agent = scalars["agent"];
  manifest.paths = parsed_values.contains("paths")
                       ? string_array_from_frontmatter_value(parsed_values["paths"])
                       : std::vector<std::string>{};
  manifest.tier = scalars.contains("tier") ? scalars["tier"] : "core-safe";
  manifest.metadata = metadata_value_from_frontmatter(parsed_values);

  return SkillDefinition{manifest, prompt, std::move(file_path), directory, std::move(source)};
}

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

SkillDefinition load_skill_file(const std::filesystem::path& file_path, std::string source) {
  const auto markdown = bytes_to_text(read_binary_file(file_path));
  return parse_anthropic_skill_markdown(markdown, file_path.string(), std::move(source));
}

std::vector<SkillDefinition> load_skill_directory(const std::filesystem::path& directory,
                                                  std::string source) {
  if (!path_is_directory(directory)) {
    return {};
  }

  std::vector<std::filesystem::directory_entry> entries;
  std::error_code error;
  for (std::filesystem::directory_iterator it(directory, std::filesystem::directory_options::skip_permission_denied,
                                             error), end;
       !error && it != end; it.increment(error)) {
    entries.push_back(*it);
  }
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    return left.path().filename().string() < right.path().filename().string();
  });

  std::vector<SkillDefinition> skills;
  for (const auto& entry : entries) {
    std::error_code entry_error;
    if (!entry.is_directory(entry_error)) {
      continue;
    }
    const auto skill_path = entry.path() / "SKILL.md";
    if (!path_exists(skill_path)) {
      continue;
    }
    skills.push_back(load_skill_file(skill_path, source));
  }
  return skills;
}

std::vector<std::filesystem::path> collect_project_skill_directories(const std::filesystem::path& cwd) {
  std::vector<std::filesystem::path> directories;
  auto current = absolute_normalized_path(cwd.empty() ? std::filesystem::current_path() : cwd);
  while (true) {
    directories.push_back(current / ".claude" / "skills");
    const auto parent = current.parent_path();
    if (parent.empty() || parent == current) {
      break;
    }
    current = parent;
  }
  return directories;
}

SkillRegistry load_anthropic_skill_registry(AnthropicSkillRegistryLoadOptions options) {
  SkillRegistry registry;
  for (const auto& directory : collect_project_skill_directories(options.cwd)) {
    for (auto& skill : load_skill_directory(directory, "project")) {
      if (!registry.has(skill.manifest.name)) {
        registry.register_skill(std::move(skill));
      }
    }
  }

  if (options.include_user) {
    const auto home = options.user_home.empty() ? default_user_home() : options.user_home;
    if (!home.empty()) {
      for (auto& skill : load_skill_directory(home / ".claude" / "skills", "user")) {
        registry.register_skill(std::move(skill));
      }
    }
  }
  return registry;
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
};

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
  if (!registry) {
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

  // 3. User-source enforcement & unknown-skill drop.
  std::vector<ActivationRecord> active_records;
  active_records.reserve(records.size());
  std::set<std::string> active_names;
  bool any_user_from_parser = !parsed.activations.empty();
  for (auto& rec : records) {
    const auto* skill = registry->get(rec.name);
    if (!skill) {
      continue;  // unknown — drop silently
    }
    if (rec.source == SkillActivationSource::User && !skill->manifest.user_invocable) {
      throw ConfigurationError("Skill \"" + rec.name +
                               "\" is not user-invocable; refusing user-source activation.");
    }
    active_records.push_back(std::move(rec));
    active_names.insert(active_records.back().name);
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
  state.auto_selected_skills =
      auto_select_skills(registry, active_names, options.input_text, options.model_settings, invoked_skill);
  for (const auto& name : state.auto_selected_skills) {
    if (active_names.count(name)) continue;
    ActivationRecord rec;
    rec.name = name;
    rec.source = SkillActivationSource::Model;
    rec.priority = 0;
    rec.order = active_records.size();
    active_records.push_back(std::move(rec));
    active_names.insert(name);
  }

  // 5. Available catalog message (same as legacy behavior).
  const auto available = registry->model_invocable();
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

  return state;
}
}  // namespace agent
