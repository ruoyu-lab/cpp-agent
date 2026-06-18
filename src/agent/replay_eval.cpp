#include "agent/app_api.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <regex>
#include <sstream>

namespace agent {

namespace {

Value strings_to_value(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.push_back(value);
  }
  return Value(array);
}

std::vector<std::string> strings_from_value(const Value& value) {
  std::vector<std::string> result;
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      result.push_back(text);
    }
  }
  return result;
}

void add_unique_string(std::vector<std::string>& values, std::string value) {
  if (value.empty()) {
    return;
  }
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(std::move(value));
  }
}

void throw_if_eval_cancelled(CancellationToken* cancellation) {
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Run);
  }
}

struct ScopedRunnerEventSink {
  AgentRunner& runner;
  std::size_t sink_id = 0;

  ~ScopedRunnerEventSink() noexcept {
    runner.events().unregister_sink(sink_id);
  }
};

std::string js_regex_literal(const std::string& pattern, const std::string& flags) {
  std::string escaped;
  escaped.reserve(pattern.size());
  for (const char ch : pattern) {
    if (ch == '/') {
      escaped += "\\/";
    } else {
      escaped.push_back(ch);
    }
  }
  return "/" + escaped + "/" + flags;
}

std::optional<EvalTextMatcher> parse_js_regex_literal(const std::string& value) {
  if (value.size() < 2 || value.front() != '/') {
    return std::nullopt;
  }
  bool escaped = false;
  std::size_t closing = std::string::npos;
  for (std::size_t index = 1; index < value.size(); ++index) {
    const char ch = value[index];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '/') {
      closing = index;
    }
  }
  if (closing == std::string::npos || closing + 1 > value.size()) {
    return std::nullopt;
  }
  const std::string flags = value.substr(closing + 1);
  if (flags.find_first_not_of("dgimsuy") != std::string::npos) {
    return std::nullopt;
  }
  std::string pattern;
  pattern.reserve(closing - 1);
  for (std::size_t index = 1; index < closing; ++index) {
    if (value[index] == '\\' && index + 1 < closing && value[index + 1] == '/') {
      pattern.push_back('/');
      ++index;
    } else {
      pattern.push_back(value[index]);
    }
  }
  return EvalTextMatcher{.value = pattern, .regex = true, .flags = flags};
}

Value text_expectations_to_value(const std::vector<std::string>& literals,
                                 const std::vector<EvalTextMatcher>& matchers) {
  Value::Array array;
  for (const auto& literal : literals) {
    array.push_back(literal);
  }
  for (const auto& matcher : matchers) {
    if (matcher.regex) {
      array.push_back(Value::object({{"regex", js_regex_literal(matcher.value, matcher.flags)},
                                     {"pattern", matcher.value},
                                     {"flags", matcher.flags}}));
    } else {
      array.push_back(Value::object({{"text", matcher.value}}));
    }
  }
  return Value(std::move(array));
}

Value eval_permission_event_to_value(const EvalPermissionEvent& event) {
  return Value::object({{"category", event.category},
                        {"toolName", event.tool_name.empty() ? Value() : Value(event.tool_name)},
                        {"reason", event.reason.empty() ? Value() : Value(event.reason)},
                        {"decision", event.decision.empty() ? Value() : Value(event.decision)},
                        {"raw", event.raw}});
}

EvalPermissionEvent eval_permission_event_from_value(const Value& value) {
  EvalPermissionEvent event;
  event.category = value.at("category").as_string();
  event.tool_name = value.at("toolName").as_string(value.at("tool_name").as_string());
  event.reason = value.at("reason").as_string();
  event.decision = value.at("decision").as_string();
  event.raw = value.at("raw").is_object() ? value.at("raw") : Value::object({});
  return event;
}

EvalPermissionEvent eval_permission_event_from_framework_event(const FrameworkEvent& event) {
  const auto& payload = event.payload;
  EvalPermissionEvent permission_event;
  permission_event.category = event.category;
  permission_event.tool_name = payload.at("toolName").as_string(payload.at("tool_name").as_string());
  permission_event.reason = payload.at("reason").as_string();
  permission_event.decision = payload.at("decision").as_string();
  permission_event.raw = framework_event_to_value(event);
  return permission_event;
}

std::vector<std::string> collect_eval_status_stages(const std::vector<AgentRunnerStreamEvent>& events) {
  std::vector<std::string> stages;
  for (const auto& event : events) {
    if (event.type == AgentRunnerStreamEventType::Status) {
      add_unique_string(stages, event.status.stage);
      continue;
    }
    if (event.type == AgentRunnerStreamEventType::KnowledgeRetrieval) {
      add_unique_string(stages, "knowledge-retrieval");
      continue;
    }
    if (event.type == AgentRunnerStreamEventType::MemoryRetrieval) {
      add_unique_string(stages, "memory-retrieval");
      continue;
    }
    if (event.type == AgentRunnerStreamEventType::Planning) {
      add_unique_string(stages, "planning");
      continue;
    }
    if (event.type == AgentRunnerStreamEventType::UserVisibleDelta) {
      add_unique_string(stages, "answer");
      continue;
    }
    if (event.type == AgentRunnerStreamEventType::Error) {
      add_unique_string(stages, "error");
      continue;
    }
    if (event.type == AgentRunnerStreamEventType::Cancelled) {
      add_unique_string(stages, "cancelled");
      continue;
    }
    if (event.type != AgentRunnerStreamEventType::Loop) {
      continue;
    }
    switch (event.loop_event.type) {
      case AgentLoopStreamEventType::ModelStart:
      case AgentLoopStreamEventType::ModelTextDelta:
      case AgentLoopStreamEventType::UserVisibleDelta:
      case AgentLoopStreamEventType::ModelReasoningDelta:
      case AgentLoopStreamEventType::ModelReasoningCompleted:
      case AgentLoopStreamEventType::AgentOutput:
      case AgentLoopStreamEventType::ToolCallArgumentDelta:
        add_unique_string(stages, "model");
        break;
      case AgentLoopStreamEventType::ToolBatchStart:
      case AgentLoopStreamEventType::ToolStart:
      case AgentLoopStreamEventType::ToolDelta:
      case AgentLoopStreamEventType::ToolComplete:
      case AgentLoopStreamEventType::ToolBatchComplete:
        add_unique_string(stages, "tool");
        break;
      case AgentLoopStreamEventType::ReActMessage:
      case AgentLoopStreamEventType::ReActFinal:
      case AgentLoopStreamEventType::ReActFinalRejected:
      case AgentLoopStreamEventType::ReActReasoningProtocolLeak:
      case AgentLoopStreamEventType::ReActParseError:
        add_unique_string(stages, "model");
        break;
      case AgentLoopStreamEventType::ReActActionBatch:
      case AgentLoopStreamEventType::ReActObservation:
        add_unique_string(stages, "tool");
        break;
      case AgentLoopStreamEventType::IterationStart:
      case AgentLoopStreamEventType::Done:
        break;
    }
  }
  return stages;
}

struct RunReplayEventSummary {
  std::size_t tool_call_count = 0;
  std::vector<std::string> status_stages;
};

std::string run_replay_loop_event_type(const Value& event) {
  if (event.is_string()) {
    return event.as_string();
  }
  if (event.is_object()) {
    return event.at("type").as_string();
  }
  return {};
}

RunReplayEventSummary summarize_run_replay_events(const std::vector<Value>& events) {
  RunReplayEventSummary summary;
  for (const auto& event : events) {
    if (!event.is_object()) {
      continue;
    }
    const auto type = event.at("type").as_string();
    if (type == "status") {
      add_unique_string(summary.status_stages, event.at("status").at("stage").as_string());
      continue;
    }
    if (type == "loop") {
      if (run_replay_loop_event_type(event.at("event")) == "tool-start") {
        summary.tool_call_count += 1;
      }
      continue;
    }
    if (type == "tool-start") {
      summary.tool_call_count += 1;
    }
  }
  return summary;
}

bool parse_fixed_int(const std::string& value, std::size_t offset, std::size_t length, int& output) {
  if (offset + length > value.size()) {
    return false;
  }
  int parsed = 0;
  for (std::size_t index = 0; index < length; ++index) {
    const auto ch = static_cast<unsigned char>(value[offset + index]);
    if (!std::isdigit(ch)) {
      return false;
    }
    parsed = (parsed * 10) + static_cast<int>(ch - '0');
  }
  output = parsed;
  return true;
}

bool is_leap_year(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
  static constexpr std::array<int, 12> days{{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};
  if (month == 2 && is_leap_year(year)) {
    return 29;
  }
  if (month < 1 || month > 12) {
    return 0;
  }
  return days[static_cast<std::size_t>(month - 1)];
}

long long days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2 ? 1 : 0;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const auto year_of_era = static_cast<unsigned>(year - era * 400);
  const unsigned month_prime = month > 2 ? month - 3 : month + 9;
  const unsigned day_of_year = (153 * month_prime + 2) / 5 + day - 1;
  const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return static_cast<long long>(era) * 146097LL + static_cast<long long>(day_of_era) - 719468LL;
}

std::optional<long long> parse_iso8601_epoch_ms(const std::string& value) {
  if (value.size() < 19 || value[4] != '-' || value[7] != '-' ||
      (value[10] != 'T' && value[10] != ' ') || value[13] != ':' || value[16] != ':') {
    return std::nullopt;
  }

  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (!parse_fixed_int(value, 0, 4, year) || !parse_fixed_int(value, 5, 2, month) ||
      !parse_fixed_int(value, 8, 2, day) || !parse_fixed_int(value, 11, 2, hour) ||
      !parse_fixed_int(value, 14, 2, minute) || !parse_fixed_int(value, 17, 2, second)) {
    return std::nullopt;
  }
  if (month < 1 || month > 12 || day < 1 || day > days_in_month(year, month) || hour < 0 ||
      hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
    return std::nullopt;
  }

  std::size_t cursor = 19;
  int millisecond = 0;
  if (cursor < value.size() && value[cursor] == '.') {
    ++cursor;
    int digits = 0;
    while (cursor < value.size()) {
      const auto ch = static_cast<unsigned char>(value[cursor]);
      if (!std::isdigit(ch)) {
        break;
      }
      if (digits < 3) {
        millisecond = (millisecond * 10) + static_cast<int>(ch - '0');
      }
      ++digits;
      ++cursor;
    }
    if (digits == 0) {
      return std::nullopt;
    }
    while (digits < 3) {
      millisecond *= 10;
      ++digits;
    }
  }

  int offset_minutes = 0;
  if (cursor < value.size()) {
    const char tz = value[cursor];
    if (tz == 'Z' || tz == 'z') {
      ++cursor;
    } else if (tz == '+' || tz == '-') {
      const bool negative = tz == '-';
      ++cursor;
      int offset_hours = 0;
      int offset_mins = 0;
      if (!parse_fixed_int(value, cursor, 2, offset_hours)) {
        return std::nullopt;
      }
      cursor += 2;
      if (cursor < value.size() && value[cursor] == ':') {
        ++cursor;
      }
      if (cursor < value.size()) {
        if (!parse_fixed_int(value, cursor, 2, offset_mins)) {
          return std::nullopt;
        }
        cursor += 2;
      }
      if (offset_hours > 23 || offset_mins > 59) {
        return std::nullopt;
      }
      offset_minutes = (offset_hours * 60) + offset_mins;
      if (negative) {
        offset_minutes = -offset_minutes;
      }
    } else {
      return std::nullopt;
    }
  }
  if (cursor != value.size()) {
    return std::nullopt;
  }

  const long long days = days_from_civil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));
  const long long seconds = (days * 24LL * 60LL * 60LL) +
                            (static_cast<long long>(hour) * 60LL * 60LL) +
                            (static_cast<long long>(minute) * 60LL) +
                            static_cast<long long>(second);
  return (seconds * 1000LL) + millisecond -
         (static_cast<long long>(offset_minutes) * 60LL * 1000LL);
}

int run_replay_duration_ms(const std::string& started_at, const std::string& finished_at) {
  const auto started_ms = parse_iso8601_epoch_ms(started_at);
  const auto finished_ms = parse_iso8601_epoch_ms(finished_at);
  if (!started_ms || !finished_ms) {
    return 0;
  }
  const auto duration = std::max<long long>(0, *finished_ms - *started_ms);
  return static_cast<int>(std::min<long long>(duration, std::numeric_limits<int>::max()));
}

void read_text_expectations(const Value& value, std::vector<std::string>& literals,
                            std::vector<EvalTextMatcher>& matchers) {
  for (const auto& item : value.as_array()) {
    if (item.is_string()) {
      const auto text = item.as_string();
      if (!text.empty()) {
        literals.push_back(text);
      }
      continue;
    }
    if (!item.is_object()) {
      continue;
    }
    const auto& text_value = item.at("text");
    if (text_value.is_string()) {
      const auto text = text_value.as_string();
      if (!text.empty()) {
        literals.push_back(text);
      }
      continue;
    }
    const auto& regex_value = item.at("regex");
    if (regex_value.is_string()) {
      auto parsed = parse_js_regex_literal(regex_value.as_string());
      if (parsed) {
        if (item.at("flags").is_string() && parsed->flags.empty()) {
          parsed->flags = item.at("flags").as_string();
        }
        matchers.push_back(*parsed);
        continue;
      }
      matchers.push_back(EvalTextMatcher{.value = regex_value.as_string(),
                                         .regex = true,
                                         .flags = item.at("flags").as_string()});
      continue;
    }
    if (item.at("pattern").is_string()) {
      matchers.push_back(EvalTextMatcher{.value = item.at("pattern").as_string(),
                                         .regex = true,
                                         .flags = item.at("flags").as_string()});
    }
  }
}

std::regex_constants::syntax_option_type regex_options_from_flags(const std::string& flags) {
  auto options = std::regex_constants::ECMAScript;
  if (flags.find('i') != std::string::npos) {
    options |= std::regex_constants::icase;
  }
  return options;
}

std::string format_eval_matcher(const EvalTextMatcher& matcher) {
  return matcher.regex ? js_regex_literal(matcher.value, matcher.flags) : "\"" + matcher.value + "\"";
}

bool eval_text_matches(const std::string& text, const EvalTextMatcher& matcher, std::string& error) {
  if (!matcher.regex) {
    return text.find(matcher.value) != std::string::npos;
  }
  try {
    return std::regex_search(text, std::regex(matcher.value, regex_options_from_flags(matcher.flags)));
  } catch (const std::regex_error& regex_error) {
    error = regex_error.what();
    return false;
  }
}

Value number_or_null(std::optional<double> value) {
  return value ? Value(*value) : Value();
}

bool has_markdown_extension(const std::filesystem::path& path) {
  const auto extension = path.extension().string();
  return extension == ".md" || extension == ".markdown";
}

EvalAssertionResult pass_eval_assertion(std::string name, std::string message) {
  return EvalAssertionResult{std::move(name), true, std::move(message)};
}

EvalAssertionResult fail_eval_assertion(std::string name, std::string message) {
  return EvalAssertionResult{std::move(name), false, std::move(message)};
}

std::vector<std::string> collect_eval_tool_calls(const AgentRunnerRunResult& run) {
  std::vector<std::string> calls;
  for (const auto& entry : run.trace) {
    if (entry.type != "model") {
      continue;
    }
    for (const auto& call : entry.response.tool_calls) {
      add_unique_string(calls, call.name);
    }
  }
  for (const auto& entry : run.react_trace) {
    if (entry.type == ReActStepType::ActionBatch) {
      for (const auto& action : entry.actions) {
        add_unique_string(calls, action.tool);
      }
    }
  }
  for (const auto& message : run.messages) {
    if (message.role != MessageRole::Assistant) {
      continue;
    }
    const auto text = extract_text_content(message.content);
    const auto actions_pos = text.find("Actions:");
    if (actions_pos == std::string::npos) {
      continue;
    }
    std::string json = text.substr(actions_pos + std::string("Actions:").size());
    const auto first = std::find_if(json.begin(), json.end(), [](unsigned char ch) {
      return !std::isspace(ch);
    });
    const auto last = std::find_if(json.rbegin(), json.rend(), [](unsigned char ch) {
      return !std::isspace(ch);
    }).base();
    if (first >= last) {
      continue;
    }
    json = std::string(first, last);
    try {
      const auto parsed = parse_json(json);
      if (!parsed.is_array()) {
        continue;
      }
      for (const auto& action : parsed.as_array()) {
        const auto tool = action.at("tool").as_string();
        if (!tool.empty()) {
          add_unique_string(calls, tool);
        }
      }
    } catch (...) {
    }
  }
  return calls;
}

void append_eval_tool_calls_from_events(std::vector<std::string>& calls,
                                        const std::vector<AgentRunnerStreamEvent>& events) {
  for (const auto& event : events) {
    if (event.type != AgentRunnerStreamEventType::Loop) {
      continue;
    }
    const auto& loop = event.loop_event;
    if (loop.type == AgentLoopStreamEventType::ReActActionBatch ||
        loop.type == AgentLoopStreamEventType::ToolBatchStart) {
      for (const auto& tool_call : loop.tool_calls) {
        add_unique_string(calls, tool_call.name);
      }
      continue;
    }
    if (loop.type == AgentLoopStreamEventType::ToolStart && !loop.tool_call.name.empty()) {
      add_unique_string(calls, loop.tool_call.name);
      continue;
    }
    if (loop.type == AgentLoopStreamEventType::ToolComplete &&
        !loop.tool_result.tool_call.name.empty()) {
      add_unique_string(calls, loop.tool_result.tool_call.name);
      continue;
    }
    if (loop.type == AgentLoopStreamEventType::ToolBatchComplete) {
      for (const auto& tool_call : loop.tool_calls) {
        add_unique_string(calls, tool_call.name);
      }
      for (const auto& result : loop.tool_results) {
        add_unique_string(calls, result.tool_call.name);
      }
    }
  }
}

std::size_t count_eval_citations(const AgentRunnerRunResult& run) {
  std::size_t count = 0;
  for (const auto& hit : run.knowledge_hits) {
    if (hit.citation.document_id.empty() && hit.citation.chunk_id.empty() && hit.citation.title.empty()) {
      continue;
    }
    ++count;
  }
  for (const auto& hit : run.memory_hits) {
    if (hit.metadata.at("citation").is_object()) {
      ++count;
    }
  }
  const auto& raw_citations = run.response.raw.at("citations");
  if (raw_citations.is_array()) {
    count += raw_citations.as_array().size();
  }
  return count;
}

std::optional<Value> eval_structured_candidate(const AgentRunnerRunResult& run) {
  if (run.response.raw.is_object()) {
    if (run.response.raw.contains("structuredContent")) {
      return run.response.raw.at("structuredContent");
    }
    if (run.response.raw.contains("structured_content")) {
      return run.response.raw.at("structured_content");
    }
  }
  try {
    return parse_json(run.text);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::string format_schema_validation_error(const SchemaValidationError& error) {
  std::ostringstream out;
  out << error.what();
  const auto& issues = error.issues();
  if (!issues.empty()) {
    out << " ";
    for (std::size_t index = 0; index < issues.size(); ++index) {
      if (index > 0) {
        out << "; ";
      }
      out << issues[index].path << ": " << issues[index].message;
    }
  }
  return out.str();
}

Value agent_runner_status_to_value(const AgentRunnerStatus& status) {
  Value payload = Value::object({
      {"kind", status.kind},
      {"stage", status.stage},
      {"state", status.state},
      {"message", status.message},
      {"details", status.details},
  });
  if (status.iteration >= 0) {
    payload["iteration"] = status.iteration;
  }
  if (!status.provider.empty()) {
    payload["provider"] = status.provider;
  }
  if (!status.model.empty()) {
    payload["model"] = status.model;
  }
  if (!status.tool_name.empty()) {
    payload["toolName"] = status.tool_name;
  }
  if (!status.tool_call_id.empty()) {
    payload["toolCallId"] = status.tool_call_id;
  }
  return payload;
}

EvalUsage eval_usage_from_model_response(const AgentOutput& response, const std::optional<EvalPricing>& pricing) {
  const auto usage = extract_model_usage(response);
  EvalUsage output;
  output.input_tokens = usage.input_tokens;
  output.output_tokens = usage.output_tokens;
  output.total_tokens = usage.total_tokens;
  if (pricing) {
    const auto cost = estimate_usage_cost(
        usage,
        UsagePricing{.input_per_1k_tokens = pricing->input_per_1k_tokens,
                     .output_per_1k_tokens = pricing->output_per_1k_tokens,
                     .currency = pricing->currency},
        response.provider,
        response.model);
    if (cost.total_cost > 0) {
      output.estimated_cost = cost.total_cost;
      output.currency = cost.currency;
    } else {
      output.currency = pricing->currency;
    }
  }
  return output;
}

Value runner_stream_event_to_value(const AgentRunnerStreamEvent& event) {
  if (event.type == AgentRunnerStreamEventType::Status) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "status"},
                          {"status", agent_runner_status_to_value(event.status)}});
  }
  if (event.type == AgentRunnerStreamEventType::KnowledgeRetrieval) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "knowledge-retrieval"},
                          {"hitCount", event.knowledge_hits.size()}});
  }
  if (event.type == AgentRunnerStreamEventType::MemoryRetrieval) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "memory-retrieval"},
                          {"hitCount", event.memory_hits.size()}});
  }
  if (event.type == AgentRunnerStreamEventType::Planning) {
    Value payload = Value::object({{"schemaVersion", event.schema_version},
                                   {"sequence", static_cast<long long>(event.sequence)},
                                   {"type", "plan"},
                                   {"hasPlan", event.plan.has_value()}});
    if (event.plan) {
      payload["goal"] = event.plan->goal;
      payload["stepCount"] = event.plan->steps.size();
    }
    return payload;
  }
  if (event.type == AgentRunnerStreamEventType::Done) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "done"},
                          {"text", event.result.text}});
  }
  if (event.type == AgentRunnerStreamEventType::UserVisibleDelta) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "user-visible-delta"},
                          {"delta", event.delta},
                          {"text", event.text}});
  }
  if (event.type == AgentRunnerStreamEventType::Error) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "error"},
                          {"error", event.error}});
  }
  if (event.type == AgentRunnerStreamEventType::Cancelled) {
    return Value::object({{"schemaVersion", event.schema_version},
                          {"sequence", static_cast<long long>(event.sequence)},
                          {"type", "cancelled"},
                          {"cancellation", event.cancellation}});
  }
  if (event.type == AgentRunnerStreamEventType::ToolCallArgumentDelta) {
    return Value::object({
        {"schemaVersion", event.schema_version},
        {"sequence", static_cast<long long>(event.sequence)},
        {"type", "tool-call-argument-delta"},
        {"iteration", static_cast<long long>(event.tool_call_iteration)},
        {"provider", event.tool_call_provider},
        {"model", event.tool_call_model},
        {"toolCallId", event.tool_call_id},
        {"toolName", event.tool_call_name},
        {"argsDelta", event.tool_call_args_delta},
        {"argsAccumulated", event.tool_call_args_accumulated},
    });
  }
  Value payload = Value::object({{"schemaVersion", event.schema_version},
                                 {"sequence", static_cast<long long>(event.sequence)},
                                 {"type", "loop"},
                                 {"event", to_string(event.loop_event.type)}});
  if (event.loop_event.type == AgentLoopStreamEventType::ModelTextDelta ||
      event.loop_event.type == AgentLoopStreamEventType::UserVisibleDelta ||
      event.loop_event.type == AgentLoopStreamEventType::ModelReasoningDelta) {
    payload["delta"] = event.loop_event.delta;
  }
  if (event.loop_event.type == AgentLoopStreamEventType::ToolStart) {
    payload["toolName"] = event.loop_event.tool_call.name;
  }
  if (event.loop_event.type == AgentLoopStreamEventType::ToolComplete) {
    payload["toolName"] = event.loop_event.tool_result.tool_call.name;
    payload["ok"] = event.loop_event.tool_result.ok;
  }
  if (event.loop_event.type == AgentLoopStreamEventType::ToolBatchStart) {
    payload["toolCount"] = event.loop_event.tool_calls.size();
  }
  if (event.loop_event.type == AgentLoopStreamEventType::ToolBatchComplete) {
    payload["toolCount"] = event.loop_event.tool_results.size();
    std::size_t failed_count = 0;
    for (const auto& result : event.loop_event.tool_results) {
      if (!result.ok) {
        ++failed_count;
      }
    }
    payload["failedCount"] = failed_count;
  }
  if (event.loop_event.type == AgentLoopStreamEventType::ReActMessage ||
      event.loop_event.type == AgentLoopStreamEventType::ReActActionBatch ||
      event.loop_event.type == AgentLoopStreamEventType::ReActObservation ||
      event.loop_event.type == AgentLoopStreamEventType::ReActFinal ||
      event.loop_event.type == AgentLoopStreamEventType::ReActFinalRejected ||
      event.loop_event.type == AgentLoopStreamEventType::ReActReasoningProtocolLeak ||
      event.loop_event.type == AgentLoopStreamEventType::ReActParseError) {
    payload["react"] = react_trace_entry_to_value(event.loop_event.react_step);
  }
  return payload;
}

std::vector<Value> runner_stream_events_to_values(const std::vector<AgentRunnerStreamEvent>& events) {
  std::vector<Value> values;
  values.reserve(events.size());
  for (const auto& event : events) {
    values.push_back(runner_stream_event_to_value(event));
  }
  return values;
}

Value eval_case_input_value(const EvalCase& test_case) {
  return test_case.input_value.is_null() ? Value(test_case.input) : test_case.input_value;
}

bool eval_case_has_message_input(const EvalCase& test_case) {
  return test_case.input_value.is_object() && test_case.input_value.contains("role");
}

std::string eval_case_text_projection(const Value& input) {
  if (input.is_string()) {
    return input.as_string();
  }
  if (input.is_object() && input.contains("role")) {
    try {
      return extract_text_content(agent_message_from_value(input).content);
    } catch (...) {
      return safe_json_stringify(input);
    }
  }
  return safe_json_stringify(input);
}

AgentRunnerStreamResult stream_eval_case_input(AgentRunner& runner,
                                               const EvalCase& test_case,
                                               const std::string& session_id,
                                               CancellationToken* cancellation,
                                               std::vector<AgentRunnerStreamEvent>& stream_events) {
  auto on_event = [&](const AgentRunnerStreamEvent& event) {
    stream_events.push_back(event);
  };
  if (eval_case_has_message_input(test_case)) {
    return runner.streaming().stream(agent_message_from_value(test_case.input_value),
                         on_event,
                         session_id,
                         test_case.model_settings,
                         {}, {}, {},
                         Value::object({}),
                         std::nullopt,
                         cancellation);
  }
  const auto input = test_case.input_value.is_null() ? test_case.input : eval_case_text_projection(test_case.input_value);
  return runner.streaming().stream(input,
                       on_event,
                       session_id,
                       test_case.model_settings,
                       {}, {}, {},
                       Value::object({}),
                       std::nullopt,
                       cancellation);
}

std::string write_eval_case_replay(const EvalSuite& suite,
                                   const EvalCase& test_case,
                                   const EvalCaseResult& result,
                                   const std::vector<AgentRunnerStreamEvent>& events) {
  if (suite.replay_dir.empty()) {
    return {};
  }
  WriteRunReplayOptions replay_options;
  replay_options.base_dir = suite.replay_dir;
  replay_options.session_id = result.session_id;
  replay_options.agent_id = suite.agent;
  replay_options.input = eval_case_input_value(test_case);
  replay_options.result = eval_case_result_to_value(result);
  replay_options.events = runner_stream_events_to_values(events);
  replay_options.run_id = test_case.id;
  replay_options.error = result.error;
  replay_options.metadata = Value::object({{"caseId", test_case.id},
                                           {"suiteAgent", suite.agent.empty() ? Value() : Value(suite.agent)},
                                           {"caseMetadata", test_case.metadata}});
  return write_run_replay(replay_options).html_path.string();
}

}  // namespace

Value run_replay_manifest_to_value(const RunReplayManifest& manifest) {
  Value::Object object{{"version", manifest.version},
                       {"runId", manifest.run_id},
                       {"sessionId", manifest.session_id},
                       {"startedAt", manifest.started_at},
                       {"finishedAt", manifest.finished_at},
                       {"durationMs", manifest.duration_ms},
                       {"status", manifest.status},
                       {"eventCount", manifest.event_count},
                       {"toolCallCount", manifest.tool_call_count},
                       {"inputFile", manifest.input_file},
                       {"eventsFile", manifest.events_file},
                       {"htmlFile", manifest.html_file}};
  if (!manifest.agent_id.empty()) {
    object["agentId"] = manifest.agent_id;
  }
  if (!manifest.result_file.empty()) {
    object["resultFile"] = manifest.result_file;
  }
  if (!manifest.error.empty()) {
    object["error"] = manifest.error;
  }
  if (manifest.metadata.is_object() && !manifest.metadata.as_object().empty()) {
    object["metadata"] = manifest.metadata;
  }
  return Value(object);
}

RunReplayManifest run_replay_manifest_from_value(const Value& value) {
  RunReplayManifest manifest;
  manifest.version = static_cast<int>(value.at("version").as_integer(1));
  manifest.run_id = value.at("runId").as_string(value.at("run_id").as_string());
  manifest.session_id = value.at("sessionId").as_string(value.at("session_id").as_string());
  manifest.agent_id = value.at("agentId").as_string(value.at("agent_id").as_string());
  manifest.started_at = value.at("startedAt").as_string(value.at("started_at").as_string());
  manifest.finished_at = value.at("finishedAt").as_string(value.at("finished_at").as_string());
  manifest.duration_ms = static_cast<int>(value.at("durationMs").as_integer(value.at("duration_ms").as_integer()));
  manifest.status = value.at("status").as_string("ok");
  manifest.event_count =
      static_cast<std::size_t>(std::max<long long>(0, value.at("eventCount").as_integer(value.at("event_count").as_integer())));
  manifest.tool_call_count =
      static_cast<std::size_t>(std::max<long long>(0, value.at("toolCallCount").as_integer(value.at("tool_call_count").as_integer())));
  manifest.input_file = value.at("inputFile").as_string(value.at("input_file").as_string("input.json"));
  manifest.result_file = value.at("resultFile").as_string(value.at("result_file").as_string());
  manifest.events_file = value.at("eventsFile").as_string(value.at("events_file").as_string("events.json"));
  manifest.html_file = value.at("htmlFile").as_string(value.at("html_file").as_string("replay.html"));
  manifest.error = value.at("error").as_string();
  manifest.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return manifest;
}

LoadedRunReplay load_run_replay(const std::filesystem::path& run_path) {
  std::filesystem::path run_dir = run_path;
  std::error_code error;
  const auto filename = run_dir.filename().string();
  if (std::filesystem::is_regular_file(run_dir, error) || filename == "manifest.json" ||
      filename == "replay.html") {
    run_dir = run_dir.parent_path();
  }
  const auto manifest = run_replay_manifest_from_value(read_json_file(run_dir / "manifest.json"));
  LoadedRunReplay replay;
  replay.dir_path = run_dir.string();
  replay.manifest = manifest;
  replay.input = read_json_file(run_dir / manifest.input_file);
  if (!manifest.result_file.empty()) {
    const auto result_path = run_dir / manifest.result_file;
    if (std::filesystem::exists(result_path)) {
      replay.result = read_json_file(result_path);
    }
  }
  const auto events = read_json_file(run_dir / manifest.events_file);
  if (events.is_array()) {
    replay.events = events.as_array();
  }
  return replay;
}

std::vector<std::filesystem::path> list_session_replays(const std::filesystem::path& base_dir,
                                                        const std::string& session_id) {
  const auto session_dir = base_dir / sanitize_segment(session_id);
  std::vector<std::filesystem::path> runs;
  if (!std::filesystem::exists(session_dir)) {
    return runs;
  }
  for (const auto& entry : std::filesystem::directory_iterator(session_dir)) {
    if (entry.is_directory() && std::filesystem::exists(entry.path() / "manifest.json")) {
      runs.push_back(entry.path());
    }
  }
  std::sort(runs.begin(), runs.end());
  return runs;
}

std::string render_run_replay_html(const LoadedRunReplay& replay) {
  const auto summary = summarize_run_replay_events(replay.events);
  const auto status_stages =
      summary.status_stages.empty()
          ? std::string("-")
          : std::accumulate(std::next(summary.status_stages.begin()),
                            summary.status_stages.end(),
                            summary.status_stages.front(),
                            [](std::string text, const std::string& stage) {
                              text += ", ";
                              text += stage;
                              return text;
                            });
  std::ostringstream html;
  html << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><title>Run Replay "
       << html_escape(replay.manifest.run_id)
       << "</title><style>body{font-family:system-ui;margin:32px;color:#1d1d1f}"
          "pre{white-space:pre-wrap;background:#f4f4f5;padding:16px;border:1px solid #ddd}"
          ".grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}"
          ".card{border:1px solid #ddd;padding:12px}</style></head><body>";
  html << "<h1>Run Replay</h1><p>Session <b>" << html_escape(replay.manifest.session_id)
       << "</b> · Run <b>" << html_escape(replay.manifest.run_id) << "</b></p><section class=\"grid\">";
  html << "<article class=\"card\"><b>Status</b><br>" << html_escape(replay.manifest.status)
       << "</article><article class=\"card\"><b>Agent</b><br>"
       << html_escape(replay.manifest.agent_id.empty() ? "-" : replay.manifest.agent_id)
       << "</article><article class=\"card\"><b>Duration</b><br>" << replay.manifest.duration_ms << " ms"
       << "</article><article class=\"card\"><b>Events</b><br>" << replay.manifest.event_count
       << "</article><article class=\"card\"><b>Tool Calls</b><br>" << summary.tool_call_count
       << "</article><article class=\"card\"><b>Started</b><br>" << html_escape(replay.manifest.started_at)
       << "</article><article class=\"card\"><b>Finished</b><br>" << html_escape(replay.manifest.finished_at)
       << "</article><article class=\"card\"><b>Status Stages</b><br>" << html_escape(status_stages)
       << "</article><article class=\"card\"><b>Error</b><br>"
       << html_escape(replay.manifest.error.empty() ? "-" : replay.manifest.error)
       << "</article></section>";
  html << "<h2>Input</h2><pre>" << html_escape(replay.input.stringify(2)) << "</pre>";
  html << "<h2>Result</h2><pre>" << html_escape(replay.result.stringify(2)) << "</pre>";
  html << "<h2>Events</h2><pre>" << html_escape(Value::Array(replay.events.begin(), replay.events.end()).empty() ? "[]"
                                                                      : Value(Value::Array(replay.events.begin(), replay.events.end())).stringify(2))
       << "</pre></body></html>";
  return html.str();
}

WriteRunReplayResult write_run_replay(const WriteRunReplayOptions& options) {
  auto run_id = options.run_id;
  if (run_id.empty()) {
    run_id = generate_uuid();
  }
  const auto started_at = options.started_at.empty() ? now_iso8601() : options.started_at;
  const auto finished_at = options.finished_at.empty() ? now_iso8601() : options.finished_at;

  RunReplayManifest manifest;
  manifest.run_id = sanitize_segment(run_id);
  manifest.session_id = options.session_id;
  manifest.agent_id = options.agent_id;
  manifest.started_at = started_at;
  manifest.finished_at = finished_at;
  manifest.duration_ms = run_replay_duration_ms(started_at, finished_at);
  manifest.status = options.error.empty() ? "ok" : "error";
  manifest.error = options.error;
  manifest.metadata = options.metadata.is_object() ? options.metadata : Value::object({});
  manifest.event_count = options.events.size();
  const auto summary = summarize_run_replay_events(options.events);
  manifest.tool_call_count = summary.tool_call_count;
  if (!options.result.is_null()) {
    manifest.result_file = "result.json";
  }
  const auto dir = options.base_dir / sanitize_segment(options.session_id) / manifest.run_id;
  LoadedRunReplay replay{dir.string(), manifest, options.input, options.result, options.events};
  write_text_file(dir / manifest.input_file, options.input.stringify(2));
  if (!manifest.result_file.empty()) {
    write_text_file(dir / manifest.result_file, options.result.stringify(2));
  }
  write_text_file(dir / manifest.events_file,
                  Value(Value::Array(options.events.begin(), options.events.end())).stringify(2));
  const auto html_path = dir / manifest.html_file;
  const auto manifest_path = dir / "manifest.json";
  write_text_file(html_path, render_run_replay_html(replay));
  write_text_file(manifest_path, run_replay_manifest_to_value(manifest).stringify(2));

  WriteRunReplayResult write_result;
  write_result.dir_path = dir;
  write_result.manifest_path = manifest_path;
  write_result.html_path = html_path;
  write_result.manifest = manifest;
  return write_result;
}

RunReplayManifest write_run_replay(const std::filesystem::path& base_dir, std::string session_id,
                                   Value input, Value result, std::vector<Value> events,
                                   std::string run_id, std::string agent_id, std::string error,
                                   Value metadata) {
  WriteRunReplayOptions options;
  options.base_dir = base_dir;
  options.run_id = std::move(run_id);
  options.session_id = std::move(session_id);
  options.agent_id = std::move(agent_id);
  options.input = std::move(input);
  options.result = std::move(result);
  options.events = std::move(events);
  options.error = std::move(error);
  options.metadata = std::move(metadata);
  return write_run_replay(options).manifest;
}

Value eval_assertion_result_to_value(const EvalAssertionResult& assertion) {
  return Value::object({{"name", assertion.name},
                        {"passed", assertion.passed},
                        {"message", assertion.message}});
}

EvalAssertionResult eval_assertion_result_from_value(const Value& value) {
  EvalAssertionResult assertion;
  assertion.name = value.at("name").as_string();
  assertion.passed = value.at("passed").as_bool();
  assertion.message = value.at("message").as_string();
  return assertion;
}

Value eval_usage_to_value(const EvalUsage& usage) {
  Value::Object object{{"inputTokens", usage.input_tokens},
                       {"outputTokens", usage.output_tokens},
                       {"totalTokens", usage.total_tokens},
                       {"currency", usage.currency}};
  if (usage.estimated_cost) {
    object["estimatedCost"] = *usage.estimated_cost;
  }
  return Value(std::move(object));
}

EvalUsage eval_usage_from_value(const Value& value) {
  EvalUsage usage;
  usage.input_tokens = static_cast<int>(
      value.at("inputTokens").as_integer(value.at("input_tokens").as_integer()));
  usage.output_tokens = static_cast<int>(
      value.at("outputTokens").as_integer(value.at("output_tokens").as_integer()));
  usage.total_tokens = static_cast<int>(
      value.at("totalTokens").as_integer(value.at("total_tokens").as_integer()));
  if (value.at("estimatedCost").is_number() || value.at("estimated_cost").is_number()) {
    usage.estimated_cost = value.at("estimatedCost").is_number()
                               ? value.at("estimatedCost").as_number()
                               : value.at("estimated_cost").as_number();
  }
  usage.currency = value.at("currency").as_string("USD");
  return usage;
}

Value eval_pricing_to_value(const EvalPricing& pricing) {
  return Value::object({{"inputPer1kTokens", number_or_null(pricing.input_per_1k_tokens)},
                        {"outputPer1kTokens", number_or_null(pricing.output_per_1k_tokens)},
                        {"currency", pricing.currency}});
}

EvalPricing eval_pricing_from_value(const Value& value) {
  EvalPricing pricing;
  if (value.at("inputPer1kTokens").is_number() || value.at("input_per_1k_tokens").is_number()) {
    pricing.input_per_1k_tokens = value.at("inputPer1kTokens").is_number()
                                      ? value.at("inputPer1kTokens").as_number()
                                      : value.at("input_per_1k_tokens").as_number();
  }
  if (value.at("outputPer1kTokens").is_number() || value.at("output_per_1k_tokens").is_number()) {
    pricing.output_per_1k_tokens = value.at("outputPer1kTokens").is_number()
                                       ? value.at("outputPer1kTokens").as_number()
                                       : value.at("output_per_1k_tokens").as_number();
  }
  pricing.currency = value.at("currency").as_string("USD");
  return pricing;
}

Value eval_assertion_to_serialized_value(const EvalAssertion& assertion) {
  if (assertion.serialized.is_object() && !assertion.serialized.as_object().empty()) {
    return assertion.serialized;
  }
  return Value::object({{"type", "custom"}, {"name", assertion.name}});
}

std::optional<EvalTextMatcher> assertion_matcher_from_value(const Value& value) {
  if (value.at("pattern").is_string()) {
    return EvalTextMatcher{.value = value.at("pattern").as_string(),
                           .regex = true,
                           .flags = value.at("flags").as_string()};
  }
  if (value.at("regex").is_string()) {
    if (auto parsed = parse_js_regex_literal(value.at("regex").as_string())) {
      if (value.at("flags").is_string() && parsed->flags.empty()) {
        parsed->flags = value.at("flags").as_string();
      }
      return parsed;
    }
    return EvalTextMatcher{.value = value.at("regex").as_string(),
                           .regex = true,
                           .flags = value.at("flags").as_string()};
  }
  if (value.at("value").is_string() && value.at("regex").as_bool()) {
    if (auto parsed = parse_js_regex_literal(value.at("value").as_string())) {
      return parsed;
    }
    return EvalTextMatcher{.value = value.at("value").as_string(),
                           .regex = true,
                           .flags = value.at("flags").as_string()};
  }
  return std::nullopt;
}

EvalAssertion eval_assertion_from_spec(const Value& value) {
  if (value.is_string()) {
    return EvalAssertion{value.as_string(), {}, Value::object({{"type", "custom"}, {"name", value.as_string()}})};
  }
  if (!value.is_object()) {
    return EvalAssertion{"custom", {}, Value::object({{"type", "custom"}, {"name", "custom"}})};
  }

  const std::string type = value.at("type").as_string(value.at("kind").as_string());
  const std::string name = value.at("name").as_string(type.empty() ? "custom" : type);
  const auto text_value = value.at("value").as_string(value.at("text").as_string());

  if (type == "outputContains" || type == "contains") {
    if (auto matcher = assertion_matcher_from_value(value)) {
      return expect_output_matches(matcher->value, matcher->flags);
    }
    return expect_output_contains(text_value);
  }
  if (type == "outputNotContains" || type == "notContains") {
    if (auto matcher = assertion_matcher_from_value(value)) {
      return expect_output_not_matches(matcher->value, matcher->flags);
    }
    return expect_output_not_contains(text_value);
  }
  if (type == "toolCalled") {
    return expect_tool_called(value.at("toolName").as_string(text_value));
  }
  if (type == "toolCallCount") {
    return expect_tool_call_count(value.at("toolName").as_string(text_value),
                                  static_cast<std::size_t>(std::max<long long>(0, value.at("count").as_integer())));
  }
  if (type == "statusStageSeen") {
    return expect_status_stage_seen(value.at("stage").as_string(text_value));
  }
  if (type == "latencyUnder") {
    return expect_latency_under(static_cast<int>(std::max<long long>(0, value.at("limitMs").as_integer())));
  }
  if (type == "tokenUnder") {
    return expect_token_under(static_cast<int>(std::max<long long>(0, value.at("limit").as_integer())));
  }
  if (type == "costUnder") {
    return expect_cost_under(value.at("limit").as_number());
  }
  if (type == "retrievalHitCount") {
    return expect_retrieval_hit_count(static_cast<std::size_t>(
        std::max<long long>(0, value.at("count").as_integer(value.at("expected").as_integer()))));
  }
  if (type == "citationCountAtLeast") {
    return expect_citation_count_at_least(static_cast<std::size_t>(
        std::max<long long>(0, value.at("min").as_integer(value.at("minCount").as_integer()))));
  }
  if (type == "approvalRequested") {
    return expect_approval_requested(value.at("toolName").as_string(text_value));
  }
  if (type == "approvalDenied") {
    return expect_approval_denied(value.at("toolName").as_string(text_value));
  }
  if (type == "jsonSchema" || type == "json_schema") {
    const auto& schema_value = value.at("schema").is_object() ? value.at("schema") : value.at("value");
    return expect_json_schema(json_schema_from_value(schema_value));
  }

  return EvalAssertion{name, {}, value};
}

Value eval_case_to_value(const EvalCase& test_case) {
  Value::Array expect;
  for (const auto& assertion : test_case.expect) {
    expect.push_back(eval_assertion_to_serialized_value(assertion));
  }
  return Value::object({{"id", test_case.id},
                        {"input", eval_case_input_value(test_case)},
                        {"sessionId", test_case.session_id},
                        {"modelSettings", model_settings_to_json_value(test_case.model_settings)},
                        {"expectContains", text_expectations_to_value(test_case.expect_contains,
                                                                       test_case.expect_contains_matchers)},
                        {"expectNotContains", text_expectations_to_value(test_case.expect_not_contains,
                                                                          test_case.expect_not_contains_matchers)},
                        {"minOutputLength", test_case.min_output_length},
                        {"expect", Value(std::move(expect))},
                        {"tags", strings_to_value(test_case.tags)},
                        {"skip", test_case.skip},
                        {"only", test_case.only},
                        {"metadata", test_case.metadata}});
}

EvalCase eval_case_from_value(const Value& value) {
  EvalCase test_case;
  test_case.id = value.at("id").as_string();
  const auto& input = value.at("input");
  if (input.is_string()) {
    test_case.input = input.as_string();
  } else if (!input.is_null()) {
    test_case.input_value = input;
    test_case.input = eval_case_text_projection(input);
  }
  test_case.session_id = value.at("sessionId").as_string(value.at("session_id").as_string());
  if (value.at("modelSettings").is_object() || value.at("model_settings").is_object()) {
    test_case.model_settings = model_settings_from_json_value(value.at("modelSettings").is_object()
                                                                 ? value.at("modelSettings")
                                                                 : value.at("model_settings"));
  }
  read_text_expectations(value.at("expectContains").is_array() ? value.at("expectContains") : value.at("expect_contains"),
                         test_case.expect_contains, test_case.expect_contains_matchers);
  read_text_expectations(value.at("expectNotContains").is_array() ? value.at("expectNotContains")
                                                                  : value.at("expect_not_contains"),
                         test_case.expect_not_contains, test_case.expect_not_contains_matchers);
  test_case.min_output_length = static_cast<std::size_t>(std::max<long long>(
      0, value.at("minOutputLength").as_integer(value.at("min_output_length").as_integer())));
  for (const auto& item : value.at("expect").as_array()) {
    test_case.expect.push_back(eval_assertion_from_spec(item));
  }
  test_case.tags = strings_from_value(value.at("tags"));
  test_case.skip = value.at("skip").as_bool();
  test_case.only = value.at("only").as_bool();
  test_case.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return test_case;
}

Value eval_suite_to_value(const EvalSuite& suite) {
  Value::Array cases;
  for (const auto& test_case : suite.cases) {
    cases.push_back(eval_case_to_value(test_case));
  }
  return Value::object({{"agent", suite.agent},
                        {"pricing", suite.pricing ? eval_pricing_to_value(*suite.pricing) : Value()},
                        {"replayDir", suite.replay_dir.empty() ? Value() : Value(suite.replay_dir.string())},
                        {"cases", Value(cases)},
                        {"metadata", suite.metadata}});
}

EvalSuite eval_suite_from_value(const Value& value) {
  EvalSuite suite;
  suite.agent = value.at("agent").as_string();
  if (value.at("pricing").is_object()) {
    suite.pricing = eval_pricing_from_value(value.at("pricing"));
  }
  suite.replay_dir = value.at("replayDir").as_string(value.at("replay_dir").as_string());
  suite.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  for (const auto& item : value.at("cases").as_array()) {
    suite.cases.push_back(eval_case_from_value(item));
  }
  return suite;
}

Value eval_case_result_to_value(const EvalCaseResult& result) {
  Value::Array assertions;
  for (const auto& assertion : result.assertions) {
    assertions.push_back(eval_assertion_result_to_value(assertion));
  }
  Value::Array permission_events;
  for (const auto& event : result.permission_events) {
    permission_events.push_back(eval_permission_event_to_value(event));
  }
  return Value::object({{"id", result.id},
                        {"passed", result.passed},
                        {"sessionId", result.session_id},
                        {"durationMs", result.duration_ms},
                        {"output", result.output},
                        {"toolCalls", strings_to_value(result.tool_calls)},
                        {"statusStages", strings_to_value(result.status_stages)},
                        {"assertions", Value(assertions)},
                        {"usage", eval_usage_to_value(result.usage)},
                        {"knowledgeHitCount", result.knowledge_hit_count},
                        {"citationCount", result.citation_count},
                        {"permissionEvents", Value(std::move(permission_events))},
                        {"replayPath", result.replay_path.empty() ? Value() : Value(result.replay_path)},
                        {"tags", strings_to_value(result.tags)},
                        {"error", result.error},
                        {"metadata", result.metadata}});
}

EvalCaseResult eval_case_result_from_value(const Value& value) {
  EvalCaseResult result;
  result.id = value.at("id").as_string();
  result.passed = value.at("passed").as_bool();
  result.session_id = value.at("sessionId").as_string(value.at("session_id").as_string());
  result.duration_ms = static_cast<int>(value.at("durationMs").as_integer(value.at("duration_ms").as_integer()));
  result.output = value.at("output").as_string();
  result.tool_calls = strings_from_value(value.at("toolCalls").is_array() ? value.at("toolCalls")
                                                                          : value.at("tool_calls"));
  result.status_stages = strings_from_value(value.at("statusStages").is_array() ? value.at("statusStages")
                                                                                : value.at("status_stages"));
  for (const auto& item : value.at("assertions").as_array()) {
    result.assertions.push_back(eval_assertion_result_from_value(item));
  }
  if (value.at("usage").is_object()) {
    result.usage = eval_usage_from_value(value.at("usage"));
  }
  result.knowledge_hit_count = static_cast<std::size_t>(std::max<long long>(
      0, value.at("knowledgeHitCount").as_integer(value.at("knowledge_hit_count").as_integer())));
  result.citation_count = static_cast<std::size_t>(std::max<long long>(
      0, value.at("citationCount").as_integer(value.at("citation_count").as_integer())));
  const auto& permission_events = value.at("permissionEvents").is_array() ? value.at("permissionEvents")
                                                                          : value.at("permission_events");
  for (const auto& item : permission_events.as_array()) {
    result.permission_events.push_back(eval_permission_event_from_value(item));
  }
  result.replay_path = value.at("replayPath").as_string(value.at("replay_path").as_string());
  result.tags = strings_from_value(value.at("tags"));
  result.error = value.at("error").as_string();
  result.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return result;
}

Value eval_report_to_value(const EvalReport& report) {
  Value::Array results;
  for (const auto& result : report.results) {
    results.push_back(eval_case_result_to_value(result));
  }
  return Value::object({{"startedAt", report.started_at},
                        {"finishedAt", report.finished_at},
                        {"durationMs", report.duration_ms},
                        {"totalCases", report.total_cases},
                        {"passedCases", report.passed_cases},
                        {"failedCases", report.failed_cases},
                        {"results", Value(results)},
                        {"metadata", report.metadata}});
}

EvalReport eval_report_from_value(const Value& value) {
  EvalReport report;
  report.started_at = value.at("startedAt").as_string(value.at("started_at").as_string());
  report.finished_at = value.at("finishedAt").as_string(value.at("finished_at").as_string());
  report.duration_ms = static_cast<int>(value.at("durationMs").as_integer(value.at("duration_ms").as_integer()));
  for (const auto& item : value.at("results").as_array()) {
    report.results.push_back(eval_case_result_from_value(item));
  }
  const auto computed_passed = static_cast<std::size_t>(std::count_if(
      report.results.begin(), report.results.end(), [](const auto& result) { return result.passed; }));
  report.total_cases = static_cast<std::size_t>(std::max<long long>(
      0, value.at("totalCases").as_integer(value.at("total_cases").as_integer(report.results.size()))));
  report.passed_cases =
      value.contains("passedCases") || value.contains("passed_cases")
          ? static_cast<std::size_t>(std::max<long long>(
                0, value.at("passedCases").as_integer(value.at("passed_cases").as_integer())))
          : computed_passed;
  report.failed_cases =
      value.contains("failedCases") || value.contains("failed_cases")
          ? static_cast<std::size_t>(std::max<long long>(
                0, value.at("failedCases").as_integer(value.at("failed_cases").as_integer())))
          : report.total_cases - std::min(report.total_cases, report.passed_cases);
  report.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return report;
}

EvalSuite load_eval_suite(const std::filesystem::path& path) {
  return eval_suite_from_value(read_json_file(path));
}

EvalReport load_eval_report(const std::filesystem::path& path) {
  return eval_report_from_value(read_json_file(path));
}

EvalSuite define_eval_suite(EvalSuite suite) {
  return suite;
}

EvalReport load_eval_baseline(const std::filesystem::path& path) {
  return load_eval_report(path);
}

std::string render_eval_report_markdown(const EvalReport& report) {
  std::ostringstream out;
  out << "# Eval Report\n\n";
  out << "- Started: " << report.started_at << "\n";
  out << "- Finished: " << report.finished_at << "\n";
  out << "- Duration: " << report.duration_ms << "ms\n";
  out << "- Passed: " << report.passed_cases << "/" << report.total_cases << "\n\n";
  out << "| Case | Passed | Tags | Duration | Tools | Status Stages | Permissions | Retrieval Hits | Total Tokens | Cost | Replay | Error |\n";
  out << "| --- | --- | --- | ---: | ---: | --- | ---: | ---: | ---: | ---: | --- | --- |\n";
  for (const auto& result : report.results) {
    std::string tag_text;
    for (const auto& tag : result.tags) {
      if (!tag_text.empty()) {
        tag_text += ",";
      }
      tag_text += tag;
    }
    std::string tool_text;
    for (const auto& tool : result.tool_calls) {
      if (!tool_text.empty()) {
        tool_text += ",";
      }
      tool_text += tool;
    }
    std::string status_text;
    for (const auto& stage : result.status_stages) {
      if (!status_text.empty()) {
        status_text += ",";
      }
      status_text += stage;
    }
    std::string cost_text;
    if (result.usage.estimated_cost) {
      std::ostringstream cost;
      cost << std::fixed << std::setprecision(6) << *result.usage.estimated_cost << " "
           << result.usage.currency;
      cost_text = cost.str();
    }
    out << "| " << result.id << " | " << (result.passed ? "yes" : "no") << " | "
        << tag_text << " | " << result.duration_ms << "ms | " << result.tool_calls.size()
        << " | " << status_text << " | " << result.permission_events.size()
        << " | " << result.knowledge_hit_count << " | " << result.usage.total_tokens
        << " | " << cost_text << " | " << result.replay_path << " | " << result.error << " |\n";
    for (const auto& assertion : result.assertions) {
      out << "| " << assertion.name << " | " << (assertion.passed ? "yes" : "no")
          << " |  |  |  |  |  |  |  |  |  | " << assertion.message << " |\n";
    }
  }
  return out.str();
}

void write_eval_report(const EvalReport& report, const std::filesystem::path& path) {
  write_text_file(path, has_markdown_extension(path) ? render_eval_report_markdown(report)
                                                     : eval_report_to_value(report).stringify(2));
}

void write_eval_baseline(const EvalReport& report, const std::filesystem::path& path) {
  write_text_file(path, eval_report_to_value(report).stringify(2));
}

EvalBaselineComparison compare_eval_baseline(const EvalReport& current, const EvalReport& baseline) {
  std::map<std::string, EvalCaseResult> before;
  std::map<std::string, EvalCaseResult> after;
  for (const auto& result : baseline.results) {
    before[result.id] = result;
  }
  for (const auto& result : current.results) {
    after[result.id] = result;
  }

  std::set<std::string> ids;
  for (const auto& [id, _] : before) {
    ids.insert(id);
  }
  for (const auto& [id, _] : after) {
    ids.insert(id);
  }

  EvalBaselineComparison comparison;
  for (const auto& id : ids) {
    const auto previous = before.find(id);
    const auto next = after.find(id);
    EvalBaselineCaseDelta delta;
    delta.id = id;
    if (previous == before.end() && next != after.end()) {
      delta.status = "added";
      delta.after = next->second;
    } else if (previous != before.end() && next == after.end()) {
      delta.status = "removed";
      delta.before = previous->second;
    } else {
      delta.before = previous->second;
      delta.after = next->second;
      delta.status = eval_case_result_to_value(previous->second).stringify(0) ==
                         eval_case_result_to_value(next->second).stringify(0)
                         ? "unchanged"
                         : "changed";
    }
    if (delta.status != "unchanged") {
      comparison.changed = true;
    }
    comparison.deltas.push_back(std::move(delta));
  }
  return comparison;
}

EvalReport run_eval_suite_impl(EvalSuite suite,
                               AgentRunner* runner,
                               std::function<std::shared_ptr<AgentRunner>(const EvalCase&)> create_runner,
                               Value config,
                               std::optional<NativeLoadedAgentConfig> loaded_config,
                               std::filesystem::path config_path,
                               std::string agent,
                               NativeConfigModuleLoader config_module_loader,
                               NativeProviderTransport provider_transport,
                               NativeProviderStreamTransport provider_stream_transport,
                               NativeProviderStreamingTransport provider_streaming_transport,
                               NativeMCPTransportFactory mcp_transport_factory,
                               NativeWebAdapters web_adapters,
                               NativeDeveloperAdapters developer_adapters,
                               NativeBrowserAdapters browser_adapters,
                               NativeLlamaCppAdapters llama_adapters,
                               std::vector<std::string> case_ids,
                               std::vector<std::string> tags,
                               bool stop_on_failure,
                               CancellationToken* cancellation) {
  const auto started = std::chrono::steady_clock::now();
  EvalReport report;
  report.started_at = now_iso8601();
  report.metadata = suite.metadata;
  throw_if_eval_cancelled(cancellation);
  const bool has_only = std::any_of(suite.cases.begin(), suite.cases.end(), [](const auto& test_case) {
    return test_case.only;
  });
  const auto resolve_options_for_agent = [&](std::string requested_agent) {
    NativeAgentAppResolveOptions options;
    options.requested_agent_id = std::move(requested_agent);
    options.provider_transport = provider_transport;
    options.provider_stream_transport = provider_stream_transport;
    options.provider_streaming_transport = provider_streaming_transport;
    options.mcp_transport_factory = mcp_transport_factory;
    options.web_adapters = web_adapters;
    options.developer_adapters = developer_adapters;
    options.browser_adapters = browser_adapters;
    options.llama_adapters = llama_adapters;
    return options;
  };
  std::size_t selected_cases = 0;
  for (const auto& test_case : suite.cases) {
    throw_if_eval_cancelled(cancellation);
    if (test_case.skip || (has_only && !test_case.only)) {
      continue;
    }
    if (!case_ids.empty() && std::find(case_ids.begin(), case_ids.end(), test_case.id) == case_ids.end()) {
      continue;
    }
    if (!tags.empty()) {
      const bool tag_match = std::any_of(test_case.tags.begin(), test_case.tags.end(), [&](const auto& tag) {
        return std::find(tags.begin(), tags.end(), tag) != tags.end();
      });
      if (!tag_match) {
        continue;
      }
    }
    throw_if_eval_cancelled(cancellation);
    ++selected_cases;

    EvalCaseResult case_result;
    case_result.id = test_case.id;
    case_result.session_id = test_case.session_id.empty() ? "eval:" + test_case.id : test_case.session_id;
    case_result.tags = test_case.tags;
    case_result.metadata = test_case.metadata;
    std::vector<AgentRunnerStreamEvent> stream_events;
    auto permission_events = std::make_shared<std::vector<EvalPermissionEvent>>();
    std::shared_ptr<AgentRunner> owned_runner;
    std::shared_ptr<NativeResolvedAgentApp> owned_app;
    AgentRunner* active_runner = nullptr;
    const auto case_started = std::chrono::steady_clock::now();
    try {
      throw_if_eval_cancelled(cancellation);
      if (create_runner) {
        owned_runner = create_runner(test_case);
        if (!owned_runner) {
          throw ConfigurationError("run_eval_suite create_runner returned null.");
        }
        active_runner = owned_runner.get();
      } else if (runner) {
        active_runner = runner;
      }
      if (!active_runner && loaded_config) {
        const auto requested_agent = agent.empty() ? suite.agent : agent;
        owned_app = std::make_shared<NativeResolvedAgentApp>(
            resolve_native_agent_app(*loaded_config, resolve_options_for_agent(requested_agent)));
        active_runner = owned_app->runner.get();
      }
      if (!active_runner && config.is_object()) {
        const auto requested_agent = agent.empty() ? suite.agent : agent;
        owned_app = std::make_shared<NativeResolvedAgentApp>(
            resolve_native_agent_app(config, resolve_options_for_agent(requested_agent)));
        active_runner = owned_app->runner.get();
      }
      if (!active_runner && !config_path.empty()) {
        const auto requested_agent = agent.empty() ? suite.agent : agent;
        owned_app = std::make_shared<NativeResolvedAgentApp>(
            load_native_agent_app(config_path,
                                  resolve_options_for_agent(requested_agent),
                                  config_module_loader));
        active_runner = owned_app->runner.get();
      }
      if (!active_runner) {
        throw ConfigurationError("run_eval_suite requires runner, create_runner, loaded_config, config, or config_path.");
      }
      const auto permission_sink_id =
          active_runner->events().register_sink([permission_events](const FrameworkEvent& event) {
            if (event.category.rfind("permission.", 0) == 0) {
              permission_events->push_back(eval_permission_event_from_framework_event(event));
            }
          });
      ScopedRunnerEventSink permission_sink{*active_runner, permission_sink_id};
      auto stream_result = stream_eval_case_input(*active_runner, test_case, case_result.session_id,
                                                  cancellation, stream_events);
      throw_if_eval_cancelled(cancellation);
      auto run = std::move(stream_result.result);
      case_result.output = run.text;
      case_result.tool_calls = collect_eval_tool_calls(run);
      append_eval_tool_calls_from_events(case_result.tool_calls, stream_events);
      case_result.status_stages = collect_eval_status_stages(stream_events);
      case_result.permission_events = *permission_events;
      case_result.usage = eval_usage_from_model_response(run.response, suite.pricing);
      case_result.knowledge_hit_count =
          run.knowledge_hits.empty() ? run.memory_hits.size() : run.knowledge_hits.size();
      case_result.citation_count = count_eval_citations(run);
      for (const auto& needle : test_case.expect_contains) {
        throw_if_eval_cancelled(cancellation);
        const bool passed = run.text.find(needle) != std::string::npos;
        case_result.assertions.push_back(
            EvalAssertionResult{"contains:" + needle, passed, passed ? "found" : "missing"});
      }
      for (const auto& matcher : test_case.expect_contains_matchers) {
        throw_if_eval_cancelled(cancellation);
        std::string error;
        const bool passed = eval_text_matches(run.text, matcher, error);
        const auto formatted = format_eval_matcher(matcher);
        case_result.assertions.push_back(EvalAssertionResult{
            "contains:" + formatted,
            passed && error.empty(),
            !error.empty() ? "invalid regex: " + error : (passed ? "found" : "missing")});
      }
      for (const auto& needle : test_case.expect_not_contains) {
        throw_if_eval_cancelled(cancellation);
        const bool passed = run.text.find(needle) == std::string::npos;
        case_result.assertions.push_back(
            EvalAssertionResult{"not-contains:" + needle, passed, passed ? "absent" : "unexpected match"});
      }
      for (const auto& matcher : test_case.expect_not_contains_matchers) {
        throw_if_eval_cancelled(cancellation);
        std::string error;
        const bool matched = eval_text_matches(run.text, matcher, error);
        const auto formatted = format_eval_matcher(matcher);
        case_result.assertions.push_back(EvalAssertionResult{
            "not-contains:" + formatted,
            !matched && error.empty(),
            !error.empty() ? "invalid regex: " + error : (!matched ? "absent" : "unexpected match")});
      }
      if (test_case.min_output_length > 0) {
        throw_if_eval_cancelled(cancellation);
        const bool passed = run.text.size() >= test_case.min_output_length;
        case_result.assertions.push_back(
            EvalAssertionResult{"min-output-length", passed, passed ? "enough output" : "too short"});
      }
      EvalAssertionContext context;
      context.test_case = &test_case;
      context.result = &run;
      context.events = stream_events;
      context.tool_calls = case_result.tool_calls;
      context.status_stages = case_result.status_stages;
      context.duration_ms =
          static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - case_started)
                               .count());
      context.usage = case_result.usage;
      context.knowledge_hit_count = case_result.knowledge_hit_count;
      context.citation_count = case_result.citation_count;
      context.permission_events = case_result.permission_events;
      context.cancellation = cancellation;
      context.runner = active_runner;
      for (const auto& assertion : test_case.expect) {
        throw_if_eval_cancelled(cancellation);
        if (!assertion.evaluate) {
          case_result.assertions.push_back(
              EvalAssertionResult{assertion.name.empty() ? "custom" : assertion.name, false,
                                  "Assertion evaluator is not configured."});
          continue;
        }
        try {
          case_result.assertions.push_back(assertion.evaluate(context));
        } catch (const std::exception& error) {
          if (cancellation && cancellation->cancelled()) {
            throw;
          }
          case_result.assertions.push_back(
              EvalAssertionResult{assertion.name.empty() ? "custom" : assertion.name, false, error.what()});
        }
        throw_if_eval_cancelled(cancellation);
      }
      case_result.passed = std::all_of(case_result.assertions.begin(), case_result.assertions.end(),
                                       [](const auto& assertion) { return assertion.passed; });
      if (case_result.assertions.empty()) {
        case_result.passed = true;
      }
    } catch (const std::exception& error) {
      if (cancellation && cancellation->cancelled()) {
        throw;
      }
      if (!stream_events.empty()) {
        case_result.status_stages = collect_eval_status_stages(stream_events);
      }
      case_result.passed = false;
      case_result.error = error.what();
    }
    case_result.duration_ms =
        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - case_started)
                             .count());
    if (case_result.permission_events.empty() && permission_events) {
      case_result.permission_events = *permission_events;
    }
    throw_if_eval_cancelled(cancellation);
    case_result.replay_path = write_eval_case_replay(suite, test_case, case_result, stream_events);
    throw_if_eval_cancelled(cancellation);
    report.results.push_back(case_result);
    if (!case_result.passed && stop_on_failure) {
      break;
    }
  }
  report.finished_at = now_iso8601();
  report.duration_ms =
      static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count());
  report.total_cases = selected_cases;
  report.passed_cases = static_cast<std::size_t>(std::count_if(report.results.begin(), report.results.end(),
                                                               [](const auto& result) { return result.passed; }));
  report.failed_cases = report.total_cases - std::min(report.total_cases, report.passed_cases);
  return report;
}

EvalReport run_eval_suite(EvalSuite suite, AgentRunner& runner, std::vector<std::string> case_ids,
                          std::vector<std::string> tags, bool stop_on_failure,
                          CancellationToken* cancellation) {
  return run_eval_suite_impl(std::move(suite),
                             &runner,
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             {},
                             std::move(case_ids),
                             std::move(tags),
                             stop_on_failure,
                             cancellation);
}

EvalReport run_eval_suite(RunEvalSuiteOptions options) {
  if (!options.replay_dir.empty()) {
    options.suite.replay_dir = options.replay_dir;
  }
  auto report = run_eval_suite_impl(std::move(options.suite),
                                    options.runner,
                                    std::move(options.create_runner),
                                    std::move(options.config),
                                    std::move(options.loaded_config),
                                    std::move(options.config_path),
                                    std::move(options.agent),
                                    std::move(options.config_module_loader),
                                    std::move(options.provider_transport),
                                    std::move(options.provider_stream_transport),
                                    std::move(options.provider_streaming_transport),
                                    std::move(options.mcp_transport_factory),
                                    std::move(options.web_adapters),
                                    std::move(options.developer_adapters),
                                    std::move(options.browser_adapters),
                                    std::move(options.llama_adapters),
                                    std::move(options.case_ids),
                                    std::move(options.tags),
                                    options.stop_on_failure,
                                    options.cancellation);
  if (!options.report_path.empty()) {
    write_eval_report(report, options.report_path);
  }
  if (!options.markdown_report_path.empty()) {
    write_eval_report(report, options.markdown_report_path);
  }
  if (options.update_baseline && !options.baseline_path.empty()) {
    write_eval_baseline(report, options.baseline_path);
  }
  return report;
}

EvalAssertion expect_output_contains(std::string needle) {
  std::string name = "outputContains(" + needle + ")";
  const std::string serialized_value = needle;
  return EvalAssertion{
      name,
      [needle = std::move(needle), name](const EvalAssertionContext& context) {
        const auto text = context.result ? context.result->text : std::string{};
        return text.find(needle) != std::string::npos
                   ? pass_eval_assertion(name, "Output matched " + needle + ".")
                   : fail_eval_assertion(name, "Output did not match " + needle + ".");
      },
      Value::object({{"type", "outputContains"}, {"value", serialized_value}}),
  };
}

EvalAssertion expect_output_not_contains(std::string needle) {
  std::string name = "outputNotContains(" + needle + ")";
  const std::string serialized_value = needle;
  return EvalAssertion{
      name,
      [needle = std::move(needle), name](const EvalAssertionContext& context) {
        const auto text = context.result ? context.result->text : std::string{};
        return text.find(needle) == std::string::npos
                   ? pass_eval_assertion(name, "Output did not match " + needle + ".")
                   : fail_eval_assertion(name, "Output unexpectedly matched " + needle + ".");
      },
      Value::object({{"type", "outputNotContains"}, {"value", serialized_value}}),
  };
}

EvalAssertion expect_output_matches(std::string pattern, std::string flags) {
  const auto formatted = js_regex_literal(pattern, flags);
  std::string name = "outputContains(" + formatted + ")";
  const std::string serialized_pattern = pattern;
  const std::string serialized_flags = flags;
  return EvalAssertion{
      name,
      [pattern = std::move(pattern), flags = std::move(flags), formatted, name](const EvalAssertionContext& context) {
        const auto text = context.result ? context.result->text : std::string{};
        std::string error;
        const bool passed = eval_text_matches(text, EvalTextMatcher{.value = pattern, .regex = true, .flags = flags},
                                              error);
        if (!error.empty()) {
          return fail_eval_assertion(name, "Invalid regex " + formatted + ": " + error + ".");
        }
        return passed ? pass_eval_assertion(name, "Output matched " + formatted + ".")
                      : fail_eval_assertion(name, "Output did not match " + formatted + ".");
      },
      Value::object({{"type", "outputContains"},
                     {"regex", formatted},
                     {"pattern", serialized_pattern},
                     {"flags", serialized_flags}}),
  };
}

EvalAssertion expect_output_not_matches(std::string pattern, std::string flags) {
  const auto formatted = js_regex_literal(pattern, flags);
  std::string name = "outputNotContains(" + formatted + ")";
  const std::string serialized_pattern = pattern;
  const std::string serialized_flags = flags;
  return EvalAssertion{
      name,
      [pattern = std::move(pattern), flags = std::move(flags), formatted, name](const EvalAssertionContext& context) {
        const auto text = context.result ? context.result->text : std::string{};
        std::string error;
        const bool matched = eval_text_matches(text, EvalTextMatcher{.value = pattern, .regex = true, .flags = flags},
                                               error);
        if (!error.empty()) {
          return fail_eval_assertion(name, "Invalid regex " + formatted + ": " + error + ".");
        }
        return !matched ? pass_eval_assertion(name, "Output did not match " + formatted + ".")
                        : fail_eval_assertion(name, "Output unexpectedly matched " + formatted + ".");
      },
      Value::object({{"type", "outputNotContains"},
                     {"regex", formatted},
                     {"pattern", serialized_pattern},
                     {"flags", serialized_flags}}),
  };
}

EvalAssertion expect_tool_called(std::string tool_name) {
  std::string name = "toolCalled(" + tool_name + ")";
  const std::string serialized_tool_name = tool_name;
  return EvalAssertion{
      name,
      [tool_name = std::move(tool_name), name](const EvalAssertionContext& context) {
        const bool found = std::find(context.tool_calls.begin(), context.tool_calls.end(), tool_name) !=
                           context.tool_calls.end();
        return found ? pass_eval_assertion(name, "Tool \"" + tool_name + "\" was called.")
                     : fail_eval_assertion(name, "Tool \"" + tool_name + "\" was not called.");
      },
      Value::object({{"type", "toolCalled"}, {"toolName", serialized_tool_name}}),
  };
}

EvalAssertion expect_tool_call_count(std::string tool_name, std::size_t count) {
  std::string name = "toolCallCount(" + tool_name + ", " + std::to_string(count) + ")";
  const std::string serialized_tool_name = tool_name;
  return EvalAssertion{
      name,
      [tool_name = std::move(tool_name), count, name](const EvalAssertionContext& context) {
        const auto actual = static_cast<std::size_t>(
            std::count(context.tool_calls.begin(), context.tool_calls.end(), tool_name));
        return actual == count
                   ? pass_eval_assertion(name, "Tool \"" + tool_name + "\" was called " +
                                                   std::to_string(actual) + " times.")
                   : fail_eval_assertion(name, "Expected \"" + tool_name + "\" " +
                                                 std::to_string(count) + " times, received " +
                                                 std::to_string(actual) + ".");
      },
      Value::object({{"type", "toolCallCount"}, {"toolName", serialized_tool_name}, {"count", static_cast<double>(count)}}),
  };
}

EvalAssertion expect_status_stage_seen(std::string stage) {
  std::string name = "statusStageSeen(" + stage + ")";
  const std::string serialized_stage = stage;
  return EvalAssertion{
      name,
      [stage = std::move(stage), name](const EvalAssertionContext& context) {
        const bool found = std::find(context.status_stages.begin(), context.status_stages.end(), stage) !=
                           context.status_stages.end();
        return found ? pass_eval_assertion(name, "Stage \"" + stage + "\" was observed.")
                     : fail_eval_assertion(name, "Stage \"" + stage + "\" was not observed.");
      },
      Value::object({{"type", "statusStageSeen"}, {"stage", serialized_stage}}),
  };
}

EvalAssertion expect_latency_under(int limit_ms) {
  const std::string name = "latencyUnder(" + std::to_string(limit_ms) + ")";
  return EvalAssertion{
      name,
      [limit_ms, name](const EvalAssertionContext& context) {
        return context.duration_ms <= limit_ms
                   ? pass_eval_assertion(name, "Latency " + std::to_string(context.duration_ms) +
                                                   "ms is within budget.")
                   : fail_eval_assertion(name, "Latency " + std::to_string(context.duration_ms) +
                                                 "ms exceeds " + std::to_string(limit_ms) + "ms.");
      },
      Value::object({{"type", "latencyUnder"}, {"limitMs", limit_ms}}),
  };
}

EvalAssertion expect_token_under(int limit) {
  const std::string name = "tokenUnder(" + std::to_string(limit) + ")";
  return EvalAssertion{
      name,
      [limit, name](const EvalAssertionContext& context) {
        return context.usage.total_tokens <= limit
                   ? pass_eval_assertion(name, "Token usage " + std::to_string(context.usage.total_tokens) +
                                                   " is within budget.")
                   : fail_eval_assertion(name, "Token usage " + std::to_string(context.usage.total_tokens) +
                                                 " exceeds " + std::to_string(limit) + ".");
      },
      Value::object({{"type", "tokenUnder"}, {"limit", limit}}),
  };
}

EvalAssertion expect_cost_under(double limit) {
  std::ostringstream label;
  label << "costUnder(" << limit << ")";
  const std::string name = label.str();
  return EvalAssertion{
      name,
      [limit, name](const EvalAssertionContext& context) {
        if (!context.usage.estimated_cost) {
          return fail_eval_assertion(name, "Estimated cost was not available. Provide suite.pricing to enable cost assertions.");
        }
        std::ostringstream value;
        value << std::fixed << std::setprecision(6) << *context.usage.estimated_cost << " "
              << context.usage.currency;
        return *context.usage.estimated_cost <= limit
                   ? pass_eval_assertion(name, "Estimated cost " + value.str() + " is within budget.")
                   : fail_eval_assertion(name, "Estimated cost " + value.str() + " exceeds limit.");
      },
      Value::object({{"type", "costUnder"}, {"limit", limit}}),
  };
}

EvalAssertion expect_retrieval_hit_count(std::size_t expected_count) {
  const std::string name = "retrievalHitCount(" + std::to_string(expected_count) + ")";
  return EvalAssertion{
      name,
      [expected_count, name](const EvalAssertionContext& context) {
        return context.knowledge_hit_count == expected_count
                   ? pass_eval_assertion(name, "Retrieval hit count is " + std::to_string(expected_count) + ".")
                   : fail_eval_assertion(name, "Expected " + std::to_string(expected_count) +
                                                 " retrieval hits, received " +
                                                 std::to_string(context.knowledge_hit_count) + ".");
      },
      Value::object({{"type", "retrievalHitCount"}, {"count", static_cast<double>(expected_count)}}),
  };
}

EvalAssertion expect_citation_count_at_least(std::size_t min_count) {
  const std::string name = "citationCountAtLeast(" + std::to_string(min_count) + ")";
  return EvalAssertion{
      name,
      [min_count, name](const EvalAssertionContext& context) {
        return context.citation_count >= min_count
                   ? pass_eval_assertion(name, "Citation count " + std::to_string(context.citation_count) +
                                                   " meets the lower bound.")
                   : fail_eval_assertion(name, "Expected at least " + std::to_string(min_count) +
                                                 " citations, received " +
                                                 std::to_string(context.citation_count) + ".");
      },
      Value::object({{"type", "citationCountAtLeast"}, {"min", static_cast<double>(min_count)}}),
  };
}

EvalAssertion expect_approval_requested(std::string tool_name) {
  const std::string suffix = tool_name.empty() ? std::string{} : "(" + tool_name + ")";
  const std::string name = "approvalRequested" + suffix;
  const std::string serialized_tool_name = tool_name;
  return EvalAssertion{
      name,
      [tool_name = std::move(tool_name), name](const EvalAssertionContext& context) {
        const bool found = std::any_of(context.permission_events.begin(), context.permission_events.end(),
                                       [&](const auto& event) {
                                         return event.category == "permission.approval_requested" &&
                                                (tool_name.empty() || event.tool_name == tool_name);
                                       });
        return found ? pass_eval_assertion(name, "Approval request was observed" +
                                                     (tool_name.empty() ? std::string{} : " for " + tool_name) + ".")
                     : fail_eval_assertion(name, "Approval request was not observed" +
                                                     (tool_name.empty() ? std::string{} : " for " + tool_name) + ".");
      },
      Value::object({{"type", "approvalRequested"}, {"toolName", serialized_tool_name}}),
  };
}

EvalAssertion expect_approval_denied(std::string tool_name) {
  const std::string suffix = tool_name.empty() ? std::string{} : "(" + tool_name + ")";
  const std::string name = "approvalDenied" + suffix;
  const std::string serialized_tool_name = tool_name;
  return EvalAssertion{
      name,
      [tool_name = std::move(tool_name), name](const EvalAssertionContext& context) {
        const bool found = std::any_of(context.permission_events.begin(), context.permission_events.end(),
                                       [&](const auto& event) {
                                         return event.category == "permission.denied" &&
                                                (tool_name.empty() || event.tool_name == tool_name);
                                       });
        return found ? pass_eval_assertion(name, "Permission denial was observed" +
                                                     (tool_name.empty() ? std::string{} : " for " + tool_name) + ".")
                     : fail_eval_assertion(name, "Permission denial was not observed" +
                                                     (tool_name.empty() ? std::string{} : " for " + tool_name) + ".");
      },
      Value::object({{"type", "approvalDenied"}, {"toolName", serialized_tool_name}}),
  };
}

EvalAssertion expect_json_schema(JsonSchema schema) {
  const std::string name = "jsonSchema";
  Value serialized_schema = json_schema_to_value(schema);
  return EvalAssertion{
      name,
      [schema = std::move(schema), name](const EvalAssertionContext& context) {
        if (!context.result) {
          return fail_eval_assertion(name, "No run result was available.");
        }
        auto candidate = eval_structured_candidate(*context.result);
        if (!candidate) {
          return fail_eval_assertion(name, "No structured output was found and result text was not valid JSON.");
        }
        try {
          assert_json_schema(schema, *candidate);
          return pass_eval_assertion(name, "Structured output satisfied the schema.");
        } catch (const SchemaValidationError& error) {
          return fail_eval_assertion(name, format_schema_validation_error(error));
        } catch (const std::exception& error) {
          return fail_eval_assertion(name, error.what());
        }
      },
      Value::object({{"type", "jsonSchema"}, {"schema", std::move(serialized_schema)}}),
  };
}

EvalAssertion expect_custom(std::string name, EvalAssertionFunction evaluate) {
  const std::string serialized_name = name;
  return EvalAssertion{std::move(name), std::move(evaluate),
                       Value::object({{"type", "custom"}, {"name", serialized_name}})};
}

EvalAssertion expect_llm_judge(std::string name, std::string rubric) {
  const std::string assertion_name = name.empty() ? "llm-judge" : name;
  const std::string serialized_rubric = rubric;
  return EvalAssertion{
      assertion_name,
      [name = assertion_name, rubric = std::move(rubric)](
          const EvalAssertionContext& context) -> EvalAssertionResult {
        if (!context.runner) {
          return EvalAssertionResult{name, false, "no runner available for LLM-as-judge"};
        }
        if (!context.result) {
          return EvalAssertionResult{name, false, "no run result to judge"};
        }
        auto critique = context.runner->models().critique();
        auto primary = context.runner->models().primary();
        std::shared_ptr<ChatModelAdapter> judge = critique ? critique : primary;
        if (!judge) {
          return EvalAssertionResult{name, false, "no adapter available for LLM-as-judge"};
        }
        if (!critique) {
          if (auto* bus = context.runner->events().bus()) {
            bus->publish("eval.self_grading_detected", ExecutionTarget::Run,
                         Value::object({
                             {"assertion", name},
                             {"reason",
                              std::string("LLM-judge assertion fell back to the runner's main adapter "
                                          "because no critique_adapter is configured.")},
                         }));
          }
        }
        const std::string user_text =
            "You are a strict grader. Read the rubric and the assistant output, then reply "
            "with PASS or FAIL as the first token, followed by a one-line justification.\n\n"
            "Rubric:\n" + rubric + "\n\nAssistant output:\n" + context.result->text + "\n";
        std::vector<AgentMessage> messages;
        messages.push_back(create_message(MessageRole::System,
                                          "You are an impartial judge. Output PASS or FAIL first."));
        messages.push_back(create_message(MessageRole::User, user_text));
        try {
          AgentOutput response = judge->generate(GenerateParams{.messages = messages});
          std::string text = response.text;
          // Strip leading whitespace/punctuation.
          std::size_t start = 0;
          while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) ++start;
          const bool pass = text.size() >= start + 4 &&
                            (text.compare(start, 4, "PASS") == 0 || text.compare(start, 4, "pass") == 0);
          return EvalAssertionResult{name, pass, text};
        } catch (const std::exception& error) {
          return EvalAssertionResult{name, false, std::string("judge error: ") + error.what()};
        }
      },
      Value::object({{"type", "llmJudge"}, {"name", assertion_name}, {"rubric", serialized_rubric}}),
  };
}
}  // namespace agent
