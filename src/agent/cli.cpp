#include "agent/agent.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <sstream>

namespace agent {

namespace {

std::string cli_env_value(const char* key, std::string fallback = {}) {
  if (const char* value = std::getenv(key)) {
    return value;
  }
  return fallback;
}

struct NativeCliOptions {
  std::string command = "chat";
  std::string replay_action = "list";
  std::filesystem::path config_path;
  std::string agent_id;
  std::string provider = "ollama";
  std::string model = cli_env_value("OLLAMA_MODEL", "gemma4:e4b");
  std::string system_prompt;
  std::string session_id = "cli";
  std::filesystem::path sessions_dir;
  std::vector<std::string> sources;
  std::string prompt;
  std::filesystem::path suite_path;
  std::filesystem::path report_path;
  std::filesystem::path markdown_report_path;
  std::filesystem::path baseline_path;
  std::filesystem::path replay_dir;
  std::filesystem::path run_path;
  std::vector<std::string> tags;
  std::vector<std::string> case_ids;
  long long top_k = 4;
  double min_score = 0.15;
  bool update_baseline = false;
  bool stop_on_failure = false;
  bool stream = true;
  bool debug = false;
  bool help = false;
  bool version = false;
  NativeConfigModuleLoader config_module_loader;
};

std::string cli_help_text() {
  return
      "Usage: native-agent [chat] [--config <file> | --provider <name> --model <name>] [options] [prompt]\n"
      "       native-agent eval --config <file> --suite <file> [options]\n"
      "       native-agent replay <list|show> [options]\n"
      "       native-agent validate --config <file> [--agent <id>]\n"
      "\n"
      "Options:\n"
      "  --config <file>         Load native agent config JSON\n"
      "  --agent <id>            Select an agent from config\n"
      "  --provider <name>       Model provider for manual mode (echo/ollama/openai/gemini/anthropic/qwen/mimo/deepseek/llamacpp-native)\n"
      "  --model <name>          Model name for manual mode\n"
      "  --system <text>         System prompt for manual mode\n"
      "  --session <id>          Session id (default: cli)\n"
      "  --sessions-dir <path>   Persist manual-mode session snapshots to a directory\n"
      "  --source <path|url>     Add a manual-mode knowledge source (repeatable)\n"
      "  --top-k <number>        Manual-mode knowledge retrieval topK (default: 4)\n"
      "  --min-score <number>    Manual-mode knowledge retrieval min score (default: 0.15)\n"
      "  --no-stream             Disable streaming output for chat/REPL\n"
      "  --debug                 Print retrieval debug output in chat/REPL\n"
      "  --suite <file>          Eval suite JSON\n"
      "  --report <file>         Write JSON eval report\n"
      "  --markdown-report <f>   Write Markdown eval report\n"
      "  --baseline <file>       Compare or update an eval baseline\n"
      "  --update-baseline       Overwrite baseline with current eval report\n"
      "  --stop-on-failure       Stop eval execution on first failure\n"
      "  --tag <name>            Filter eval cases by tag (repeatable)\n"
      "  --case <id>             Filter eval cases by id (repeatable)\n"
      "  --replay-dir <path>     Replay artifact base directory\n"
      "  --run <path>            Replay run directory for replay show\n"
      "  --version               Show version\n"
      "  --help                  Show this help\n"
      "\n"
      "REPL commands:\n"
      "  :exit | :quit           Exit the REPL\n"
      "  :clear                  Clear current session\n"
      "  :debug on|off           Toggle retrieval debug output\n";
}

bool needs_value(const std::string& flag, int index, const std::vector<std::string>& args) {
  if (index + 1 < static_cast<int>(args.size()) && !args[index + 1].empty()) {
    return true;
  }
  throw ConfigurationError("Missing value for " + flag + ".");
}

std::string read_option_value(const std::string& flag, int& index, const std::vector<std::string>& args) {
  (void)needs_value(flag, index, args);
  index += 1;
  return args[index];
}

bool is_supported_manual_provider(const std::string& provider) {
  return provider == "echo" || provider == "ollama" || provider == "openai" ||
         provider == "gemini" || provider == "anthropic" || provider == "qwen" ||
         provider == "mimo" || provider == "deepseek" || provider == "llamacpp-native";
}

std::string parse_manual_provider(const std::string& provider) {
  if (is_supported_manual_provider(provider)) {
    return provider;
  }
  throw ConfigurationError("Unsupported provider \"" + provider +
                           "\". Expected one of: echo, ollama, openai, gemini, anthropic, qwen, mimo, deepseek, llamacpp-native.");
}

long long parse_integer_option(const std::string& flag, const std::string& raw) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stoll(raw, &consumed);
    if (consumed == raw.size()) {
      return value;
    }
  } catch (...) {
  }
  throw ConfigurationError("Invalid integer for " + flag + ": " + raw);
}

double parse_number_option(const std::string& flag, const std::string& raw) {
  try {
    std::size_t consumed = 0;
    const auto value = std::stod(raw, &consumed);
    if (consumed == raw.size()) {
      return value;
    }
  } catch (...) {
  }
  throw ConfigurationError("Invalid number for " + flag + ": " + raw);
}

NativeCliOptions parse_cli_options(const std::vector<std::string>& args) {
  NativeCliOptions options;
  std::vector<std::string> positional;
  int index = 0;
  if (!args.empty()) {
    if (args[0] == "eval" || args[0] == "replay" || args[0] == "validate" || args[0] == "chat") {
      options.command = args[0];
      index = 1;
      if (options.command == "replay" && index < static_cast<int>(args.size()) &&
          (args[index] == "list" || args[index] == "show")) {
        options.replay_action = args[index];
        index += 1;
      }
    } else if (args[0] == "config" && args.size() > 1 && args[1] == "validate") {
      options.command = "validate";
      index = 2;
    }
  }

  for (; index < static_cast<int>(args.size()); ++index) {
    const auto& arg = args[index];
    if (arg == "--help" || arg == "-h") {
      options.help = true;
    } else if (arg == "--version") {
      options.version = true;
    } else if (arg == "--config") {
      options.config_path = read_option_value(arg, index, args);
    } else if (arg == "--agent") {
      options.agent_id = read_option_value(arg, index, args);
    } else if (arg == "--provider") {
      options.provider = parse_manual_provider(read_option_value(arg, index, args));
    } else if (arg == "--model") {
      options.model = read_option_value(arg, index, args);
    } else if (arg == "--system") {
      options.system_prompt = read_option_value(arg, index, args);
    } else if (arg == "--session") {
      options.session_id = read_option_value(arg, index, args);
    } else if (arg == "--sessions-dir") {
      options.sessions_dir = read_option_value(arg, index, args);
    } else if (arg == "--source") {
      options.sources.push_back(read_option_value(arg, index, args));
    } else if (arg == "--top-k") {
      options.top_k = parse_integer_option(arg, read_option_value(arg, index, args));
    } else if (arg == "--min-score") {
      options.min_score = parse_number_option(arg, read_option_value(arg, index, args));
    } else if (arg == "--suite") {
      options.suite_path = read_option_value(arg, index, args);
    } else if (arg == "--report") {
      options.report_path = read_option_value(arg, index, args);
    } else if (arg == "--markdown-report") {
      options.markdown_report_path = read_option_value(arg, index, args);
    } else if (arg == "--baseline") {
      options.baseline_path = read_option_value(arg, index, args);
    } else if (arg == "--replay-dir") {
      options.replay_dir = read_option_value(arg, index, args);
    } else if (arg == "--run") {
      options.run_path = read_option_value(arg, index, args);
    } else if (arg == "--tag") {
      options.tags.push_back(read_option_value(arg, index, args));
    } else if (arg == "--case") {
      options.case_ids.push_back(read_option_value(arg, index, args));
    } else if (arg == "--update-baseline") {
      options.update_baseline = true;
    } else if (arg == "--stop-on-failure") {
      options.stop_on_failure = true;
    } else if (arg == "--no-stream") {
      options.stream = false;
    } else if (arg == "--stream") {
      options.stream = true;
    } else if (arg == "--debug") {
      options.debug = true;
    } else if (!arg.empty() && arg.front() == '-') {
      throw ConfigurationError("Unknown option: " + arg);
    } else {
      positional.push_back(arg);
    }
  }

  if (!positional.empty()) {
    std::ostringstream prompt;
    for (std::size_t item = 0; item < positional.size(); ++item) {
      if (item > 0) {
        prompt << ' ';
      }
      prompt << positional[item];
    }
    options.prompt = prompt.str();
  }
  return options;
}

std::filesystem::path default_replay_dir() {
  return std::filesystem::current_path() / ".node-agent" / "runs";
}

bool has_http_prefix(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0;
}

bool has_github_repository_prefix(const std::string& value) {
  std::string lower = value;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return lower.rfind("https://github.com/", 0) == 0 || lower.rfind("http://github.com/", 0) == 0;
}

Value manual_model_config(const NativeCliOptions& options) {
  Value model = Value::object({{"provider", options.provider}, {"model", options.model}});
  if (options.provider == "ollama") {
    model["baseUrlEnv"] = "OLLAMA_BASE_URL";
  } else if (options.provider == "openai") {
    model["apiKeyEnv"] = "OPENAI_API_KEY";
    model["baseUrlEnv"] = "OPENAI_BASE_URL";
  } else if (options.provider == "gemini") {
    model["apiKeyEnv"] = "GEMINI_API_KEY";
    model["baseUrlEnv"] = "GEMINI_BASE_URL";
  } else if (options.provider == "anthropic") {
    model["apiKeyEnv"] = "ANTHROPIC_API_KEY";
    model["baseUrlEnv"] = "ANTHROPIC_BASE_URL";
  } else if (options.provider == "qwen") {
    model["apiKeyEnv"] = "QWEN_API_KEY";
    model["baseUrlEnv"] = "QWEN_BASE_URL";
  } else if (options.provider == "mimo") {
    model["apiKeyEnv"] = "MIMO_API_KEY";
    model["baseUrlEnv"] = "MIMO_BASE_URL";
  } else if (options.provider == "deepseek") {
    model["apiKeyEnv"] = "DEEPSEEK_API_KEY";
    model["baseUrlEnv"] = "DEEPSEEK_BASE_URL";
  } else if (options.provider == "llamacpp-native") {
    model["modelPathEnv"] = "LLAMA_CPP_MODEL_PATH";
    model["libraryPathEnv"] = "LLAMA_CPP_LIBRARY_PATH";
    model["libraryDirEnv"] = "LLAMA_CPP_LIBRARY_DIR";
    model["mmprojPathEnv"] = "LLAMA_CPP_MMPROJ_PATH";
    model["mtmdLibraryPathEnv"] = "LLAMA_CPP_MTMD_LIBRARY_PATH";
    model["mtmdLibraryDirEnv"] = "LLAMA_CPP_MTMD_LIBRARY_DIR";
  }
  return model;
}

Value manual_source_config(const std::string& input) {
  if (has_github_repository_prefix(input)) {
    return Value::object({{"type", "github"}, {"url", input}});
  }
  if (has_http_prefix(input)) {
    return Value::object({{"type", "web"}, {"url", input}});
  }

  const std::filesystem::path path(input);
  if (!std::filesystem::exists(path)) {
    throw ConfigurationError("Knowledge source does not exist: " + input);
  }
  if (std::filesystem::is_directory(path)) {
    return Value::object({{"type", "repository"}, {"path", path.string()}});
  }
  return Value::object({{"type", "file"}, {"path", path.string()}});
}

bool source_requires_web_runtime(const Value& source) {
  const auto type = source.at("type").as_string();
  return type == "web" || type == "github" || type == "sitemap" || type == "website";
}

Value manual_web_config(bool force_fetcher) {
  Value::Array search;
  const auto searxng_base_url = cli_env_value("SEARXNG_BASE_URL");
  if (!searxng_base_url.empty()) {
    search.push_back(Value::object({
        {"kind", "searxng"},
        {"baseUrl", searxng_base_url},
    }));
  }
  const auto brave_api_key = cli_env_value("BRAVE_SEARCH_API_KEY");
  if (!brave_api_key.empty()) {
    Value brave = Value::object({
        {"kind", "brave"},
        {"apiKey", brave_api_key},
    });
    const auto brave_base_url = cli_env_value("BRAVE_SEARCH_BASE_URL");
    if (!brave_base_url.empty()) {
      brave["baseUrl"] = brave_base_url;
    }
    search.push_back(std::move(brave));
  }
  if (search.empty() && !force_fetcher) {
    return Value();
  }
  Value web = Value::object({{"fetcher", Value::object({})}});
  if (!search.empty()) {
    web["search"] = Value(std::move(search));
  }
  return web;
}

NativeResolvedAgentApp create_manual_agent_app(const NativeCliOptions& options) {
  Value::Array bundles = {"core", "agent"};
  Value agent_definition = Value::object({
      {"model", manual_model_config(options)},
      {"skills", Value::object({{"anthropic", Value::object({{"cwd", "."}})}})},
      {"mcp", Value::object({{"anthropic", Value::object({{"cwd", "."}})}})},
  });
  if (!options.system_prompt.empty()) {
    agent_definition["systemPrompt"] = options.system_prompt;
  }
  if (!options.sessions_dir.empty()) {
    agent_definition["memory"] = Value::object({
        {"session", Value::object({{"kind", "file"}, {"baseDir", options.sessions_dir.string()}})},
    });
  }

  bool needs_web_runtime = false;
  Value::Array sources;
  for (const auto& source : options.sources) {
    auto resolved = manual_source_config(source);
    needs_web_runtime = needs_web_runtime || source_requires_web_runtime(resolved);
    sources.push_back(std::move(resolved));
  }
  if (!sources.empty()) {
    agent_definition["knowledge"] = Value::object({
        {"base", Value::object({
                     {"id", "cli-kb"},
                     {"title", "CLI Knowledge Base"},
                     {"chunkSize", 600},
                     {"chunkOverlap", 80},
                     {"sources", Value(std::move(sources))},
                 })},
        {"retrievalOptions", Value::object({
                                 {"enabled", true},
                                 {"topK", options.top_k},
                                 {"minScore", options.min_score},
                             })},
    });
  }
  Value web_config = manual_web_config(needs_web_runtime);
  if (web_config.is_object()) {
    if (!web_config.at("search").as_array().empty()) {
      bundles.push_back("web");
    }
    agent_definition["web"] = std::move(web_config);
  }
  agent_definition["tools"] = Value::object({{"bundles", Value(std::move(bundles))}});

  Value config = Value::object({
      {"_cwd", std::filesystem::current_path().string()},
      {"defaultAgent", "cli"},
      {"agents", Value::object({{"cli", agent_definition}})},
  });
  return resolve_native_agent_app(config, "cli");
}

void require_config_path(const NativeCliOptions& options) {
  if (options.config_path.empty()) {
    throw ConfigurationError("Command requires --config <file>.");
  }
}

void render_cli_debug(const AgentRunnerRunResult& result, std::ostream& out) {
  if (result.memory_hits.empty() && result.knowledge_hits.empty()) {
    out << "No retrieval debug info.\n";
    return;
  }
  out << "Knowledge Debug\n";
  if (!result.knowledge_hits.empty()) {
    out << "- knowledge hits: " << result.knowledge_hits.size() << "\n";
    for (const auto& hit : result.knowledge_hits) {
      out << "- " << hit.document.id << " score=" << hit.score;
      if (!hit.document.title.empty()) {
        out << " title=" << hit.document.title;
      }
      out << "\n";
    }
  }
  if (result.memory_hits.empty()) {
    return;
  }
  out << "- memory hits: " << result.memory_hits.size() << "\n";
  for (const auto& hit : result.memory_hits) {
    out << "- " << hit.id << " score=" << hit.score;
    if (!hit.namespace_id.empty()) {
      out << " namespace=" << hit.namespace_id;
    }
    out << "\n";
  }
}

void render_cli_retrieval_debug(const std::vector<RetrievedMemory>& memory_hits,
                                const std::vector<KnowledgeSearchHit>& knowledge_hits,
                                std::ostream& out) {
  if (memory_hits.empty() && knowledge_hits.empty()) {
    out << "No retrieval debug info.\n";
    return;
  }
  out << "Knowledge Debug\n";
  if (!knowledge_hits.empty()) {
    out << "- knowledge hits: " << knowledge_hits.size() << "\n";
    for (const auto& hit : knowledge_hits) {
      out << "- " << hit.document.id << " score=" << hit.score;
      if (!hit.document.title.empty()) {
        out << " title=" << hit.document.title;
      }
      out << "\n";
    }
  }
  if (memory_hits.empty()) {
    return;
  }
  out << "- memory hits: " << memory_hits.size() << "\n";
  for (const auto& hit : memory_hits) {
    out << "- " << hit.id << " score=" << hit.score;
    if (!hit.namespace_id.empty()) {
      out << " namespace=" << hit.namespace_id;
    }
    out << "\n";
  }
}

AgentRunnerRunResult run_cli_prompt(AgentRunner& runner,
                                    const std::string& input,
                                    const std::string& session_id,
                                    bool stream,
                                    bool debug,
                                    std::ostream& out) {
  if (!stream) {
    auto result = runner.run(input, session_id);
    out << result.text << "\n";
    if (debug) {
      render_cli_debug(result, out);
    }
    return result;
  }

  auto stream_result = runner.stream(input, session_id);
  bool printed_debug = false;
  for (const auto& event : stream_result.events) {
    if ((event.type == AgentRunnerStreamEventType::MemoryRetrieval ||
         event.type == AgentRunnerStreamEventType::KnowledgeRetrieval) && debug) {
      render_cli_retrieval_debug(event.memory_hits, event.knowledge_hits, out);
      printed_debug = true;
      continue;
    }
    if (event.type != AgentRunnerStreamEventType::Loop ||
        event.loop_event.type != AgentLoopStreamEventType::ModelTextDelta) {
      continue;
    }
    out << event.loop_event.delta;
  }
  out << "\n";
  if (debug && !printed_debug) {
    render_cli_debug(stream_result.result, out);
  }
  return stream_result.result;
}

std::string trim_cli_line(const std::string& line) {
  const auto begin = std::find_if_not(line.begin(), line.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto end = std::find_if_not(line.rbegin(), line.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

int run_repl_command(NativeResolvedAgentApp& app,
                     const NativeCliOptions& options,
                     std::istream& in,
                     std::ostream& out) {
  bool debug_enabled = options.debug;
  std::string line;
  while (true) {
    out << "> ";
    if (!std::getline(in, line)) {
      break;
    }
    line = trim_cli_line(line);
    if (line.empty()) {
      continue;
    }
    if (line == ":exit" || line == ":quit") {
      break;
    }
    if (line == ":clear") {
      if (auto* store = app.runner->session_store()) {
        store->clear(options.session_id);
      }
      out << "Cleared session " << options.session_id << ".\n";
      continue;
    }
    if (line.rfind(":debug", 0) == 0) {
      std::istringstream parts(line);
      std::string command;
      std::string value;
      parts >> command >> value;
      if (value == "on") {
        debug_enabled = true;
      } else if (value == "off") {
        debug_enabled = false;
      }
      out << "Debug " << (debug_enabled ? "on" : "off") << ".\n";
      continue;
    }
    (void)run_cli_prompt(*app.runner, line, options.session_id, options.stream, debug_enabled, out);
  }
  return 0;
}

int run_chat_command(const NativeCliOptions& options, std::istream& in, std::ostream& out) {
  auto app = options.config_path.empty()
                 ? create_manual_agent_app(options)
                 : load_native_agent_app(options.config_path, options.agent_id, options.config_module_loader);
  if (!options.prompt.empty()) {
    (void)run_cli_prompt(*app.runner, options.prompt, options.session_id, options.stream, options.debug, out);
    return 0;
  }
  return run_repl_command(app, options, in, out);
}

int run_validate_command(const NativeCliOptions& options, std::ostream& out) {
  require_config_path(options);
  const auto app = load_native_agent_app(options.config_path, options.agent_id, options.config_module_loader);
  out << "Config OK";
  if (!app.agent_id.empty()) {
    out << ": " << app.agent_id;
  }
  out << "\n";
  return 0;
}

int run_eval_command(const NativeCliOptions& options, std::ostream& out) {
  require_config_path(options);
  if (options.suite_path.empty()) {
    throw ConfigurationError("Eval command requires --suite <file>.");
  }
  auto suite = load_eval_suite(options.suite_path);
  if (!options.replay_dir.empty()) {
    suite.replay_dir = options.replay_dir;
  }
  auto app = load_native_agent_app(options.config_path,
                                   options.agent_id.empty() ? suite.agent : options.agent_id,
                                   options.config_module_loader);
  auto report = run_eval_suite(std::move(suite), *app.runner, options.case_ids, options.tags, options.stop_on_failure);

  if (!options.report_path.empty()) {
    write_eval_report(report, options.report_path);
    out << "Report written: " << options.report_path.string() << "\n";
  }
  if (!options.markdown_report_path.empty()) {
    write_eval_report(report, options.markdown_report_path);
    out << "Markdown report written: " << options.markdown_report_path.string() << "\n";
  }
  if (!options.baseline_path.empty()) {
    if (options.update_baseline) {
      write_eval_baseline(report, options.baseline_path);
      out << "Baseline updated: " << options.baseline_path.string() << "\n";
    } else {
      const auto baseline = load_eval_report(options.baseline_path);
      const auto comparison = compare_eval_baseline(report, baseline);
      std::size_t changed = 0;
      for (const auto& delta : comparison.deltas) {
        if (delta.status != "unchanged") {
          changed += 1;
        }
      }
      out << "Baseline diff: " << changed << " changed case(s)\n";
      for (const auto& delta : comparison.deltas) {
        if (delta.status != "unchanged") {
          out << "- " << delta.id << ": " << delta.status << "\n";
        }
      }
    }
  }
  if (options.report_path.empty() && options.markdown_report_path.empty()) {
    const auto fallback = std::filesystem::current_path() / ".node-agent" / "reports" /
                          "latest-eval-report.json";
    write_eval_report(report, fallback);
    out << "Report written: " << fallback.string() << "\n";
  }

  out << "Eval: " << report.passed_cases << "/" << report.total_cases << " passed in "
      << report.duration_ms << "ms\n";
  return report.failed_cases == 0 ? 0 : 1;
}

int run_replay_command(const NativeCliOptions& options, std::ostream& out) {
  const auto replay_dir = options.replay_dir.empty() ? default_replay_dir() : options.replay_dir;
  if (options.replay_action == "show") {
    if (options.run_path.empty()) {
      throw ConfigurationError("Replay show requires --run <path>.");
    }
    const auto replay = load_run_replay(options.run_path);
    out << "Run: " << replay.manifest.run_id << "\n";
    out << "Session: " << replay.manifest.session_id << "\n";
    out << "Status: " << replay.manifest.status << "\n";
    out << "Duration: " << replay.manifest.duration_ms << "ms\n";
    out << "Events: " << replay.manifest.event_count << "\n";
    out << "HTML: " << (std::filesystem::path(replay.dir_path) / replay.manifest.html_file).string() << "\n";
    if (!replay.manifest.error.empty()) {
      out << "Error: " << replay.manifest.error << "\n";
    }
    return 0;
  }

  const auto runs = list_session_replays(replay_dir, options.session_id);
  if (runs.empty()) {
    out << "No replay runs found for session " << options.session_id << " in " << replay_dir.string() << ".\n";
    return 0;
  }
  for (const auto& run_path : runs) {
    const auto replay = load_run_replay(run_path);
    out << run_path.string() << "  " << replay.manifest.status << "  "
        << replay.manifest.duration_ms << "ms\n";
  }
  return 0;
}

}  // namespace

NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::istream& stdin_stream,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream) {
  return run_native_agent_cli(args, stdin_stream, stdout_stream, stderr_stream, {});
}

NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::istream& stdin_stream,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream,
                                     NativeConfigModuleLoader config_module_loader) {
  try {
    auto options = parse_cli_options(args);
    options.config_module_loader = std::move(config_module_loader);
    if (options.help) {
      stdout_stream << cli_help_text();
      return {};
    }
    if (options.version) {
      stdout_stream << "native-agent 0.1.0\n";
      return {};
    }
    if (options.command == "chat") {
      return NativeCliResult{run_chat_command(options, stdin_stream, stdout_stream)};
    }
    if (options.command == "eval") {
      return NativeCliResult{run_eval_command(options, stdout_stream)};
    }
    if (options.command == "replay") {
      return NativeCliResult{run_replay_command(options, stdout_stream)};
    }
    if (options.command == "validate") {
      return NativeCliResult{run_validate_command(options, stdout_stream)};
    }
    throw ConfigurationError("Unknown command: " + options.command);
  } catch (const std::exception& error) {
    stderr_stream << error.what() << "\n";
    return NativeCliResult{1};
  }
}

NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream) {
  return run_native_agent_cli(args, std::cin, stdout_stream, stderr_stream);
}

NativeCliResult run_native_agent_cli(const std::vector<std::string>& args,
                                     std::ostream& stdout_stream,
                                     std::ostream& stderr_stream,
                                     NativeConfigModuleLoader config_module_loader) {
  return run_native_agent_cli(args, std::cin, stdout_stream, stderr_stream, std::move(config_module_loader));
}

}  // namespace agent
