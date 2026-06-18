#pragma once

#include "agent/security_governance.hpp"

#include <string>
#include <vector>

namespace agent {

enum class ShellTokenKind {
  Word,
  Operator,
  Redirect,
};

struct ShellToken {
  ShellTokenKind kind = ShellTokenKind::Word;
  std::string text;
  bool quoted = false;
  bool dynamic = false;
};

struct ShellRedirect {
  std::string op;
  std::string target;
};

struct ShellSimpleCommand {
  std::vector<std::string> assignments;
  std::vector<std::string> argv;
  std::vector<ShellRedirect> redirects;
  bool dynamic = false;
};

struct ShellCommandAst {
  std::vector<ShellSimpleCommand> commands;
  std::vector<std::string> operators;
  bool dynamic = false;
  std::vector<std::string> parse_warnings;
};

struct ShellCommandFinding {
  std::string category;
  std::string risk_level = "low";
  std::string reason;
  std::string command;
};

struct ShellCommandClassification {
  ShellCommandAst ast;
  std::vector<PermissionAction> actions;
  std::vector<PermissionResource> resources;
  std::string risk_level = "low";
  std::vector<ShellCommandFinding> findings;
  std::vector<std::string> commands;
};

std::vector<ShellToken> lex_shell_command(const std::string& input,
                                          std::vector<std::string>* warnings = nullptr);
ShellCommandAst parse_shell_command(const std::string& input);
ShellCommandClassification classify_shell_command(const std::string& input);
ShellCommandClassification classify_exec_file_command(const std::string& command,
                                                      const std::vector<std::string>& args = {});

}  // namespace agent
