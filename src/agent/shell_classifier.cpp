#include "agent/shell_classifier.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <sstream>

namespace agent {

namespace {

int risk_value(const std::string& risk) {
  if (risk == "high") return 2;
  if (risk == "medium") return 1;
  return 0;
}

std::string max_risk(const std::string& left, const std::string& right) {
  return risk_value(left) >= risk_value(right) ? left : right;
}

template <typename T>
bool contains(const std::vector<T>& values, const T& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

template <typename T>
void push_unique(std::vector<T>& target, const T& value) {
  if (!contains(target, value)) {
    target.push_back(value);
  }
}

bool is_operator(const std::string& text) {
  return text == "&&" || text == "||" || text == ";" || text == "|";
}

bool is_redirect(const std::string& text) {
  return text == ">" || text == ">>" || text == "<" || text == "2>" ||
         text == "2>>" || text == "&>";
}

bool is_assignment(const std::string& text) {
  if (text.empty() || !(std::isalpha(static_cast<unsigned char>(text[0])) || text[0] == '_')) {
    return false;
  }
  for (std::size_t index = 1; index < text.size(); ++index) {
    const auto ch = static_cast<unsigned char>(text[index]);
    if (text[index] == '=') {
      return true;
    }
    if (!(std::isalnum(ch) || text[index] == '_')) {
      return false;
    }
  }
  return false;
}

bool is_likely_path(const std::string& text) {
  return text == "." || text == ".." || text.rfind("/", 0) == 0 ||
         text.rfind("./", 0) == 0 || text.rfind("../", 0) == 0 ||
         text.find('/') != std::string::npos;
}

std::string host_from_token(const std::string& text) {
  const auto scheme = text.find("://");
  if (scheme == std::string::npos) {
    return {};
  }
  auto start = scheme + 3;
  auto end = text.find_first_of("/:?#", start);
  if (end == std::string::npos) {
    end = text.size();
  }
  return text.substr(start, end - start);
}

PermissionResource process_resource(const ShellSimpleCommand& command) {
  PermissionResource resource;
  resource.kind = PermissionResourceKind::Process;
  if (!command.argv.empty()) {
    resource.id = command.argv.front();
    resource.boundary.command = command.argv.front();
  }
  resource.actions = {PermissionAction::ProcessExecute};
  resource.source = PermissionDecisionSource::Classifier;
  return resource;
}

std::vector<PermissionResource> path_resources(const ShellSimpleCommand& command,
                                               PermissionAction action) {
  std::vector<PermissionResource> resources;
  for (std::size_t index = 1; index < command.argv.size(); ++index) {
    const auto& arg = command.argv[index];
    if (!arg.empty() && arg[0] != '-' && is_likely_path(arg)) {
      PermissionResource resource;
      resource.kind = PermissionResourceKind::Filesystem;
      resource.id = arg;
      resource.actions = {action};
      resource.boundary.path = arg;
      resource.source = PermissionDecisionSource::Classifier;
      resources.push_back(std::move(resource));
    }
  }
  return resources;
}

ShellCommandFinding finding(std::string category,
                            std::string risk_level,
                            std::string reason,
                            std::string command = {}) {
  ShellCommandFinding out;
  out.category = std::move(category);
  out.risk_level = std::move(risk_level);
  out.reason = std::move(reason);
  out.command = std::move(command);
  return out;
}

struct SemanticResult {
  std::vector<PermissionAction> actions;
  std::vector<PermissionResource> resources;
  std::string risk_level = "low";
  std::vector<ShellCommandFinding> findings;
};

SemanticResult base_semantic(const ShellSimpleCommand& command,
                             std::string category,
                             std::string risk_level,
                             std::string reason,
                             std::vector<PermissionAction> extra_actions = {},
                             std::vector<PermissionResource> extra_resources = {}) {
  SemanticResult result;
  result.actions = {PermissionAction::ProcessExecute};
  for (const auto action : extra_actions) {
    push_unique(result.actions, action);
  }
  result.resources.push_back(process_resource(command));
  for (auto& resource : extra_resources) {
    result.resources.push_back(std::move(resource));
  }
  result.risk_level = risk_level;
  result.findings.push_back(finding(std::move(category), std::move(risk_level),
                                    std::move(reason),
                                    command.argv.empty() ? std::string{} : command.argv.front()));
  return result;
}

SemanticResult classify_readonly(const ShellSimpleCommand& command) {
  return base_semantic(command, "inspection", "low",
                       "Read-only shell inspection command.",
                       {PermissionAction::FilesystemRead},
                       path_resources(command, PermissionAction::FilesystemRead));
}

SemanticResult classify_write(const ShellSimpleCommand& command) {
  return base_semantic(command, "filesystem-write", "medium",
                       "Shell command can create or modify filesystem state.",
                       {PermissionAction::FilesystemWrite},
                       path_resources(command, PermissionAction::FilesystemWrite));
}

SemanticResult classify_delete(const ShellSimpleCommand& command) {
  return base_semantic(command, "filesystem-delete", "high",
                       "Shell command can delete filesystem state.",
                       {PermissionAction::FilesystemDelete},
                       path_resources(command, PermissionAction::FilesystemDelete));
}

SemanticResult classify_network(const ShellSimpleCommand& command) {
  auto result = base_semantic(command, "network", "medium",
                              "Shell command can reach network resources.",
                              {PermissionAction::NetworkConnect});
  for (std::size_t index = 1; index < command.argv.size(); ++index) {
    const auto host = host_from_token(command.argv[index]);
    if (!host.empty()) {
      PermissionResource resource;
      resource.kind = PermissionResourceKind::Network;
      resource.id = host;
      resource.actions = {PermissionAction::NetworkConnect};
      resource.boundary.host = host;
      resource.source = PermissionDecisionSource::Classifier;
      result.resources.push_back(std::move(resource));
    }
  }
  return result;
}

SemanticResult classify_git(const ShellSimpleCommand& command) {
  static const std::set<std::string> read = {"status", "log", "show", "diff", "branch", "rev-parse", "ls-files"};
  static const std::set<std::string> network = {"fetch", "pull", "push", "clone"};
  static const std::set<std::string> write = {"add", "commit", "checkout", "switch", "restore", "reset", "clean", "merge", "rebase", "tag"};
  std::string subcommand;
  for (std::size_t index = 1; index < command.argv.size(); ++index) {
    if (!command.argv[index].empty() && command.argv[index][0] != '-') {
      subcommand = command.argv[index];
      break;
    }
  }
  if (!subcommand.empty() && read.count(subcommand) > 0) {
    return base_semantic(command, "inspection", "low",
                         "git " + subcommand + " is read-only repository inspection.",
                         {PermissionAction::RepositoryRead, PermissionAction::FilesystemRead},
                         path_resources(command, PermissionAction::FilesystemRead));
  }
  if (!subcommand.empty() && network.count(subcommand) > 0) {
    auto resources = path_resources(command, PermissionAction::FilesystemWrite);
    for (std::size_t index = 2; index < command.argv.size(); ++index) {
      const auto host = host_from_token(command.argv[index]);
      if (!host.empty()) {
        PermissionResource resource;
        resource.kind = PermissionResourceKind::Network;
        resource.id = host;
        resource.actions = {PermissionAction::NetworkConnect};
        resource.boundary.host = host;
        resource.source = PermissionDecisionSource::Classifier;
        resources.push_back(std::move(resource));
      }
    }
    return base_semantic(command, "network", "high",
                         "git " + subcommand + " can mutate repository state or contact remotes.",
                         {PermissionAction::RepositoryWrite, PermissionAction::FilesystemWrite,
                          PermissionAction::NetworkConnect},
                         std::move(resources));
  }
  if (!subcommand.empty() && write.count(subcommand) > 0) {
    const auto risk = (subcommand == "clean" || subcommand == "reset") ? "high" : "medium";
    return base_semantic(command, "filesystem-write", risk,
                         "git " + subcommand + " can mutate repository state.",
                         {PermissionAction::RepositoryWrite, PermissionAction::FilesystemWrite},
                         path_resources(command, PermissionAction::FilesystemWrite));
  }
  return base_semantic(command, "unknown", "medium",
                       "git subcommand is not categorized by the shell classifier.",
                       {PermissionAction::RepositoryWrite},
                       path_resources(command, PermissionAction::FilesystemWrite));
}

using SemanticFn = SemanticResult (*)(const ShellSimpleCommand&);

std::map<std::string, SemanticFn> build_semantics() {
  std::map<std::string, SemanticFn> out;
  for (const auto& name : {"pwd", "ls", "cat", "head", "tail", "grep", "rg", "find", "wc", "du", "df", "stat", "file", "sed", "awk", "sort", "uniq"}) {
    out[name] = classify_readonly;
  }
  for (const auto& name : {"touch", "mkdir", "cp", "mv", "tee", "truncate"}) {
    out[name] = classify_write;
  }
  for (const auto& name : {"rm", "rmdir", "shred"}) {
    out[name] = classify_delete;
  }
  for (const auto& name : {"curl", "wget", "ssh", "scp", "rsync", "nc", "telnet"}) {
    out[name] = classify_network;
  }
  out["git"] = classify_git;
  return out;
}

SemanticResult classify_special(const ShellSimpleCommand& command) {
  if (command.argv.empty()) {
    return {};
  }
  const auto& name = command.argv.front();
  static const std::set<std::string> package_managers = {
      "npm", "yarn", "pnpm", "pip", "pip3", "uv", "cargo", "go", "brew", "apt", "apt-get", "yum", "apk"};
  static const std::set<std::string> privilege = {"sudo", "su", "doas"};
  static const std::set<std::string> process_control = {"kill", "pkill", "killall", "launchctl", "systemctl", "service"};
  static const std::set<std::string> interpreters = {"sh", "bash", "zsh", "fish", "python", "python3", "node", "perl", "ruby"};

  if (package_managers.count(name) > 0) {
    return base_semantic(command, "package-manager", "high",
                         "Package manager commands can execute install scripts or mutate dependencies.",
                         {PermissionAction::FilesystemWrite, PermissionAction::NetworkConnect},
                         path_resources(command, PermissionAction::FilesystemWrite));
  }
  if (privilege.count(name) > 0) {
    return base_semantic(command, "privilege", "high", "Privilege escalation command.");
  }
  if (process_control.count(name) > 0) {
    return base_semantic(command, "process-control", "high", "Process or service control command.");
  }
  if (interpreters.count(name) > 0) {
    const bool inline_eval = std::find(command.argv.begin(), command.argv.end(), "-c") != command.argv.end() ||
                             std::find(command.argv.begin(), command.argv.end(), "-e") != command.argv.end();
    return base_semantic(command, "dynamic-shell", inline_eval ? "high" : "medium",
                         inline_eval ? "Interpreter inline execution is dynamic shell behavior."
                                     : "Interpreter command needs host review unless arguments constrain it.");
  }
  return base_semantic(command, "unknown", "medium",
                       "Command is not categorized by the shell classifier.");
}

}  // namespace

std::vector<ShellToken> lex_shell_command(const std::string& input,
                                          std::vector<std::string>* warnings) {
  std::vector<ShellToken> tokens;
  std::string current;
  char quote = '\0';
  bool quoted = false;
  bool dynamic = false;

  auto flush_word = [&]() {
    if (current.empty()) {
      return;
    }
    tokens.push_back(ShellToken{ShellTokenKind::Word, current, quoted, dynamic});
    current.clear();
    quoted = false;
    dynamic = false;
  };

  for (std::size_t index = 0; index < input.size(); ++index) {
    const char ch = input[index];
    const char next = index + 1 < input.size() ? input[index + 1] : '\0';

    if (quote != '\0') {
      if (ch == quote) {
        quote = '\0';
        quoted = true;
        continue;
      }
      if (ch == '\\' && quote == '"' && next != '\0') {
        current.push_back(next);
        ++index;
        continue;
      }
      if (quote == '"' && (ch == '$' || ch == '`')) {
        dynamic = true;
      }
      current.push_back(ch);
      continue;
    }

    if (std::isspace(static_cast<unsigned char>(ch))) {
      flush_word();
      continue;
    }
    if (ch == '\'' || ch == '"') {
      quote = ch;
      quoted = true;
      continue;
    }
    if (ch == '\\' && next != '\0') {
      current.push_back(next);
      ++index;
      continue;
    }

    const std::string two = std::string() + ch + next;
    if (is_operator(two) || is_redirect(two)) {
      flush_word();
      tokens.push_back(ShellToken{is_operator(two) ? ShellTokenKind::Operator : ShellTokenKind::Redirect,
                                  two, false, false});
      ++index;
      continue;
    }
    const std::string one(1, ch);
    if (is_operator(one) || is_redirect(one)) {
      flush_word();
      tokens.push_back(ShellToken{is_operator(one) ? ShellTokenKind::Operator : ShellTokenKind::Redirect,
                                  one, false, false});
      continue;
    }
    if (ch == '$' || ch == '`') {
      dynamic = true;
    }
    current.push_back(ch);
  }

  if (quote != '\0' && warnings) {
    warnings->push_back("unterminated quote");
  }
  flush_word();
  return tokens;
}

ShellCommandAst parse_shell_command(const std::string& input) {
  ShellCommandAst ast;
  auto tokens = lex_shell_command(input, &ast.parse_warnings);
  ShellSimpleCommand current;
  auto flush_command = [&]() {
    if (!current.argv.empty() || !current.assignments.empty() || !current.redirects.empty()) {
      ast.commands.push_back(current);
    }
    current = ShellSimpleCommand{};
  };

  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    if (token.kind == ShellTokenKind::Operator) {
      flush_command();
      ast.operators.push_back(token.text);
      continue;
    }
    if (token.kind == ShellTokenKind::Redirect) {
      if (index + 1 >= tokens.size() || tokens[index + 1].kind != ShellTokenKind::Word) {
        ast.parse_warnings.push_back("redirect " + token.text + " is missing a target");
        current.redirects.push_back(ShellRedirect{token.text, {}});
      } else {
        current.redirects.push_back(ShellRedirect{token.text, tokens[index + 1].text});
        current.dynamic = current.dynamic || tokens[index + 1].dynamic;
        ++index;
      }
      continue;
    }
    if (current.argv.empty() && is_assignment(token.text)) {
      current.assignments.push_back(token.text);
    } else {
      current.argv.push_back(token.text);
    }
    current.dynamic = current.dynamic || token.dynamic;
  }
  flush_command();
  ast.dynamic = std::any_of(ast.commands.begin(), ast.commands.end(), [](const ShellSimpleCommand& command) {
    return command.dynamic;
  });
  return ast;
}

ShellCommandClassification classify_shell_command(const std::string& input) {
  static const auto semantics = build_semantics();
  ShellCommandClassification classification;
  classification.ast = parse_shell_command(input);

  for (const auto& command : classification.ast.commands) {
    if (command.argv.empty()) {
      continue;
    }
    const auto& command_name = command.argv.front();
    classification.commands.push_back(command_name);
    auto semantic = semantics.find(command_name);
    auto result = semantic != semantics.end() ? semantic->second(command) : classify_special(command);
    for (const auto action : result.actions) {
      push_unique(classification.actions, action);
    }
    for (auto& resource : result.resources) {
      classification.resources.push_back(std::move(resource));
    }
    for (auto& shell_finding : result.findings) {
      classification.findings.push_back(std::move(shell_finding));
    }
    classification.risk_level = max_risk(classification.risk_level, result.risk_level);

    for (const auto& redirect : command.redirects) {
      if (redirect.target.empty()) {
        continue;
      }
      if (redirect.op.find('>') != std::string::npos) {
        push_unique(classification.actions, PermissionAction::FilesystemWrite);
        PermissionResource resource;
        resource.kind = PermissionResourceKind::Filesystem;
        resource.id = redirect.target;
        resource.actions = {PermissionAction::FilesystemWrite};
        resource.boundary.path = redirect.target;
        resource.source = PermissionDecisionSource::Classifier;
        classification.resources.push_back(std::move(resource));
        classification.findings.push_back(finding("filesystem-write", "medium",
                                                  "Output redirection writes a filesystem path.",
                                                  command_name));
        classification.risk_level = max_risk(classification.risk_level, "medium");
      } else if (redirect.op.find('<') != std::string::npos) {
        push_unique(classification.actions, PermissionAction::FilesystemRead);
        PermissionResource resource;
        resource.kind = PermissionResourceKind::Filesystem;
        resource.id = redirect.target;
        resource.actions = {PermissionAction::FilesystemRead};
        resource.boundary.path = redirect.target;
        resource.source = PermissionDecisionSource::Classifier;
        classification.resources.push_back(std::move(resource));
      }
    }

    for (std::size_t index = 1; index < command.argv.size(); ++index) {
      const auto host = host_from_token(command.argv[index]);
      if (!host.empty()) {
        push_unique(classification.actions, PermissionAction::NetworkConnect);
        PermissionResource resource;
        resource.kind = PermissionResourceKind::Network;
        resource.id = host;
        resource.actions = {PermissionAction::NetworkConnect};
        resource.boundary.host = host;
        resource.source = PermissionDecisionSource::Classifier;
        classification.resources.push_back(std::move(resource));
        classification.findings.push_back(finding("network", "medium",
                                                  "URL argument references a network host.",
                                                  command_name));
        classification.risk_level = max_risk(classification.risk_level, "medium");
      }
    }
  }

  if (classification.ast.dynamic || !classification.ast.parse_warnings.empty()) {
    classification.risk_level = "high";
    classification.findings.push_back(finding(
        "dynamic-shell", "high",
        !classification.ast.parse_warnings.empty()
            ? "Shell parser warning: " + classification.ast.parse_warnings.front() + "."
            : "Command contains shell expansion or dynamic substitution."));
  }

  if (classification.actions.empty()) {
    classification.actions.push_back(PermissionAction::ProcessExecute);
  }
  return classification;
}

ShellCommandClassification classify_exec_file_command(const std::string& command,
                                                      const std::vector<std::string>& args) {
  std::ostringstream joined;
  joined << command;
  for (const auto& arg : args) {
    joined << ' ';
    for (const auto ch : arg) {
      if (ch == '"' || ch == '\\' || ch == '$' || ch == '`') {
        joined << '\\';
      }
      joined << ch;
    }
  }
  return classify_shell_command(joined.str());
}

}  // namespace agent
