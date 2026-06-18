#include "agent/skills.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

}  // namespace agent
