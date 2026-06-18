#pragma once

#include "agent/skills_core.hpp"

namespace agent {

SkillDefinition parse_anthropic_skill_markdown(const std::string& markdown, std::string file_path,
                                               std::string source = "custom");
SkillDefinition load_skill_file(const std::filesystem::path& file_path, std::string source = "custom");
std::vector<SkillDefinition> load_skill_directory(const std::filesystem::path& directory,
                                                  std::string source = "custom");
std::vector<std::filesystem::path> collect_project_skill_directories(const std::filesystem::path& cwd);

struct AnthropicSkillRegistryLoadOptions {
  std::filesystem::path cwd = std::filesystem::current_path();
  bool include_user = true;
  std::filesystem::path user_home;
};

SkillRegistry load_anthropic_skill_registry(AnthropicSkillRegistryLoadOptions options = {});

}  // namespace agent
