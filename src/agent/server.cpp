#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

namespace agent {

namespace {

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string upper_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
  if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
  return -1;
}

std::string url_decode(const std::string& value) {
  std::string output;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '%' && index + 2 < value.size()) {
      const int high = hex_value(value[index + 1]);
      const int low = hex_value(value[index + 2]);
      if (high >= 0 && low >= 0) {
        output.push_back(static_cast<char>((high << 4) | low));
        index += 2;
        continue;
      }
    }
    output.push_back(value[index] == '+' ? ' ' : value[index]);
  }
  return output;
}

std::string header_value(const HttpRequest& request, const std::string& name) {
  const auto normalized = lower_copy(name);
  for (const auto& [key, value] : request.headers) {
    if (lower_copy(key) == normalized) {
      return value;
    }
  }
  return {};
}

std::string bearer_token(const HttpRequest& request, const std::string& header_name) {
  const auto value = header_value(request, header_name.empty() ? "authorization" : header_name);
  const std::string prefix = "bearer ";
  const auto lower = lower_copy(value);
  if (lower.rfind(prefix, 0) != 0) {
    return {};
  }
  return value.substr(prefix.size());
}

bool contains_string(const std::vector<std::string>& values, const std::string& needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

// Compare two secrets in time independent of where the first mismatching byte
// is. Every byte (and the length delta) is folded into a single accumulator so
// there is no early-out for an attacker to time, unlike `==` / std::find.
bool constant_time_equals(const std::string& a, const std::string& b) {
  unsigned char diff = static_cast<unsigned char>(a.size() ^ b.size());
  const std::size_t n = a.size() > b.size() ? a.size() : b.size();
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0u;
    const unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
    diff = static_cast<unsigned char>(diff | (ca ^ cb));
  }
  return diff == 0;
}

// Membership test for a secret against a configured list, using a constant-time
// per-entry compare and scanning the whole list (no short-circuit) so the match
// position isn't timing-observable.
bool contains_secret(const std::vector<std::string>& values, const std::string& candidate) {
  bool found = false;
  for (const auto& value : values) {
    if (constant_time_equals(value, candidate)) {
      found = true;
    }
  }
  return found;
}

bool path_exempt(const std::string& path, const std::vector<std::string>& exempt_paths) {
  return contains_string(exempt_paths, path);
}

std::string path_from_url(const std::string& url) {
  std::string value = url.empty() ? "/" : url;
  const auto scheme = value.find("://");
  if (scheme != std::string::npos) {
    const auto path_start = value.find('/', scheme + 3);
    value = path_start == std::string::npos ? "/" : value.substr(path_start);
  }
  const auto query = value.find('?');
  if (query != std::string::npos) {
    value = value.substr(0, query);
  }
  const auto hash = value.find('#');
  if (hash != std::string::npos) {
    value = value.substr(0, hash);
  }
  return value.empty() ? "/" : value;
}

std::map<std::string, std::string> query_from_url(const std::string& url) {
  std::map<std::string, std::string> query;
  const auto question = url.find('?');
  if (question == std::string::npos) {
    return query;
  }
  const auto hash = url.find('#', question + 1);
  const auto raw = url.substr(question + 1, hash == std::string::npos ? std::string::npos : hash - question - 1);
  std::stringstream stream(raw);
  std::string item;
  while (std::getline(stream, item, '&')) {
    if (item.empty()) {
      continue;
    }
    const auto equals = item.find('=');
    const auto key = url_decode(equals == std::string::npos ? item : item.substr(0, equals));
    const auto value = equals == std::string::npos ? std::string{} : url_decode(item.substr(equals + 1));
    query[key] = value;
  }
  return query;
}

std::string join_values(const std::vector<std::string>& values, const std::string& separator) {
  std::ostringstream out;
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index > 0) {
      out << separator;
    }
    out << values[index];
  }
  return out.str();
}

std::string trim_copy(const std::string& value) {
  auto begin = value.begin();
  while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
    ++begin;
  }
  auto end = value.end();
  while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
    --end;
  }
  return std::string(begin, end);
}

bool valid_trace_header_char(char ch) {
  const auto value = static_cast<unsigned char>(ch);
  return std::isalnum(value) != 0 || ch == '_' || ch == '.' || ch == ':' || ch == '-';
}

std::string normalize_trace_header(const std::string& value) {
  const auto trimmed = trim_copy(value);
  if (trimmed.empty() || trimmed.size() > 128) {
    return {};
  }
  if (!std::all_of(trimmed.begin(), trimmed.end(), valid_trace_header_char)) {
    return {};
  }
  return trimmed;
}

AgentServerRequestTrace ensure_request_trace(const HttpRequest& request,
                                             const AgentServerTracingConfig& config) {
  const auto request_id =
      normalize_trace_header(header_value(request, config.request_id_header.empty()
                                                       ? "x-request-id"
                                                       : config.request_id_header));
  const auto trace_id =
      normalize_trace_header(header_value(request, config.trace_id_header.empty()
                                                       ? "x-trace-id"
                                                       : config.trace_id_header));
  AgentServerRequestTrace trace;
  trace.request_id = request_id.empty() ? generate_uuid() : request_id;
  trace.trace_context = create_trace_context(TraceContext{
      .trace_id = trace_id.empty() ? trace.request_id : trace_id,
      .span_name = "http.request",
  });
  return trace;
}

void apply_trace_headers(HttpResponse& response, const AgentServerRequestTrace& trace,
                         const AgentServerTracingConfig& config) {
  if (!config.response_headers) {
    return;
  }
  response.headers["x-request-id"] = trace.request_id;
  response.headers["x-trace-id"] = trace.trace_context.trace_id;
}

void add_optional_audit_field(Value& value, const std::string& key, const std::string& field) {
  if (!field.empty()) {
    value[key] = field;
  }
}

void merge_headers(const HttpResponse& source, HttpResponse& target) {
  for (const auto& [key, value] : source.headers) {
    target.headers[key] = value;
  }
}

Value strings_to_value(const std::vector<std::string>& values) {
  Value::Array array;
  array.reserve(values.size());
  for (const auto& value : values) {
    array.push_back(value);
  }
  return Value(std::move(array));
}

std::vector<std::string> strings_from_value(const Value& value) {
  std::vector<std::string> output;
  for (const auto& item : value.as_array()) {
    if (item.is_string()) {
      output.push_back(item.as_string());
    }
  }
  return output;
}

Value tool_call_to_value(const ToolCall& tool_call) {
  return Value::object({{"id", tool_call.id},
                        {"name", tool_call.name},
                        {"arguments", tool_call.arguments}});
}

ToolCall tool_call_from_value(const Value& value) {
  return ToolCall{
      .id = value.at("id").as_string(),
      .name = value.at("name").as_string(),
      .arguments = value.at("arguments").is_object() ? value.at("arguments") : Value::object({}),
  };
}

TraceContext trace_context_from_value(const Value& value) {
  return TraceContext{
      .trace_id = value.at("traceId").as_string(value.at("trace_id").as_string()),
      .span_id = value.at("spanId").as_string(value.at("span_id").as_string()),
      .parent_span_id = value.at("parentSpanId").as_string(value.at("parent_span_id").as_string()),
      .span_name = value.at("spanName").as_string(value.at("span_name").as_string()),
      .run_id = value.at("runId").as_string(value.at("run_id").as_string()),
      .workflow_run_id = value.at("workflowRunId").as_string(value.at("workflow_run_id").as_string()),
  };
}

Value tool_execution_context_to_value(const ToolExecutionContext& context) {
  return Value::object({{"services", context.services},
                        {"attributes", context.attributes},
                        {"iteration", context.iteration},
                        {"trace", trace_context_to_value(context.trace_context)},
                        {"toolCall", context.tool_call ? tool_call_to_value(*context.tool_call) : Value()}});
}

ToolExecutionContext tool_execution_context_from_value(const Value& value) {
  ToolExecutionContext context;
  if (!value.is_object()) {
    return context;
  }
  context.services = value.at("services").is_object() ? value.at("services") : value;
  context.attributes = value.at("attributes").is_object() ? value.at("attributes") : Value::object({});
  context.iteration = static_cast<int>(value.at("iteration").as_integer());
  context.trace_context = trace_context_from_value(value.at("trace"));
  if (value.at("toolCall").is_object()) {
    context.tool_call = tool_call_from_value(value.at("toolCall"));
  }
  return normalize_tool_execution_context(std::move(context));
}

std::string input_to_text(const Value& value) {
  return value.is_string() ? value.as_string() : safe_json_stringify(value);
}

ModelSettings model_settings_from_server_value(const Value& value) {
  ModelSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  settings.model = value.at("model").as_string();
  if (value.contains("temperature")) {
    settings.temperature = value.at("temperature").as_number();
  }
  if (value.contains("maxOutputTokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("maxOutputTokens").as_integer());
  } else if (value.contains("max_output_tokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("max_output_tokens").as_integer());
  }
  settings.extra = value.at("extra").is_object() ? value.at("extra") : Value::object({});
  if (value.contains("reasoning")) {
    assert_reasoning_settings(value.at("reasoning"), "modelSettings.reasoning");
    settings.reasoning = reasoning_settings_from_json_value(value.at("reasoning"));
  }
  return settings;
}

void validate_server_model_settings(const Value& value) {
  if (!value.is_object() || !value.contains("reasoning")) {
    return;
  }
  try {
    assert_reasoning_settings(value.at("reasoning"), "modelSettings.reasoning");
  } catch (const ConfigurationError& error) {
    throw HttpRequestError(400, error.what(), Value::object({{"error", error.what()},
                                                            {"field", "modelSettings.reasoning"}}));
  }
}

Value model_response_summary(const ModelResponse& response) {
  return Value::object({{"id", response.id.empty() ? Value() : Value(response.id)},
                        {"provider", response.provider},
                        {"model", response.model},
                        {"text", response.text},
                        {"finishReason", response.finish_reason},
                        {"raw", response.raw}});
}

Value runner_result_summary(const AgentRunnerRunResult& result) {
  Value::Array messages;
  for (const auto& message : result.messages) {
    messages.push_back(agent_message_to_value(message));
  }
  Value::Array tool_calls;
  for (const auto& entry : result.trace) {
    for (const auto& tool_result : entry.tool_results) {
      tool_calls.push_back(Value::object({{"id", tool_result.tool_call.id},
                                          {"name", tool_result.tool_call.name},
                                          {"ok", tool_result.ok},
                                          {"error", tool_result.error.empty() ? Value() : Value(tool_result.error)}}));
    }
  }
  Value::Array memory_hits;
  Value::Array knowledge_hits;
  for (const auto& hit : result.memory_hits) {
    memory_hits.push_back(retrieved_memory_to_value(hit));
    if (result.knowledge_hits.empty() && hit.metadata.at("citation").is_object()) {
      knowledge_hits.push_back(Value::object({{"id", hit.id},
                                              {"content", hit.content},
                                              {"score", hit.score},
                                              {"citation", hit.metadata.at("citation")},
                                              {"metadata", hit.metadata}}));
    }
  }
  if (!result.knowledge_hits.empty()) {
    for (const auto& hit : result.knowledge_hits) {
      knowledge_hits.push_back(knowledge_search_hit_to_value(hit));
    }
  }
  Value summary = Value::object({{"sessionId", result.session_id},
                                 {"iterationCount", result.iteration_count},
                                 {"text", result.text},
                                 {"response", model_response_summary(result.response)},
                                 {"messages", Value(std::move(messages))},
                                 {"terminationReason", to_string(result.termination_reason)},
                                 {"toolCalls", Value(std::move(tool_calls))},
                                 {"memoryHits", Value(std::move(memory_hits))},
                                 {"knowledgeHits", Value(std::move(knowledge_hits))},
                                 {"knowledgeDebug", result.knowledge_debug}});
  if (result.plan) {
    summary["plan"] = execution_plan_to_value(*result.plan);
  }
  return summary;
}

std::vector<Value> replay_events_from_result(const AgentRunnerRunResult& result, const Value& governed_result) {
  std::vector<Value> events;
  for (const auto& entry : result.trace) {
    if (entry.type == "model") {
      events.push_back(Value::object({{"type", "model"},
                                      {"iteration", entry.iteration},
                                      {"response", model_response_summary(entry.response)}}));
      continue;
    }
    if (entry.type == "tools") {
      for (const auto& tool_result : entry.tool_results) {
        events.push_back(Value::object({
            {"type", "tool-call"},
            {"iteration", entry.iteration},
            {"toolCall", Value::object({{"id", tool_result.tool_call.id},
                                        {"name", tool_result.tool_call.name},
                                        {"arguments", tool_result.tool_call.arguments}})},
            {"ok", tool_result.ok},
            {"output", tool_result.output},
            {"error", tool_result.error.empty() ? Value() : Value(tool_result.error)},
        }));
      }
    }
  }
  events.push_back(Value::object({{"type", "done"}, {"result", governed_result}}));
  return events;
}

Value status_codes_to_value(const std::map<std::string, int>& status_codes) {
  Value::Object object;
  for (const auto& [key, value] : status_codes) {
    object[key] = value;
  }
  return Value(std::move(object));
}

Value double_map_to_value(const std::map<std::string, double>& values) {
  Value::Object object;
  for (const auto& [key, value] : values) {
    object[key] = value;
  }
  return Value(std::move(object));
}

Value buckets_to_value(const std::map<std::string, AgentServerMetricsBucket>& buckets) {
  Value::Object object;
  for (const auto& [key, bucket] : buckets) {
    object[key] = agent_server_metrics_bucket_to_value(bucket);
  }
  return Value(std::move(object));
}

JsonSchema json_schema_from_server_value(const Value& value) {
  return json_schema_from_value(value);
}

Value json_schema_to_server_value(const JsonSchema& schema) {
  return json_schema_to_value(schema);
}

AgentServerCitationGovernanceConfig citation_governance_from_value(const Value& value) {
  AgentServerCitationGovernanceConfig config;
  if (value.is_bool()) {
    config.mode = value.as_bool() ? "require" : "off";
    return config;
  }
  if (!value.is_object()) {
    return config;
  }
  config.mode = value.at("mode").as_string("require");
  if (value.at("appendToText").is_bool()) {
    config.append_to_text = value.at("appendToText").as_bool();
  } else if (value.at("append_to_text").is_bool()) {
    config.append_to_text = value.at("append_to_text").as_bool();
  }
  if (value.at("maxSources").is_number()) {
    config.max_sources = static_cast<int>(value.at("maxSources").as_integer(3));
  } else if (value.at("max_sources").is_number()) {
    config.max_sources = static_cast<int>(value.at("max_sources").as_integer(3));
  }
  config.header = value.at("header").as_string("Sources");
  return config;
}

std::string optional_string_field(const Value& body, const std::string& field) {
  if (!body.contains(field) || body.at(field).is_null()) {
    return {};
  }
  if (!body.at(field).is_string()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a string.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a string."},
                                          {"field", field}}));
  }
  return body.at(field).as_string();
}

std::string optional_string_field_strict(const Value& body, const std::string& field) {
  if (!body.contains(field)) {
    return {};
  }
  if (!body.at(field).is_string()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a string.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a string."},
                                          {"field", field}}));
  }
  return body.at(field).as_string();
}

Value optional_object_field(const Value& body, const std::string& field) {
  if (!body.contains(field) || body.at(field).is_null()) {
    return Value::object({});
  }
  if (!body.at(field).is_object()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a JSON object."},
                                          {"field", field}}));
  }
  return body.at(field);
}

Value optional_object_field_strict(const Value& body, const std::string& field) {
  if (!body.contains(field)) {
    return Value::object({});
  }
  if (!body.at(field).is_object()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a JSON object."},
                                          {"field", field}}));
  }
  return body.at(field);
}

bool optional_bool_field_strict(const Value& body, const std::string& field, bool fallback = false) {
  if (!body.contains(field)) {
    return fallback;
  }
  if (!body.at(field).is_bool()) {
    throw HttpRequestError(400, "Request body field \"" + field + "\" must be a boolean.",
                           Value::object({{"error", "Request body field \"" + field + "\" must be a boolean."},
                                          {"field", field}}));
  }
  return body.at(field).as_bool();
}

std::optional<std::string> optional_reason_field(const Value& body) {
  if (!body.contains("reason")) {
    return std::nullopt;
  }
  if (!body.at("reason").is_string()) {
    throw HttpRequestError(400, "Request body field \"reason\" must be a string.",
                           Value::object({{"error", "Request body field \"reason\" must be a string."},
                                          {"field", "reason"}}));
  }
  return body.at("reason").as_string();
}

struct TaskAccessFields {
  std::string owner_api_key_id;
  std::string tenant_id;
  Value metadata = Value::object({});
};

std::string access_scope_or_throw(const std::string& scope, const std::string& runtime_name) {
  if (scope.empty() || scope == "api-key") {
    return "api-key";
  }
  if (scope == "tenant") {
    return "tenant";
  }
  throw HttpRequestError(500, "Invalid " + runtime_name + " access scope.",
                         Value::object({{"error", "Invalid " + runtime_name + " access scope."},
                                        {"scope", scope}}));
}

std::string task_tenant_metadata_key(const AgentServerOptions& options) {
  if (!options.tasks || options.tasks->access.tenant_metadata_key.empty()) {
    return "tenantId";
  }
  return options.tasks->access.tenant_metadata_key;
}

std::string autonomous_tenant_metadata_key(const AgentServerOptions& options) {
  if (!options.autonomous || options.autonomous->access.tenant_metadata_key.empty()) {
    return "tenantId";
  }
  return options.autonomous->access.tenant_metadata_key;
}

std::string api_key_metadata_string(const AgentServerApiKey& api_key,
                                    const std::string& key) {
  if (!api_key.metadata.is_object() || !api_key.metadata.contains(key)) {
    return {};
  }
  const auto& value = api_key.metadata.at(key);
  if (!value.is_string() || value.as_string().empty()) {
    throw HttpRequestError(500, "API key metadata \"" + key + "\" must be a non-empty string.",
                           Value::object({{"error", "API key metadata \"" + key +
                                                       "\" must be a non-empty string."}}));
  }
  return value.as_string();
}

std::string task_api_key_tenant_id(const AgentServerAccessContext& access,
                                   const AgentServerOptions& options) {
  if (!access.api_key) {
    return {};
  }
  return api_key_metadata_string(*access.api_key, task_tenant_metadata_key(options));
}

std::string autonomous_api_key_tenant_id(const AgentServerAccessContext& access,
                                         const AgentServerOptions& options) {
  if (!access.api_key) {
    return {};
  }
  return api_key_metadata_string(*access.api_key, autonomous_tenant_metadata_key(options));
}

void assert_task_route_access(const AgentServerAccessContext& access,
                              const AgentServerOptions& options) {
  if (options.tasks && options.tasks->access.require_api_key && !access.api_key) {
    throw HttpRequestError(403, "Task runtime requires API key authentication.",
                           Value::object({{"error", "Task runtime requires API key authentication."}}));
  }
}

TaskScopeFilter task_access_filter(const AgentServerAccessContext& access,
                                   const AgentServerOptions& options) {
  TaskScopeFilter filter;
  if (!access.api_key) {
    return filter;
  }
  const auto scope = access_scope_or_throw(options.tasks ? options.tasks->access.scope : "api-key", "task");
  if (scope == "tenant") {
    const auto tenant_id = task_api_key_tenant_id(access, options);
    if (tenant_id.empty()) {
      throw HttpRequestError(403, "Task tenant scope requires API key tenant metadata.",
                             Value::object({{"error", "Task tenant scope requires API key tenant metadata."},
                                            {"field", task_tenant_metadata_key(options)}}));
    }
    filter.tenant_id = tenant_id;
    return filter;
  }
  filter.owner_api_key_id = access.api_key->id;
  return filter;
}

void merge_task_access_filter(TaskScopeFilter& target, TaskScopeFilter source) {
  if (source.owner_api_key_id) {
    target.owner_api_key_id = std::move(source.owner_api_key_id);
  }
  if (source.tenant_id) {
    target.tenant_id = std::move(source.tenant_id);
  }
}

bool can_access_task_snapshot(const TaskStoreSnapshot& snapshot,
                              const AgentServerAccessContext& access,
                              const AgentServerOptions& options) {
  if (!access.api_key) {
    return true;
  }
  const auto scope = access_scope_or_throw(options.tasks ? options.tasks->access.scope : "api-key", "task");
  if (scope == "tenant") {
    const auto tenant_id = task_api_key_tenant_id(access, options);
    return !tenant_id.empty() && snapshot.task.tenant_id == tenant_id;
  }
  return snapshot.task.owner_api_key_id == access.api_key->id;
}

void assert_task_snapshot_access(const TaskStoreSnapshot& snapshot,
                                 const AgentServerAccessContext& access,
                                 const AgentServerOptions& options,
                                 const std::string& task_id) {
  if (!can_access_task_snapshot(snapshot, access, options)) {
    throw HttpRequestError(404, "Task not found: " + task_id,
                           Value::object({{"error", "Task not found: " + task_id}}));
  }
}

TaskAccessFields build_task_access_fields(Value metadata,
                                          const AgentServerAccessContext& access,
                                          const AgentServerOptions& options) {
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  TaskAccessFields fields{.metadata = std::move(metadata)};
  if (!access.api_key) {
    return fields;
  }

  if (fields.metadata.contains("ownerApiKeyId") &&
      fields.metadata.at("ownerApiKeyId") != Value(access.api_key->id)) {
    throw HttpRequestError(403, "Task metadata \"ownerApiKeyId\" must match API key owner.",
                           Value::object({{"error", "Task metadata \"ownerApiKeyId\" must match API key owner."},
                                          {"field", "ownerApiKeyId"}}));
  }

  const auto tenant_key = task_tenant_metadata_key(options);
  const auto tenant_id = task_api_key_tenant_id(access, options);
  if (!tenant_id.empty() && fields.metadata.contains(tenant_key) &&
      fields.metadata.at(tenant_key) != Value(tenant_id)) {
    throw HttpRequestError(403, "Task metadata \"" + tenant_key + "\" must match API key tenant.",
                           Value::object({{"error", "Task metadata \"" + tenant_key +
                                                       "\" must match API key tenant."},
                                          {"field", tenant_key}}));
  }

  fields.owner_api_key_id = access.api_key->id;
  fields.tenant_id = tenant_id;
  fields.metadata["ownerApiKeyId"] = access.api_key->id;
  if (!tenant_id.empty()) {
    fields.metadata[tenant_key] = tenant_id;
  }
  return fields;
}

void assert_autonomous_route_access(const AgentServerAccessContext& access,
                                    const AgentServerOptions& options) {
  if (options.autonomous && options.autonomous->access.require_api_key && !access.api_key) {
    throw HttpRequestError(403, "Autonomous runtime requires API key authentication.",
                           Value::object({{"error", "Autonomous runtime requires API key authentication."}}));
  }
}

Value build_autonomous_access_metadata(Value metadata,
                                       const AgentServerAccessContext& access,
                                       const AgentServerOptions& options) {
  if (!metadata.is_object()) {
    metadata = Value::object({});
  }
  if (!access.api_key) {
    return metadata;
  }
  if (metadata.contains("ownerApiKeyId") && metadata.at("ownerApiKeyId") != Value(access.api_key->id)) {
    throw HttpRequestError(
        403,
        "Autonomous metadata ownerApiKeyId does not match authenticated API key.",
        Value::object({{"error", "Autonomous metadata ownerApiKeyId does not match authenticated API key."}}));
  }
  const auto tenant_key = autonomous_tenant_metadata_key(options);
  const auto tenant_id = autonomous_api_key_tenant_id(access, options);
  if (!tenant_id.empty() && metadata.contains(tenant_key) && metadata.at(tenant_key) != Value(tenant_id)) {
    throw HttpRequestError(403,
                           "Autonomous metadata " + tenant_key +
                               " does not match authenticated API key.",
                           Value::object({{"error", "Autonomous metadata " + tenant_key +
                                                       " does not match authenticated API key."},
                                          {"field", tenant_key}}));
  }
  metadata["ownerApiKeyId"] = access.api_key->id;
  if (!tenant_id.empty()) {
    metadata[tenant_key] = tenant_id;
  }
  return metadata;
}

Value autonomous_access_filter(const AgentServerAccessContext& access,
                               const AgentServerOptions& options) {
  if (!access.api_key) {
    return Value::object({});
  }
  const auto scope = access_scope_or_throw(options.autonomous ? options.autonomous->access.scope : "api-key",
                                           "autonomous");
  if (scope == "tenant") {
    const auto tenant_id = autonomous_api_key_tenant_id(access, options);
    if (tenant_id.empty()) {
      throw HttpRequestError(403, "Autonomous tenant scope requires API key tenant metadata.",
                             Value::object({{"error", "Autonomous tenant scope requires API key tenant metadata."},
                                            {"field", autonomous_tenant_metadata_key(options)}}));
    }
    return Value::object({{autonomous_tenant_metadata_key(options), tenant_id}});
  }
  return Value::object({{"ownerApiKeyId", access.api_key->id}});
}

bool can_access_autonomous_snapshot(const AutonomousRunSnapshot& snapshot,
                                    const AgentServerAccessContext& access,
                                    const AgentServerOptions& options) {
  if (!access.api_key) {
    return true;
  }
  const auto scope = access_scope_or_throw(options.autonomous ? options.autonomous->access.scope : "api-key",
                                           "autonomous");
  if (scope == "tenant") {
    const auto tenant_id = autonomous_api_key_tenant_id(access, options);
    const auto tenant_key = autonomous_tenant_metadata_key(options);
    return !tenant_id.empty() &&
           snapshot.run.metadata.is_object() &&
           snapshot.run.metadata.at(tenant_key) == Value(tenant_id);
  }
  return snapshot.run.metadata.is_object() &&
         snapshot.run.metadata.at("ownerApiKeyId") == Value(access.api_key->id);
}

void assert_autonomous_snapshot_access(const AutonomousRunSnapshot& snapshot,
                                       const AgentServerAccessContext& access,
                                       const AgentServerOptions& options,
                                       const std::string& run_id) {
  if (!can_access_autonomous_snapshot(snapshot, access, options)) {
    throw HttpRequestError(404, "Autonomous run not found: " + run_id,
                           Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
}

struct NormalizedPiiPolicy {
  bool enabled = false;
  std::vector<std::string> detectors;
  std::string replacement = "[REDACTED]";
};

struct NormalizedCitationPolicy {
  std::string mode = "off";
  bool append_to_text = false;
  int max_sources = 3;
  std::string header = "Sources";
};

struct ResolvedGovernancePolicy {
  std::optional<NormalizedPiiPolicy> pii;
  NormalizedCitationPolicy citations;
  std::optional<JsonSchema> output_schema;
};

struct GovernedRun {
  std::string output;
  Value result = Value::object({});
  AgentServerGovernanceSummary summary;
};

std::optional<NormalizedPiiPolicy> normalize_pii_policy(
    const std::optional<AgentServerPiiGovernanceConfig>& policy) {
  if (!policy || !policy->enabled) {
    return std::nullopt;
  }
  NormalizedPiiPolicy normalized;
  normalized.enabled = true;
  normalized.detectors = policy->detectors.empty()
                             ? std::vector<std::string>{"email", "phone", "credit-card"}
                             : policy->detectors;
  normalized.replacement = policy->replacement.empty() ? "[REDACTED]" : policy->replacement;
  return normalized;
}

NormalizedCitationPolicy normalize_citation_policy(
    const std::optional<AgentServerCitationGovernanceConfig>& policy,
    bool has_output_schema) {
  if (!policy) {
    return {};
  }
  NormalizedCitationPolicy normalized;
  normalized.mode = policy->mode.empty() ? "require" : policy->mode;
  normalized.max_sources = policy->max_sources <= 0 ? 3 : policy->max_sources;
  normalized.header = policy->header.empty() ? "Sources" : policy->header;
  normalized.append_to_text = policy->append_to_text.value_or(normalized.mode == "require" && !has_output_schema);
  return normalized;
}

std::optional<AgentServerRequestGovernanceConfig> request_governance_from_body(const Value& body) {
  if (!body.contains("governance") || body.at("governance").is_null()) {
    return std::nullopt;
  }
  if (!body.at("governance").is_object()) {
    throw HttpRequestError(400, "Request body field \"governance\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"governance\" must be a JSON object."},
                                          {"field", "governance"}}));
  }
  AgentServerRequestGovernanceConfig config;
  const auto& governance = body.at("governance");
  if (governance.contains("citations")) {
    config.citations = citation_governance_from_value(governance.at("citations"));
  }
  if (governance.at("outputSchema").is_object()) {
    config.output_schema = json_schema_from_server_value(governance.at("outputSchema"));
  }
  return config;
}

ResolvedGovernancePolicy resolve_governance_policy(
    const std::optional<AgentServerGovernanceConfig>& options,
    const std::optional<AgentServerRequestGovernanceConfig>& request) {
  std::optional<JsonSchema> output_schema;
  if (request && request->output_schema) {
    output_schema = request->output_schema;
  } else if (options && options->output_schema) {
    output_schema = options->output_schema;
  }
  std::optional<AgentServerCitationGovernanceConfig> citations;
  if (request && request->citations) {
    citations = request->citations;
  } else if (options && options->citations) {
    citations = options->citations;
  }
  return ResolvedGovernancePolicy{
      .pii = options ? normalize_pii_policy(options->pii) : std::nullopt,
      .citations = normalize_citation_policy(citations, output_schema.has_value()),
      .output_schema = std::move(output_schema),
  };
}

Value normalized_citation_policy_to_value(const NormalizedCitationPolicy& policy) {
  return Value::object({{"mode", policy.mode},
                        {"appendToText", policy.append_to_text},
                        {"maxSources", policy.max_sources},
                        {"header", policy.header}});
}

std::string redact_text(std::string value, const NormalizedPiiPolicy& policy) {
  const auto replace_if_enabled = [&](const std::string& detector, const std::regex& pattern) {
    if (contains_string(policy.detectors, detector)) {
      value = std::regex_replace(value, pattern, policy.replacement);
    }
  };
  replace_if_enabled("email", std::regex(R"([A-Z0-9._%+-]+@[A-Z0-9.-]+\.[A-Z]{2,})",
                                         std::regex_constants::icase));
  replace_if_enabled("phone", std::regex(R"((\+?\d[\d().\s-]{7,}\d))"));
  replace_if_enabled("credit-card", std::regex(R"(\b(?:\d[ -]*?){13,19}\b)"));
  return value;
}

Value redact_value_strings(const Value& value, const NormalizedPiiPolicy& policy) {
  if (value.is_string()) {
    return redact_text(value.as_string(), policy);
  }
  if (value.is_array()) {
    Value::Array output;
    for (const auto& item : value.as_array()) {
      output.push_back(redact_value_strings(item, policy));
    }
    return Value(std::move(output));
  }
  if (value.is_object()) {
    Value::Object output;
    for (const auto& [key, item] : value.as_object()) {
      output[key] = redact_value_strings(item, policy);
    }
    return Value(std::move(output));
  }
  return value;
}

std::vector<Value> collect_citations(const Value& result) {
  std::vector<Value> citations;
  for (const auto& hit : result.at("knowledgeHits").as_array()) {
    if (hit.at("citation").is_object()) {
      citations.push_back(hit.at("citation"));
    }
  }
  for (const auto& hit : result.at("memoryHits").as_array()) {
    if (hit.at("metadata").at("citation").is_object()) {
      citations.push_back(hit.at("metadata").at("citation"));
    } else if (hit.at("metadata").contains("documentId") || hit.at("metadata").contains("chunkId")) {
      citations.push_back(Value::object({
          {"documentId", hit.at("metadata").at("documentId").as_string(hit.at("id").as_string())},
          {"chunkId", hit.at("metadata").at("chunkId").as_string(hit.at("id").as_string())},
          {"title", hit.at("metadata").at("title").as_string(hit.at("id").as_string())},
          {"uri", hit.at("metadata").at("uri").as_string()},
      }));
    }
  }
  return citations;
}

std::pair<std::string, int> build_citation_appendix(const Value& result,
                                                    const NormalizedCitationPolicy& policy) {
  std::set<std::string> seen;
  std::vector<std::string> lines;
  for (const auto& citation : collect_citations(result)) {
    const auto document_id = citation.at("documentId").as_string(
        citation.at("document_id").as_string(citation.at("id").as_string("unknown")));
    const auto chunk_id = citation.at("chunkId").as_string(
        citation.at("chunk_id").as_string(citation.at("chunkIndex").as_string()));
    const auto key = document_id + ":" + chunk_id;
    if (seen.find(key) != seen.end()) {
      continue;
    }
    seen.insert(key);
    const auto title = citation.at("title").as_string(document_id);
    const auto uri = citation.at("uri").as_string();
    lines.push_back("[" + std::to_string(lines.size() + 1) + "] " + title +
                    (uri.empty() ? "" : " (" + uri + ")"));
    if (static_cast<int>(lines.size()) >= policy.max_sources) {
      break;
    }
  }
  if (lines.empty()) {
    return {"", 0};
  }
  return {policy.header + ":\n" + join_values(lines, "\n"), static_cast<int>(lines.size())};
}

void set_governed_output(Value& result, const std::string& output, bool redact_raw) {
  result["text"] = output;
  result["response"]["text"] = output;
  if (redact_raw) {
    result["response"]["raw"] = Value();
  }
}

GovernedRun apply_governance_to_run(std::string output,
                                    Value result,
                                    const ResolvedGovernancePolicy& policy) {
  AgentServerGovernanceSummary summary;
  std::vector<std::string> violations;
  const auto [citation_appendix, citation_count] = build_citation_appendix(result, policy.citations);
  summary.citation_count = citation_count;
  if (policy.citations.mode == "require" && citation_count == 0) {
    violations.push_back("Citation policy requires at least one knowledge citation.");
  }

  std::optional<Value> structured_output;
  if (policy.output_schema) {
    try {
      structured_output = parse_json(output);
    } catch (const std::exception&) {
      violations.push_back("Output schema validation requires JSON output.");
    }
  }

  if (structured_output && policy.pii) {
    auto redacted = redact_value_strings(*structured_output, *policy.pii);
    summary.redacted = summary.redacted || redacted != *structured_output;
    structured_output = std::move(redacted);
  }

  if (policy.output_schema && structured_output) {
    const auto issues = validate_json_schema(*policy.output_schema, *structured_output);
    if (!issues.empty()) {
      for (const auto& issue : issues) {
        violations.push_back(issue.path + ": " + issue.message);
      }
    } else {
      summary.schema_validated = true;
    }
    output = structured_output->stringify(2);
  }

  if (policy.citations.mode == "require" && policy.citations.append_to_text &&
      !citation_appendix.empty() && !policy.output_schema) {
    output += "\n\n" + citation_appendix;
    summary.citations_appended = true;
  }

  if (policy.pii && !policy.output_schema) {
    const auto redacted = redact_text(output, *policy.pii);
    summary.redacted = summary.redacted || redacted != output;
    output = redacted;
  }

  if (!violations.empty()) {
    Value::Array violation_values;
    for (const auto& violation : violations) {
      violation_values.push_back(violation);
    }
    throw HttpRequestError(422, "Output policy violation.",
                           Value::object({{"error", "Output policy violation."},
                                          {"violations", Value(std::move(violation_values))}}));
  }

  const bool redact_raw = policy.pii.has_value() || summary.citations_appended || summary.schema_validated;
  set_governed_output(result, output, redact_raw);
  if (policy.pii) {
    result = redact_value_strings(result, *policy.pii);
    set_governed_output(result, output, true);
  }
  return GovernedRun{.output = std::move(output), .result = std::move(result), .summary = summary};
}

void add_trace_payload(Value& payload, const AgentServerRequestContext& context) {
  if (!payload.is_object()) {
    return;
  }
  if (!context.access.request_id.empty()) {
    payload["requestId"] = context.access.request_id;
  }
  if (!context.access.trace_context.trace_id.empty()) {
    payload["traceId"] = context.access.trace_context.trace_id;
  }
}

Value merge_server_execution_context(Value context,
                                     const AgentServerRequestContext& request_context,
                                     std::string span_name,
                                     Value details = Value::object({})) {
  Value output = context.is_object() ? std::move(context) : Value::object({});
  TraceContext trace = create_child_trace_context(request_context.access.trace_context, TraceContext{
      .span_name = span_name.empty() ? "server.execution" : std::move(span_name),
  });
  output["traceContext"] = trace_context_to_value(trace);
  output["traceId"] = trace.trace_id;
  if (!request_context.access.request_id.empty()) {
    output["requestId"] = request_context.access.request_id;
  }

  Value server = output.at("server").is_object() ? output.at("server") : Value::object({});
  server["path"] = request_context.path;
  if (!request_context.access.request_id.empty()) {
    server["requestId"] = request_context.access.request_id;
  }
  server["apiKeyId"] = request_context.access.api_key ? Value(request_context.access.api_key->id) : Value();
  if (details.is_object()) {
    for (const auto& [key, value] : details.as_object()) {
      server[key] = value;
    }
  }
  output["server"] = server;
  return output;
}

std::optional<TaskStatus> task_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  const auto& value = found->second;
  if (value == "queued") return TaskStatus::Queued;
  if (value == "running") return TaskStatus::Running;
  if (value == "completed") return TaskStatus::Completed;
  if (value == "failed") return TaskStatus::Failed;
  if (value == "cancelled") return TaskStatus::Cancelled;
  if (value == "interrupted") return TaskStatus::Interrupted;
  if (value == "waiting") return TaskStatus::Waiting;
  throw HttpRequestError(400, "Invalid task status filter.",
                         Value::object({{"error", "Invalid task status filter."}, {"status", value}}));
}

std::optional<AutonomousRunStatus> autonomous_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  const auto& value = found->second;
  if (value == "queued" || value == "running" || value == "waiting" || value == "completed" ||
      value == "failed" || value == "cancelled" || value == "interrupted") {
    return autonomous_run_status_from_string(value);
  }
  throw HttpRequestError(400, "Invalid autonomous run status filter.",
                         Value::object({{"error", "Invalid autonomous run status filter."},
                                        {"status", value}}));
}

std::optional<ApprovalRecordStatus> approval_status_filter(const std::map<std::string, std::string>& query) {
  const auto found = query.find("status");
  if (found == query.end() || found->second.empty()) {
    return std::nullopt;
  }
  if (found->second == "pending") return ApprovalRecordStatus::Pending;
  if (found->second == "resolved") return ApprovalRecordStatus::Resolved;
  throw HttpRequestError(400, "Invalid approval status filter.",
                         Value::object({{"error", "Invalid approval status filter."},
                                        {"status", found->second}}));
}

std::string pg_approval_identifier(const std::string& value) {
  std::string escaped;
  escaped.reserve(value.size() + 2);
  escaped.push_back('"');
  for (const char ch : value) {
    if (ch == '"') {
      escaped += "\"\"";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back('"');
  return escaped;
}

Value pg_approval_json_param(const Value& value) {
  return value.is_null() ? Value() : Value(value.stringify(0));
}

Value pg_approval_json_from_value(const Value& value) {
  if (!value.is_string()) {
    return value;
  }
  try {
    return parse_json(value.as_string());
  } catch (...) {
    return value;
  }
}

ApprovalRecord approval_from_pg_row(const Value& row) {
  Value value = Value::object({
      {"id", row.at("id").as_string()},
      {"createdAt", row.at("created_at").as_string()},
      {"resolvedAt", row.at("resolved_at").is_null() ? Value() : Value(row.at("resolved_at").as_string())},
      {"status", row.at("status").as_string("pending")},
      {"request", pg_approval_json_from_value(row.at("request"))},
      {"proposedDecision", pg_approval_json_from_value(row.at("proposed_decision"))},
      {"finalDecision", row.at("final_decision").is_null()
                            ? Value()
                            : pg_approval_json_from_value(row.at("final_decision"))},
      {"metadata", row.at("metadata").is_null() ? Value::object({}) : pg_approval_json_from_value(row.at("metadata"))},
  });
  return approval_record_from_value(value);
}

const Value& require_pg_approval_row(const std::vector<Value>& rows, const std::string& message) {
  if (rows.empty()) {
    throw ConfigurationError(message);
  }
  return rows.front();
}

}  // namespace

struct ApprovalStoreState {
  mutable std::mutex mutex;
  std::map<std::string, ApprovalRecord> records;
};

struct PendingApprovalResolution {
  std::mutex mutex;
  std::condition_variable ready;
  bool resolved = false;
  PermissionDecision decision;
};

struct ManualApprovalQueueState {
  std::shared_ptr<ApprovalStore> owned_store;
  ApprovalStore* store = nullptr;
  mutable std::mutex mutex;
  std::map<std::string, std::shared_ptr<PendingApprovalResolution>> pending;
};

HttpRequestError::HttpRequestError(int status_code, std::string message, Value payload)
    : AgentFrameworkError(std::move(message)),
      status_code_(status_code),
      payload_(payload.is_object() ? std::move(payload) : Value::object({})) {}

int HttpRequestError::status_code() const noexcept {
  return status_code_;
}

const Value& HttpRequestError::payload() const noexcept {
  return payload_;
}

Value audit_record_to_value(const AuditRecord& record) {
  Value value = Value::object({{"at", record.at.empty() ? now_iso8601() : record.at},
                               {"type", record.type}});
  add_optional_audit_field(value, "method", record.method);
  add_optional_audit_field(value, "path", record.path);
  add_optional_audit_field(value, "agentId", record.agent_id);
  add_optional_audit_field(value, "sessionId", record.session_id);
  add_optional_audit_field(value, "workflowRunId", record.workflow_run_id);
  add_optional_audit_field(value, "approvalId", record.approval_id);
  add_optional_audit_field(value, "apiKeyId", record.api_key_id);
  add_optional_audit_field(value, "requestId", record.request_id);
  add_optional_audit_field(value, "traceId", record.trace_id);
  add_optional_audit_field(value, "taskId", record.task_id);
  if (record.status_code) {
    value["statusCode"] = *record.status_code;
  }
  if (record.duration_ms) {
    value["durationMs"] = *record.duration_ms;
  }
  if (record.detail.is_object() && !record.detail.as_object().empty()) {
    value["detail"] = record.detail;
  }
  return value;
}

void append_audit_log(const std::filesystem::path& file_path, AuditRecord record) {
  if (file_path.empty()) {
    return;
  }
  if (record.at.empty()) {
    record.at = now_iso8601();
  }
  if (!file_path.parent_path().empty()) {
    std::filesystem::create_directories(file_path.parent_path());
  }
  std::ofstream output(file_path, std::ios::binary | std::ios::app);
  if (!output) {
    throw ConfigurationError("Unable to append audit log: " + file_path.string());
  }
  output << audit_record_to_value(record).stringify(0) << '\n';
}

Value agent_server_governance_summary_to_value(const AgentServerGovernanceSummary& summary) {
  return Value::object({{"redacted", summary.redacted},
                        {"schemaValidated", summary.schema_validated},
                        {"citationCount", summary.citation_count},
                        {"citationsAppended", summary.citations_appended}});
}

std::string to_string(ApprovalRecordStatus status) {
  switch (status) {
    case ApprovalRecordStatus::Pending:
      return "pending";
    case ApprovalRecordStatus::Resolved:
      return "resolved";
  }
  return "pending";
}

ApprovalRecordStatus approval_record_status_from_string(const std::string& value,
                                                        ApprovalRecordStatus fallback) {
  if (value == "pending") return ApprovalRecordStatus::Pending;
  if (value == "resolved") return ApprovalRecordStatus::Resolved;
  return fallback;
}

Value permission_decision_to_value(const PermissionDecision& decision) {
  return Value::object({{"decision", to_string(decision.decision)},
                        {"reason", decision.reason.empty() ? Value() : Value(decision.reason)}});
}

PermissionDecision permission_decision_from_value(const Value& value) {
  return PermissionDecision{
      .decision = permission_decision_kind_from_string(value.at("decision").as_string(), PermissionDecisionKind::Deny),
      .reason = value.at("reason").as_string(),
  };
}

Value permission_request_to_value(const PermissionRequest& request) {
  return Value::object({{"toolName", request.tool_name},
                        {"capabilities", strings_to_value(request.capabilities)},
                        {"riskLevel", to_string(request.risk_level)},
                        {"tags", strings_to_value(request.tags)},
                        {"bundle", request.bundle.empty() ? Value() : Value(request.bundle)},
                        {"builtin", request.builtin},
                        {"toolCall", tool_call_to_value(request.tool_call)},
                        {"context", tool_execution_context_to_value(request.context)}});
}

PermissionRequest permission_request_from_value(const Value& value) {
  PermissionRequest request;
  request.tool_name = value.at("toolName").as_string(value.at("tool_name").as_string());
  request.capabilities = strings_from_value(value.at("capabilities"));
  request.risk_level = tool_risk_level_from_string(value.at("riskLevel").as_string(
      value.at("risk_level").as_string()), ToolRiskLevel::Low);
  request.tags = strings_from_value(value.at("tags"));
  request.bundle = value.at("bundle").as_string();
  request.builtin = value.at("builtin").as_bool();
  request.tool_call = tool_call_from_value(value.at("toolCall").is_object() ? value.at("toolCall")
                                                                            : value.at("tool_call"));
  request.context = tool_execution_context_from_value(value.at("context"));
  return request;
}

Value approval_record_to_value(const ApprovalRecord& record) {
  return Value::object({{"id", record.id},
                        {"createdAt", record.created_at},
                        {"resolvedAt", record.resolved_at.empty() ? Value() : Value(record.resolved_at)},
                        {"status", to_string(record.status)},
                        {"request", permission_request_to_value(record.request)},
                        {"proposedDecision", permission_decision_to_value(record.proposed_decision)},
                        {"finalDecision", record.final_decision ? permission_decision_to_value(*record.final_decision)
                                                                 : Value()},
                        {"metadata", record.metadata}});
}

ApprovalRecord approval_record_from_value(const Value& value) {
  ApprovalRecord record;
  record.id = value.at("id").as_string();
  record.created_at = value.at("createdAt").as_string(value.at("created_at").as_string());
  record.resolved_at = value.at("resolvedAt").as_string(value.at("resolved_at").as_string());
  record.status = approval_record_status_from_string(value.at("status").as_string(), ApprovalRecordStatus::Pending);
  record.request = permission_request_from_value(value.at("request"));
  record.proposed_decision = permission_decision_from_value(value.at("proposedDecision").is_object()
                                                               ? value.at("proposedDecision")
                                                               : value.at("proposed_decision"));
  const auto& final_decision = value.at("finalDecision").is_object() ? value.at("finalDecision")
                                                                     : value.at("final_decision");
  if (final_decision.is_object()) {
    record.final_decision = permission_decision_from_value(final_decision);
  }
  record.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return record;
}

InMemoryApprovalStore::InMemoryApprovalStore() : state_(std::make_shared<ApprovalStoreState>()) {}

ApprovalRecord InMemoryApprovalStore::create(CreateApprovalRecordInput input) {
  ApprovalRecord record;
  record.id = input.id.empty() ? generate_uuid() : std::move(input.id);
  record.created_at = now_iso8601();
  record.status = ApprovalRecordStatus::Pending;
  record.request = std::move(input.request);
  record.proposed_decision = std::move(input.proposed_decision);
  record.metadata = input.metadata.is_object() ? std::move(input.metadata) : Value::object({});

  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->records.find(record.id) != state_->records.end()) {
    throw ConfigurationError("Approval already exists: " + record.id);
  }
  state_->records[record.id] = record;
  return record;
}

std::optional<ApprovalRecord> InMemoryApprovalStore::get(const std::string& id) const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const auto found = state_->records.find(id);
  if (found == state_->records.end()) {
    return std::nullopt;
  }
  return found->second;
}

std::vector<ApprovalRecord> InMemoryApprovalStore::list(ApprovalRecordFilter filter) const {
  std::vector<ApprovalRecord> records;
  std::lock_guard<std::mutex> lock(state_->mutex);
  for (const auto& [_, record] : state_->records) {
    if (!filter.status || record.status == *filter.status) {
      records.push_back(record);
    }
  }
  std::sort(records.begin(), records.end(), [](const ApprovalRecord& left, const ApprovalRecord& right) {
    return left.created_at < right.created_at;
  });
  return records;
}

std::optional<ApprovalRecord> InMemoryApprovalStore::resolve(const std::string& id,
                                                             PermissionDecision final_decision) {
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto found = state_->records.find(id);
  if (found == state_->records.end()) {
    return std::nullopt;
  }
  if (found->second.status == ApprovalRecordStatus::Resolved) {
    return found->second;
  }
  found->second.status = ApprovalRecordStatus::Resolved;
  found->second.resolved_at = now_iso8601();
  found->second.final_decision = std::move(final_decision);
  return found->second;
}

FileApprovalStore::FileApprovalStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

ApprovalRecord FileApprovalStore::create(CreateApprovalRecordInput input) {
  ensure_loaded();
  auto record = InMemoryApprovalStore::create(std::move(input));
  persist();
  return record;
}

std::optional<ApprovalRecord> FileApprovalStore::get(const std::string& id) const {
  ensure_loaded();
  return InMemoryApprovalStore::get(id);
}

std::vector<ApprovalRecord> FileApprovalStore::list(ApprovalRecordFilter filter) const {
  ensure_loaded();
  return InMemoryApprovalStore::list(std::move(filter));
}

std::optional<ApprovalRecord> FileApprovalStore::resolve(const std::string& id,
                                                         PermissionDecision final_decision) {
  ensure_loaded();
  auto record = InMemoryApprovalStore::resolve(id, std::move(final_decision));
  persist();
  return record;
}

const std::filesystem::path& FileApprovalStore::file_path() const noexcept {
  return file_path_;
}

void FileApprovalStore::ensure_loaded() const {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  if (!std::filesystem::exists(file_path_)) {
    return;
  }
  const auto root = read_json_file(file_path_);
  const auto& items = root.is_array() ? root.as_array() : root.at("records").as_array();
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->records.clear();
  for (const auto& item : items) {
    auto record = approval_record_from_value(item);
    const auto record_id = record.id;
    state_->records[record_id] = std::move(record);
  }
}

void FileApprovalStore::persist() const {
  Value::Array records;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (const auto& [_, record] : state_->records) {
      records.push_back(approval_record_to_value(record));
    }
  }
  write_json_file(file_path_, Value(std::move(records)));
}

PgApprovalStore::PgApprovalStore(PgApprovalStoreConfig config)
    : client_(std::move(config.client)),
      schema_name_(config.schema_name.empty() ? "public" : std::move(config.schema_name)),
      table_name_(config.table_name.empty() ? "node_agent_approvals" : std::move(config.table_name)),
      create_table_(config.create_table),
      close_client_on_close_(config.close_client_on_close) {
  if (!client_) {
    throw ConfigurationError("PgApprovalStore requires an injected PgApprovalClient.");
  }
}

PgApprovalStore::~PgApprovalStore() {
  close();
}

ApprovalRecord PgApprovalStore::create(CreateApprovalRecordInput input) {
  ensure_ready();
  const auto id = input.id.empty() ? generate_uuid() : std::move(input.id);
  const auto created_at = now_iso8601();
  const auto request = permission_request_to_value(input.request);
  const auto proposed_decision = permission_decision_to_value(input.proposed_decision);
  const auto metadata = input.metadata.is_object() ? input.metadata : Value::object({});
  const auto rows = client_->query(
      "INSERT INTO " + table() + " ("
      "id, created_at, resolved_at, status, request, proposed_decision, final_decision, metadata"
      ") VALUES ($1, $2, NULL, 'pending', $3::jsonb, $4::jsonb, NULL, $5::jsonb) "
      "RETURNING *",
      {id, created_at, pg_approval_json_param(request), pg_approval_json_param(proposed_decision),
       pg_approval_json_param(metadata)});
  return approval_from_pg_row(require_pg_approval_row(rows, "Approval insert returned no row: " + id));
}

std::optional<ApprovalRecord> PgApprovalStore::get(const std::string& id) const {
  ensure_ready();
  const auto rows = client_->query("SELECT * FROM " + table() + " WHERE id = $1", {id});
  if (rows.empty()) {
    return std::nullopt;
  }
  return approval_from_pg_row(rows.front());
}

std::vector<ApprovalRecord> PgApprovalStore::list(ApprovalRecordFilter filter) const {
  ensure_ready();
  std::vector<Value> params;
  std::string where;
  if (filter.status) {
    where = " WHERE status = $1";
    params.push_back(to_string(*filter.status));
  }
  const auto rows = client_->query("SELECT * FROM " + table() + where + " ORDER BY created_at ASC", params);
  std::vector<ApprovalRecord> records;
  records.reserve(rows.size());
  for (const auto& row : rows) {
    records.push_back(approval_from_pg_row(row));
  }
  return records;
}

std::optional<ApprovalRecord> PgApprovalStore::resolve(const std::string& id,
                                                       PermissionDecision final_decision) {
  ensure_ready();
  const auto resolved_at = now_iso8601();
  const auto final_decision_value = permission_decision_to_value(final_decision);
  const auto rows = client_->query(
      "UPDATE " + table() +
          " SET status = 'resolved', resolved_at = $2, final_decision = $3::jsonb "
          "WHERE id = $1 AND status = 'pending' RETURNING *",
      {id, resolved_at, pg_approval_json_param(final_decision_value)});
  if (!rows.empty()) {
    return approval_from_pg_row(rows.front());
  }
  return get(id);
}

void PgApprovalStore::close() {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (!close_client_on_close_ || !client_) {
    return;
  }
  client_->close();
  close_client_on_close_ = false;
}

std::string PgApprovalStore::table() const {
  return pg_approval_identifier(schema_name_) + "." + pg_approval_identifier(table_name_);
}

void PgApprovalStore::ensure_ready() const {
  std::lock_guard<std::mutex> lock(ready_mutex_);
  if (ready_ || !create_table_) {
    return;
  }
  client_->query("CREATE SCHEMA IF NOT EXISTS " + pg_approval_identifier(schema_name_));
  client_->query("CREATE TABLE IF NOT EXISTS " + table() + " ("
                 "id TEXT PRIMARY KEY,"
                 "created_at TEXT NOT NULL,"
                 "resolved_at TEXT,"
                 "status TEXT NOT NULL,"
                 "request JSONB NOT NULL,"
                 "proposed_decision JSONB NOT NULL,"
                 "final_decision JSONB,"
                 "metadata JSONB"
                 ")");
  client_->query("CREATE INDEX IF NOT EXISTS " + pg_approval_identifier(table_name_ + "_status_idx") +
                 " ON " + table() + " (status, created_at)");
  ready_ = true;
}

ManualApprovalQueue::ManualApprovalQueue(ManualApprovalQueueConfig config)
    : state_(std::make_shared<ManualApprovalQueueState>()) {
  if (config.store) {
    state_->store = config.store;
  } else {
    state_->owned_store = std::make_shared<InMemoryApprovalStore>();
    state_->store = state_->owned_store.get();
  }
}

PermissionDecision ManualApprovalQueue::approve(const PermissionRequest& request,
                                                const PermissionDecision& decision) {
  auto pending = std::make_shared<PendingApprovalResolution>();
  ApprovalRecord record;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    record = state_->store->create(CreateApprovalRecordInput{
        .request = request,
        .proposed_decision = decision,
    });
    state_->pending[record.id] = pending;
  }

  std::unique_lock<std::mutex> lock(pending->mutex);
  pending->ready.wait(lock, [&pending]() {
    return pending->resolved;
  });
  return pending->decision;
}

PermissionApprovalHandler ManualApprovalQueue::handler() {
  return [this](const PermissionRequest& request, const PermissionDecision& decision) {
    return approve(request, decision);
  };
}

std::vector<ApprovalRecord> ManualApprovalQueue::list(ApprovalRecordFilter filter) const {
  return state_->store->list(std::move(filter));
}

std::optional<ApprovalRecord> ManualApprovalQueue::get(const std::string& id) const {
  return state_->store->get(id);
}

std::optional<ApprovalRecord> ManualApprovalQueue::resolve(const std::string& id,
                                                           PermissionDecision final_decision) {
  auto record = state_->store->resolve(id, final_decision);
  if (!record) {
    return std::nullopt;
  }

  std::shared_ptr<PendingApprovalResolution> pending;
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->pending.find(id);
    if (found != state_->pending.end()) {
      pending = found->second;
      state_->pending.erase(found);
    }
  }
  if (pending) {
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      pending->decision = record->final_decision.value_or(std::move(final_decision));
      pending->resolved = true;
    }
    pending->ready.notify_all();
  }
  return record;
}

ApprovalStore& ManualApprovalQueue::store() const {
  return *state_->store;
}

PermissionApprovalHandler create_manual_approval_handler(ManualApprovalQueue& queue) {
  return queue.handler();
}

Value agent_server_metrics_bucket_to_value(const AgentServerMetricsBucket& bucket) {
  return Value::object({{"requests", bucket.requests},
                        {"succeeded", bucket.succeeded},
                        {"failed", bucket.failed},
                        {"rejected", bucket.rejected},
                        {"durationMs", bucket.duration_ms},
                        {"statusCodes", status_codes_to_value(bucket.status_codes)},
                        {"inputTokens", bucket.input_tokens},
                        {"outputTokens", bucket.output_tokens},
                        {"totalTokens", bucket.total_tokens},
                        {"estimatedCostByCurrency", double_map_to_value(bucket.estimated_cost_by_currency)}});
}

AgentServerMetricsCollector::AgentServerMetricsCollector(bool enabled,
                                                         std::map<std::string, UsagePricing> pricing)
    : enabled_(enabled), pricing_(std::move(pricing)) {}

AgentServerMetricsBucket& AgentServerMetricsCollector::bucket_for(
    std::map<std::string, AgentServerMetricsBucket>& buckets,
    const std::string& key) {
  return buckets[key];
}

std::optional<UsagePricing> AgentServerMetricsCollector::pricing_for(const std::string& provider,
                                                                     const std::string& model) const {
  const auto exact = pricing_.find(provider + ":" + model);
  if (exact != pricing_.end()) {
    return exact->second;
  }
  const auto provider_default = pricing_.find(provider + ":*");
  if (provider_default != pricing_.end()) {
    return provider_default->second;
  }
  const auto global_default = pricing_.find("*");
  if (global_default != pricing_.end()) {
    return global_default->second;
  }
  return std::nullopt;
}

void AgentServerMetricsCollector::apply(AgentServerMetricsBucket& bucket, int status_code,
                                        long long duration_ms, const std::string& kind,
                                        const Value& model_response) {
  bucket.requests += 1;
  bucket.duration_ms += duration_ms;
  bucket.status_codes[std::to_string(status_code)] += 1;
  if (kind == "succeeded") {
    bucket.succeeded += 1;
    if (model_response.is_object()) {
      const auto provider = model_response.at("provider").as_string();
      const auto model = model_response.at("model").as_string();
      const auto usage = extract_model_usage(model_response, provider);
      bucket.input_tokens += usage.input_tokens;
      bucket.output_tokens += usage.output_tokens;
      bucket.total_tokens += usage.total_tokens;
      if (auto pricing = pricing_for(provider, model)) {
        const auto estimate = estimate_usage_cost(usage, *pricing, provider, model);
        bucket.estimated_cost_by_currency[estimate.currency] += estimate.total_cost;
      }
    }
  } else if (kind == "rejected") {
    bucket.rejected += 1;
  } else {
    bucket.failed += 1;
  }
}

void AgentServerMetricsCollector::record(const std::string& route, int status_code, long long duration_ms,
                                         std::string kind, std::string api_key_id,
                                         std::string agent_id, Value model_response) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) {
    return;
  }
  apply(totals_, status_code, duration_ms, kind, model_response);
  if (!route.empty()) {
    apply(bucket_for(routes_, route), status_code, duration_ms, kind, model_response);
  }
  if (!agent_id.empty()) {
    apply(bucket_for(agents_, agent_id), status_code, duration_ms, kind, model_response);
  }
  if (!api_key_id.empty()) {
    apply(bucket_for(api_keys_, api_key_id), status_code, duration_ms, kind, model_response);
  }
}

Value AgentServerMetricsCollector::snapshot(const std::string& service_name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!enabled_) {
    return Value::object({{"ok", true}, {"service", service_name}, {"enabled", false}});
  }
  return Value::object({{"ok", true},
                        {"service", service_name},
                        {"enabled", true},
                        {"totals", agent_server_metrics_bucket_to_value(totals_)},
                        {"routes", buckets_to_value(routes_)},
                        {"agents", buckets_to_value(agents_)},
                        {"apiKeys", buckets_to_value(api_keys_)}});
}

RateLimitWindow::Result RateLimitWindow::consume(const std::string& key, int max_requests, int window_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current_ms = now_ms();
  auto& bucket = buckets_[key];
  if (bucket.reset_at_ms <= current_ms) {
    bucket.count = 0;
    bucket.reset_at_ms = current_ms + window_ms;
  }
  bucket.count += 1;
  return RateLimitWindow::Result{
      .allowed = max_requests <= 0 || bucket.count <= max_requests,
      .remaining = std::max(0, max_requests - bucket.count),
      .reset_at_ms = bucket.reset_at_ms,
  };
}

Value read_json_body(const HttpRequest& request, std::size_t max_bytes) {
  if (request.body.size() > max_bytes) {
    throw HttpRequestError(413, "Request body exceeded configured size limit.",
                           Value::object({{"error", "Request body exceeded configured size limit."}}));
  }
  if (request.body.empty()) {
    return Value::object({});
  }
  try {
    auto parsed = parse_json(request.body);
    if (!parsed.is_object()) {
      throw HttpRequestError(400, "Request body must be a JSON object.",
                             Value::object({{"error", "Request body must be a JSON object."}}));
    }
    return parsed;
  } catch (const HttpRequestError&) {
    throw;
  } catch (const std::exception&) {
    throw HttpRequestError(400, "Request body must be valid JSON.",
                           Value::object({{"error", "Request body must be valid JSON."}}));
  }
}

AgentServerApp::AgentServerApp(AgentServerOptions options)
    : options_(std::move(options)), metrics_(options_.metrics_enabled, options_.metrics_pricing) {
  if (!options_.approval_mode.empty() && options_.approval_mode != "inherit" &&
      options_.approval_mode != "manual") {
    throw ConfigurationError("AgentServerOptions approval_mode must be \"inherit\" or \"manual\".");
  }
  if (!options_.config.is_null() && !options_.config.is_object()) {
    throw ConfigurationError("AgentServerOptions config must be a JSON object.");
  }
  // Fail-closed: a server with no authentication configured is a
  // misconfiguration, not a default. Refuse to construct unless the operator
  // explicitly opts into a public server via allow_unauthenticated.
  const bool auth_configured = options_.auth || !options_.api_keys.empty();
  if (!auth_configured && !options_.allow_unauthenticated) {
    throw ConfigurationError(
        "Agent server has no authentication configured. Set auth.bearer_tokens or api_keys, "
        "or set allow_unauthenticated=true to run a deliberately public server.");
  }

  if (options_.approvals && options_.approvals->queue) {
    approval_queue_ = options_.approvals->queue;
  } else {
    owned_approval_queue_ = std::make_shared<ManualApprovalQueue>(ManualApprovalQueueConfig{
        .store = options_.approvals ? options_.approvals->store : nullptr,
    });
    approval_queue_ = owned_approval_queue_.get();
  }
  if (options_.approval_mode == "manual" && options_.runner && approval_queue_) {
    options_.runner->set_approval_handler(approval_queue_->handler());
  }
  register_builtin_routes();
}

void AgentServerApp::add_route(AgentServerRoute route) {
  route.method = upper_copy(std::move(route.method));
  if (route.method.empty() || route.pattern.empty() || !route.handler) {
    throw ConfigurationError("AgentServerRoute requires method, pattern, and handler.");
  }
  std::lock_guard<std::mutex> lock(routes_mutex_);
  routes_.push_back(std::move(route));
}

void AgentServerApp::add_route_module(const AgentServerRouteModule& module) {
  module.register_routes(*this);
}

std::vector<std::string> AgentServerApp::route_specs() const {
  std::vector<std::string> specs;
  std::lock_guard<std::mutex> lock(routes_mutex_);
  specs.reserve(routes_.size());
  for (const auto& route : routes_) {
    specs.push_back(route.method + " " + route.pattern);
  }
  return specs;
}

HttpResponse AgentServerApp::handle(HttpRequest request) {
  const auto started = now_ms();
  const auto method = upper_copy(request.method.empty() ? "GET" : request.method);
  const auto path = path_from_url(request.url);
  const auto query = query_from_url(request.url);
  std::string route_spec = "unknown";
  std::string api_key_id;
  HttpResponse carried_headers;
  const auto request_trace = ensure_request_trace(request, options_.tracing);
  apply_trace_headers(carried_headers, request_trace, options_.tracing);
  try {
    std::vector<AgentServerRoute> routes;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      routes = routes_;
    }

    if (auto preflight = handle_cors_preflight(request, path)) {
      merge_headers(carried_headers, *preflight);
      return *preflight;
    }

    auto access = enforce_access(request, carried_headers, path, request_trace);
    if (access.api_key) {
      api_key_id = access.api_key->id;
    }

    for (const auto& route : routes) {
      if (route.method != method) {
        continue;
      }
      auto match = match_route_pattern(route.pattern, path);
      if (!match.matched) {
        continue;
      }
      route_spec = route.method + " " + route.pattern;
      AgentServerRequestContext context{
          .request = request,
          .method = method,
          .path = path,
          .query = query,
          .params = std::move(match.params),
          .access = std::move(access),
      };
      auto response = route.handler(context);
      merge_headers(carried_headers, response);
      apply_cors_headers(request, response);
      const auto metrics_agent_id = response.metadata.at("agentId").as_string();
      const auto metrics_model_response = response.metadata.at("modelResponse");
      metrics_.record(route_spec, response.status, now_ms() - started,
                      response.status >= 200 && response.status < 400 ? "succeeded" : "failed",
                      api_key_id, metrics_agent_id, metrics_model_response);
      return response;
    }

    auto response = send_json(404, Value::object({{"error", "Route not found."}, {"path", path}}));
    merge_headers(carried_headers, response);
    apply_cors_headers(request, response);
    metrics_.record(route_spec, 404, now_ms() - started, "failed", api_key_id);
    return response;
  } catch (const std::exception& error) {
    auto response = handle_error(request, error);
    merge_headers(carried_headers, response);
    const std::string kind = response.status == 401 || response.status == 429
                                 ? "rejected"
                                 : "failed";
    const auto duration = now_ms() - started;
    if (kind != "rejected") {
      try {
        append_audit(AuditRecord{
            .type = "http.request.failed",
            .method = method,
            .path = path,
            .api_key_id = api_key_id,
            .request_id = request_trace.request_id,
            .trace_id = request_trace.trace_context.trace_id,
            .status_code = response.status,
            .duration_ms = duration,
            .detail = Value::object({{"error", error.what()}}),
        });
      } catch (const std::exception&) {
        // Preserve the original request failure over audit write failures.
      }
    }
    metrics_.record(route_spec, response.status, duration, kind, api_key_id);
    return response;
  }
}

HttpResponse AgentServerApp::handle_error(const HttpRequest& request, const std::exception& error) {
  const auto* request_error = dynamic_cast<const HttpRequestError*>(&error);
  auto response = request_error
                      ? send_json(request_error->status_code(),
                                  request_error->payload().as_object().empty()
                                      ? Value::object({{"error", error.what()}})
                                      : request_error->payload())
                      : send_json(500, Value::object({{"error", error.what()}}));
  const auto request_trace = ensure_request_trace(request, options_.tracing);
  apply_trace_headers(response, request_trace, options_.tracing);
  apply_cors_headers(request, response);
  return response;
}

const AgentServerMetricsCollector& AgentServerApp::metrics() const noexcept {
  return metrics_;
}

void AgentServerApp::append_audit(AuditRecord record) const {
  append_audit_log(options_.audit_log_path, std::move(record));
}

bool AgentServerApp::has_runner_source() const noexcept {
  return options_.runner != nullptr || static_cast<bool>(options_.create_runner) || has_config_source();
}

bool AgentServerApp::has_config_source() const noexcept {
  return options_.config.is_object() || !options_.config_path.empty();
}

const Value& AgentServerApp::server_config() const {
  if (options_.config.is_object()) {
    return options_.config;
  }
  std::lock_guard<std::mutex> lock(config_load_mutex_);
  if (!loaded_config_) {
    loaded_config_ = load_native_agent_config(options_.config_path, options_.config_module_loader);
  }
  return *loaded_config_;
}

std::string AgentServerApp::resolve_default_agent_id() const {
  if (has_config_source()) {
    return resolve_agent_id(server_config(), options_.agent);
  }
  return options_.agent;
}

std::string AgentServerApp::resolve_authorized_agent_id(const AgentServerAccessContext& access,
                                                        const std::string& requested_agent) const {
  std::string agent_id;
  if (has_config_source()) {
    agent_id = resolve_agent_id(server_config(), requested_agent.empty() ? options_.agent : requested_agent);
  } else {
    agent_id = requested_agent.empty() ? options_.agent : requested_agent;
  }
  if (access.api_key && !access.api_key->agents.empty() && !agent_id.empty() &&
      !contains_string(access.api_key->agents, agent_id)) {
    throw HttpRequestError(403, "API key is not allowed to access agent \"" + agent_id + "\".",
                           Value::object({{"error", "Forbidden."}, {"agentId", agent_id}}));
  }
  return agent_id;
}

std::shared_ptr<AgentRunner> AgentServerApp::create_request_runner(const Value& request_body) const {
  if (!options_.create_runner) {
    return {};
  }
  auto runner = options_.create_runner(request_body);
  if (runner && options_.approval_mode == "manual" && approval_queue_) {
    runner->set_approval_handler(approval_queue_->handler());
  }
  return runner;
}

std::shared_ptr<NativeResolvedAgentApp> AgentServerApp::resolve_config_app(const std::string& agent_id) const {
  if (!has_config_source()) {
    return {};
  }
  const auto& config = server_config();
  const auto resolved_agent_id = resolve_agent_id(config, agent_id.empty() ? options_.agent : agent_id);
  std::lock_guard<std::mutex> lock(config_app_cache_mutex_);
  auto found = config_app_cache_.find(resolved_agent_id);
  if (found != config_app_cache_.end()) {
    return found->second;
  }
  auto app = std::make_shared<NativeResolvedAgentApp>(
      resolve_native_agent_app(config, resolved_agent_id));
  if (app->runner && options_.approval_mode == "manual" && approval_queue_) {
    app->runner->set_approval_handler(approval_queue_->handler());
  }
  config_app_cache_[resolved_agent_id] = app;
  return app;
}

std::shared_ptr<AgentRunner> AgentServerApp::resolve_request_runner(const Value& request_body,
                                                                    const std::string& agent_id) const {
  if (options_.runner) {
    return std::shared_ptr<AgentRunner>(options_.runner, [](AgentRunner*) {});
  }
  if (options_.create_runner) {
    return create_request_runner(request_body);
  }
  auto app = resolve_config_app(agent_id);
  if (!app || !app->runner) {
    return {};
  }
  if (options_.approval_mode == "manual" && approval_queue_) {
    app->runner->set_approval_handler(approval_queue_->handler());
  }
  return app->runner;
}

std::string AgentServerApp::maybe_write_replay(const AgentServerRequestContext& context, const Value& input,
                                               const Value& result, const std::vector<Value>& events,
                                               const std::string& agent_id) const {
  if (options_.replay_dir.empty()) {
    return {};
  }
  const auto replay_agent_id = agent_id.empty() ? options_.server_name : agent_id;
  auto manifest = write_run_replay(options_.replay_dir,
                                   result.at("sessionId").as_string("server"),
                                   input,
                                   result,
                                   events,
                                   {},
                                   replay_agent_id,
                                   {},
                                   Value::object({{"source", "server"}, {"path", context.path}}));
  const auto html_path = options_.replay_dir / sanitize_segment(manifest.session_id) / manifest.run_id /
                         manifest.html_file;
  (void)context;
  return html_path.string();
}

void AgentServerApp::register_builtin_routes() {
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/health",
      .handler = [this](const AgentServerRequestContext&) {
        return send_json(200, Value::object({{"ok", true}, {"service", options_.server_name}}));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/ready",
      .handler = [this](const AgentServerRequestContext&) {
        Value::Array routes;
        for (const auto& spec : route_specs()) {
          routes.push_back(spec);
        }
        return send_json(200, Value::object({{"ok", true},
                                            {"service", options_.server_name},
                                            {"agent", resolve_default_agent_id().empty()
                                                          ? Value()
                                                          : Value(resolve_default_agent_id())},
                                            {"routes", Value(std::move(routes))},
                                            {"runner", has_runner_source()}}));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/whoami",
      .handler = [this](const AgentServerRequestContext& context) {
        const auto governance = resolve_governance_policy(options_.governance, std::nullopt);
        const auto agent_id = resolve_default_agent_id();
        return send_json(200, Value::object({
                                  {"authenticated", !context.access.bearer_token.empty()},
                                  {"agent", agent_id.empty() ? Value() : Value(agent_id)},
                                  {"apiKeyId", context.access.api_key ? Value(context.access.api_key->id) : Value()},
                                  {"apiKey", context.access.api_key
                                                 ? Value::object({
                                                       {"id", context.access.api_key->id},
                                                       {"agents", strings_to_value(context.access.api_key->agents)},
                                                       {"metadata", context.access.api_key->metadata},
                                                       {"quota", context.access.quota_limit
                                                                     ? Value::object({
                                                                           {"limit", *context.access.quota_limit},
                                                                           {"remaining", *context.access.quota_remaining},
                                                                           {"resetAtMs", *context.access.quota_reset_at_ms},
                                                                       })
                                                                     : Value()},
                                                   })
                                                 : Value()},
                                  {"quota", context.access.quota_limit
                                                ? Value::object({{"limit", *context.access.quota_limit},
                                                                 {"remaining", *context.access.quota_remaining},
                                                                 {"resetAtMs", *context.access.quota_reset_at_ms}})
                                                : Value()},
                                  {"sessionPolicy", Value::object({{"mode", options_.session.mode},
                                                                   {"idPrefix", options_.session.id_prefix.empty()
                                                                                    ? Value()
                                                                                    : Value(options_.session.id_prefix)},
                                                                   {"allowDelete", options_.session.allow_delete}})},
                                  {"governance", Value::object({
                                                     {"pii", governance.pii.has_value()},
                                                     {"citations", normalized_citation_policy_to_value(governance.citations)},
                                                     {"outputSchema", governance.output_schema
                                                                          ? json_schema_to_server_value(*governance.output_schema)
                                                                          : Value()},
                                                 })},
                              }));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/metrics",
      .handler = [this](const AgentServerRequestContext&) {
        return send_json(200, metrics_.snapshot(options_.server_name));
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/sessions",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/sessions/:sessionId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "DELETE",
      .pattern = "/sessions/:sessionId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_session_delete(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/approvals",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_approval_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/approvals/:approvalId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_approval_resolve(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/tasks",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_create(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "DELETE",
      .pattern = "/tasks/:taskId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_cancel(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId/events",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_events(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId/checkpoints",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_checkpoints(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/tasks/:taskId/state",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_state(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/tasks/:taskId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/tasks/:taskId/cancel",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_task_cancel(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/autonomous-runs",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_list(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_create(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/autonomous-runs/:runId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs/:runId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs/:runId/cancel",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_cancel(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/autonomous-runs/:runId/steps/:stepId/complete",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_autonomous_complete_step(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/run",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_run(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/workflows/:workflowRunId",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_get(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "GET",
      .pattern = "/workflows/:workflowRunId/checkpoints",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_checkpoints(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/:workflowRunId/resume",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_resume(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/:workflowRunId/human-response",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_human_response(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/workflows/:workflowRunId/webhook",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_workflow_webhook(context);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/chat",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_chat_like(context, false);
      },
  });
  add_route(AgentServerRoute{
      .method = "POST",
      .pattern = "/stream",
      .handler = [this](const AgentServerRequestContext& context) {
        return handle_chat_like(context, true);
      },
  });
}

std::optional<HttpResponse> AgentServerApp::handle_cors_preflight(const HttpRequest& request,
                                                                  const std::string& path) {
  if (upper_copy(request.method) != "OPTIONS") {
    return std::nullopt;
  }
  auto response = HttpResponse{.status = 204};
  const auto origin = header_value(request, "origin");
  if (!origin.empty() && (!options_.cors || !contains_string(options_.cors->origins, origin))) {
    return send_json(403, Value::object({{"error", "CORS origin is not allowed."}}));
  }
  (void)path;
  apply_cors_headers(request, response);
  return response;
}

void AgentServerApp::apply_cors_headers(const HttpRequest& request, HttpResponse& response) const {
  if (!options_.cors) {
    return;
  }
  const auto origin = header_value(request, "origin");
  if (origin.empty() || !contains_string(options_.cors->origins, origin)) {
    return;
  }
  response.headers["access-control-allow-origin"] = origin;
  response.headers["vary"] = "Origin";
  response.headers["access-control-allow-headers"] = join_values(options_.cors->allowed_headers, ",");
  response.headers["access-control-allow-methods"] = join_values(options_.cors->methods, ",");
  if (options_.cors->max_age_seconds) {
    response.headers["access-control-max-age"] = std::to_string(*options_.cors->max_age_seconds);
  }
}

AgentServerAccessContext AgentServerApp::enforce_access(const HttpRequest& request, HttpResponse& response,
                                                        const std::string& path,
                                                        const AgentServerRequestTrace& trace) {
  AgentServerAccessContext access;
  access.request_id = trace.request_id;
  access.trace_context = trace.trace_context;
  if (upper_copy(request.method) == "OPTIONS") {
    return access;
  }

  const std::string auth_header = options_.auth ? options_.auth->header_name : "authorization";
  access.bearer_token = bearer_token(request, auth_header);
  // A missing bearer (empty) never matches a configured secret, so an empty
  // configured token can't authenticate an unauthenticated request.
  if (!access.bearer_token.empty()) {
    for (const auto& key : options_.api_keys) {
      if (constant_time_equals(key.token, access.bearer_token)) {
        access.api_key = key;
        break;
      }
    }
  }

  const bool auth_configured = options_.auth || !options_.api_keys.empty();
  const auto exempt_paths = options_.auth ? options_.auth->exempt_paths : std::vector<std::string>{"/health"};
  if (auth_configured && !path_exempt(path, exempt_paths)) {
    const bool valid_bearer = options_.auth && !access.bearer_token.empty() &&
                              contains_secret(options_.auth->bearer_tokens, access.bearer_token);
    const bool valid_api_key = access.api_key.has_value();
    if (!valid_bearer && !valid_api_key) {
      append_audit(AuditRecord{
          .type = "http.request.rejected",
          .method = upper_copy(request.method.empty() ? "GET" : request.method),
          .path = path,
          .request_id = access.request_id,
          .trace_id = access.trace_context.trace_id,
          .status_code = 401,
          .detail = Value::object({{"reason", "unauthorized"}}),
      });
      throw HttpRequestError(401, "Unauthorized.", Value::object({{"error", "Unauthorized."}}));
    }
  }

  if (access.api_key && access.api_key->quota && !path_exempt(path, exempt_paths)) {
    const auto quota = access.api_key->quota.value();
    auto limit = quota_window_.consume("api-key:" + access.api_key->id, quota.max_requests,
                                       quota.window_ms <= 0 ? 60000 : quota.window_ms);
    access.quota_limit = quota.max_requests;
    access.quota_remaining = limit.remaining;
    access.quota_reset_at_ms = limit.reset_at_ms;
    response.headers["x-quota-limit"] = std::to_string(quota.max_requests);
    response.headers["x-quota-remaining"] = std::to_string(limit.remaining);
    response.headers["x-quota-reset"] = std::to_string(limit.reset_at_ms);
    response.headers["x-api-key-id"] = access.api_key->id;
    if (!limit.allowed) {
      append_audit(AuditRecord{
          .type = "http.request.rejected",
          .method = upper_copy(request.method.empty() ? "GET" : request.method),
          .path = path,
          .api_key_id = access.api_key->id,
          .request_id = access.request_id,
          .trace_id = access.trace_context.trace_id,
          .status_code = 429,
          .detail = Value::object({{"reason", "quota"}}),
      });
      throw HttpRequestError(429, "API key quota exceeded.",
                             Value::object({{"error", "API key quota exceeded."}}));
    }
  }

  if (options_.rate_limit && !path_exempt(path, options_.rate_limit->exempt_paths)) {
    const auto key = options_.rate_limit->key_by == "authorization"
                         ? (access.bearer_token.empty() ? "anonymous" : access.bearer_token)
                         : header_value(request, "x-forwarded-for").empty() ? "anonymous"
                                                                            : header_value(request, "x-forwarded-for");
    auto limit = rate_limit_window_.consume(key,
                                            options_.rate_limit->max_requests,
                                            options_.rate_limit->window_ms <= 0
                                                ? 60000
                                                : options_.rate_limit->window_ms);
    response.headers["x-ratelimit-limit"] = std::to_string(options_.rate_limit->max_requests);
    response.headers["x-ratelimit-remaining"] = std::to_string(limit.remaining);
    response.headers["x-ratelimit-reset"] = std::to_string(limit.reset_at_ms);
    if (!limit.allowed) {
      append_audit(AuditRecord{
          .type = "http.request.rejected",
          .method = upper_copy(request.method.empty() ? "GET" : request.method),
          .path = path,
          .api_key_id = access.api_key ? access.api_key->id : std::string{},
          .request_id = access.request_id,
          .trace_id = access.trace_context.trace_id,
          .status_code = 429,
          .detail = Value::object({{"reason", "rate-limit"},
                                   {"keyBy", options_.rate_limit->key_by.empty()
                                                 ? Value("ip")
                                                 : Value(options_.rate_limit->key_by)}}),
      });
      throw HttpRequestError(429, "Rate limit exceeded.", Value::object({{"error", "Rate limit exceeded."}}));
    }
  }

  if (access.api_key) {
    response.headers["x-api-key-id"] = access.api_key->id;
  }
  return access;
}

HttpResponse AgentServerApp::handle_chat_like(const AgentServerRequestContext& context, bool stream) {
  if (!has_runner_source()) {
    throw HttpRequestError(404, "Agent runner is not configured.",
                           Value::object({{"error", "Agent runner is not configured."}}));
  }
  auto body = read_json_body(context.request);
  if (!body.contains("input")) {
    throw HttpRequestError(400, "Request body must include input.",
                           Value::object({{"error", "Request body must include input."}}));
  }
  if (body.contains("context") && !body.at("context").is_object()) {
    throw HttpRequestError(400, "Request body field \"context\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"context\" must be a JSON object."},
                                          {"field", "context"}}));
  }
  if (body.contains("modelSettings") && !body.at("modelSettings").is_object()) {
    throw HttpRequestError(400, "Request body field \"modelSettings\" must be a JSON object.",
                           Value::object({{"error", "Request body field \"modelSettings\" must be a JSON object."},
                                          {"field", "modelSettings"}}));
  }
  validate_server_model_settings(body.at("modelSettings"));
  auto request_governance = request_governance_from_body(body);
  const auto governance_policy = resolve_governance_policy(options_.governance, request_governance);

  const auto started = now_ms();
  const auto session_id = resolve_session_id(body);
  const auto agent_id = resolve_authorized_agent_id(context.access, optional_string_field(body, "agent"));
  append_audit(AuditRecord{
      .type = "http.request.started",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .session_id = session_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"stream", stream}}),
  });
  Value server_details = Value::object({{"sessionId", session_id}});
  if (!agent_id.empty()) {
    server_details["agentId"] = agent_id;
  }
  auto run_context = merge_server_execution_context(
      body.at("context").is_object() ? body.at("context") : Value::object({}),
      context,
      stream ? "stream" : "chat",
      std::move(server_details));
  Value runner_body = body;
  runner_body["sessionId"] = session_id;
  runner_body["context"] = run_context;
  if (!agent_id.empty()) {
    runner_body["agent"] = agent_id;
  }
  auto request_runner = resolve_request_runner(runner_body, agent_id);
  auto* runner = request_runner.get();
  if (!runner) {
    throw HttpRequestError(404, "Agent runner is not configured.",
                           Value::object({{"error", "Agent runner is not configured."}}));
  }
  auto result = runner->run(input_to_text(body.at("input")),
                            session_id,
                            model_settings_from_server_value(body.at("modelSettings")),
                            RunnerRetrievalOptions{},
                            RunnerWritebackOptions{},
                            std::vector<SkillActivation>{},
                            std::move(run_context));
  const auto duration = now_ms() - started;
  auto result_value = runner_result_summary(result);
  Value::Array tool_calls;
  for (const auto& item : result_value.at("toolCalls").as_array()) {
    tool_calls.push_back(item.at("name"));
  }
  auto governed = apply_governance_to_run(result.text, std::move(result_value), governance_policy);
  auto replay_events = replay_events_from_result(result, governed.result);
  const auto replay_path = maybe_write_replay(context, body.at("input"), governed.result, replay_events, agent_id);
  Value payload = Value::object({{"sessionId", result.session_id},
                                 {"durationMs", duration},
                                 {"output", governed.output},
                                 {"toolCalls", Value(std::move(tool_calls))},
                                 {"statusStages", Value::Array{}},
                                 {"events", Value::Array{}},
                                 {"response", governed.result.at("response")},
                                 {"knowledgeHits", governed.result.at("knowledgeHits")},
                                 {"result", governed.result},
                                 {"replayPath", replay_path.empty() ? Value() : Value(replay_path)},
                                 {"governance", agent_server_governance_summary_to_value(governed.summary)}});
  add_trace_payload(payload, context);
  append_audit(AuditRecord{
      .type = "http.request.completed",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .session_id = result.session_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .status_code = 200,
      .duration_ms = duration,
      .detail = Value::object({{"toolCalls", payload.at("toolCalls")},
                               {"replayPath", replay_path.empty() ? Value() : Value(replay_path)},
                               {"governance", payload.at("governance")}}),
  });
  Value metrics_metadata = Value::object({{"agentId", agent_id},
                                          {"modelResponse", model_response_summary(result.response)}});
  if (!stream) {
    auto response = send_json(200, payload);
    response.metadata = metrics_metadata;
    return response;
  }
  auto response = send_sse({{"replay", Value::object({{"replayPath", replay_path.empty() ? Value() : Value(replay_path)}})},
                            {"done", payload}});
  response.metadata = std::move(metrics_metadata);
  return response;
}

HttpResponse AgentServerApp::handle_task_list(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  TaskScopeFilter filter;
  filter.status = task_status_filter(context.query);
  const auto type = context.query.find("type");
  if (type != context.query.end()) {
    filter.type = type->second;
  }
  merge_task_access_filter(filter, task_access_filter(context.access, options_));
  Value::Array items;
  for (const auto& task : options_.tasks->store->list_tasks(filter)) {
    items.push_back(agent_task_to_value(task));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_task_create(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store || !options_.tasks->queue) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  auto body = read_json_body(context.request);
  if (!body.contains("input")) {
    throw HttpRequestError(400, "Request body must include input.",
                           Value::object({{"error", "Request body must include input."}}));
  }
  const auto requested_type = optional_string_field(body, "type");
  const auto type = requested_type.empty() ? options_.tasks->default_type : requested_type;
  const auto session_id = resolve_session_id(body);
  const auto model_settings = optional_object_field(body, "modelSettings");
  validate_server_model_settings(model_settings);
  const auto idempotency_header_name = options_.tasks->idempotency.header_name.empty()
                                           ? std::string("Idempotency-Key")
                                           : options_.tasks->idempotency.header_name;
  const auto idempotency_header_value = header_value(context.request, idempotency_header_name);
  const auto idempotency_body_value = optional_string_field(body, "idempotencyKey");
  if (!idempotency_header_value.empty() && !idempotency_body_value.empty() &&
      idempotency_header_value != idempotency_body_value) {
    throw HttpRequestError(400, "Task idempotency key header and body value must match.",
                           Value::object({{"error", "Task idempotency key header and body value must match."},
                                          {"headerName", idempotency_header_name}}));
  }
  const auto idempotency_key =
      idempotency_header_value.empty() ? idempotency_body_value : idempotency_header_value;
  if (options_.tasks->idempotency.require_key && idempotency_key.empty()) {
    throw HttpRequestError(400, "Task creation requires an idempotency key.",
                           Value::object({{"error", "Task creation requires an idempotency key."},
                                          {"headerName", idempotency_header_name}}));
  }
  const auto agent_id = resolve_authorized_agent_id(context.access, optional_string_field(body, "agent"));
  if (!idempotency_key.empty()) {
    TaskScopeFilter idempotency_filter{.type = type};
    merge_task_access_filter(idempotency_filter, task_access_filter(context.access, options_));
    auto existing = options_.tasks->store->find_task_by_idempotency_key(idempotency_key, idempotency_filter);
    if (existing) {
      auto payload = task_store_snapshot_to_value(*existing);
      payload["idempotent"] = true;
      add_trace_payload(payload, context);
      return send_json(200, payload);
    }
  }
  const auto task_id = optional_string_field(body, "id").empty()
                           ? generate_uuid()
                           : optional_string_field(body, "id");
  Value task_details = Value::object({{"taskId", task_id}});
  if (!agent_id.empty()) {
    task_details["agentId"] = agent_id;
  }
  Value task_context = merge_server_execution_context(optional_object_field(body, "context"),
                                                      context,
                                                      "task.create",
                                                      std::move(task_details));
  Value input = Value::object({{"input", body.at("input")},
                               {"sessionId", session_id},
                               {"agent", agent_id.empty()
                                             ? Value()
                                             : Value(agent_id)},
                               {"modelSettings", model_settings},
                               {"context", task_context}});
  Value metadata = optional_object_field(body, "metadata");
  metadata["source"] = "server";
  if (!idempotency_key.empty()) {
    metadata["idempotencyKey"] = idempotency_key;
  }
  auto access_fields = build_task_access_fields(std::move(metadata), context.access, options_);
  if (!agent_id.empty()) {
    access_fields.metadata["agent"] = agent_id;
  }
  auto task = options_.tasks->store->create_task(CreateTaskInput{
      .id = task_id,
      .type = type.empty() ? "agent.run" : type,
      .input = std::move(input),
      .idempotency_key = idempotency_key,
      .owner_api_key_id = access_fields.owner_api_key_id,
      .tenant_id = access_fields.tenant_id,
      .metadata = std::move(access_fields.metadata),
  });
  options_.tasks->queue->enqueue(task.id);
  options_.tasks->store->append_event(task.id, {}, "task.created",
                                      Value::object({{"source", "server"}, {"type", task.type}}));
  append_audit(AuditRecord{
      .type = "task.created",
      .method = context.method,
      .path = context.path,
      .agent_id = agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = task.id,
      .status_code = 202,
      .detail = Value::object({{"taskId", task.id}, {"taskType", task.type}}),
  });
  auto snapshot = options_.tasks->store->get_task(task.id);
  auto payload = snapshot ? task_store_snapshot_to_value(*snapshot) : Value::object({{"task", agent_task_to_value(task)}});
  payload["idempotent"] = false;
  add_trace_payload(payload, context);
  return send_json(202, payload);
}

HttpResponse AgentServerApp::handle_task_get(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  return send_json(200, task_store_snapshot_to_value(*snapshot));
}

HttpResponse AgentServerApp::handle_task_events(const AgentServerRequestContext& context) {
  auto response = handle_task_get(context);
  if (response.status != 200) {
    return response;
  }
  auto snapshot = parse_json(response.body);
  return send_json(200, Value::object({{"items", snapshot.at("events")}}));
}

HttpResponse AgentServerApp::handle_task_checkpoints(const AgentServerRequestContext& context) {
  auto response = handle_task_get(context);
  if (response.status != 200) {
    return response;
  }
  auto snapshot = parse_json(response.body);
  return send_json(200, Value::object({{"items", snapshot.at("checkpoints")}}));
}

HttpResponse AgentServerApp::handle_task_state(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  const TaskCheckpoint* runner_state = nullptr;
  for (auto it = snapshot->checkpoints.rbegin(); it != snapshot->checkpoints.rend(); ++it) {
    if (it->name == "runner.state") {
      runner_state = &(*it);
      break;
    }
  }
  const bool terminal = snapshot->task.status == TaskStatus::Completed ||
                        snapshot->task.status == TaskStatus::Cancelled ||
                        snapshot->task.status == TaskStatus::Running;
  return send_json(200, Value::object({{"taskId", task_id},
                                       {"status", to_string(snapshot->task.status)},
                                       {"runnerState", runner_state ? runner_state->state : Value()},
                                       {"checkpointId", runner_state ? Value(runner_state->id) : Value()},
                                       {"checkpointCreatedAtMs", runner_state ? time_point_to_ms(runner_state->created_at) : 0},
                                       {"resumable", runner_state != nullptr && !terminal},
                                       {"reason", runner_state == nullptr ? Value("Task has no runner.state checkpoint.") : Value()}}));
}

HttpResponse AgentServerApp::handle_task_resume(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store || !options_.tasks->queue) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  if (snapshot->task.status == TaskStatus::Completed || snapshot->task.status == TaskStatus::Cancelled ||
      snapshot->task.status == TaskStatus::Running) {
    throw HttpRequestError(409, "Task is not resumable.",
                           Value::object({{"error", "Task is not resumable."},
                                          {"taskId", task_id},
                                          {"status", to_string(snapshot->task.status)}}));
  }
  options_.tasks->store->update_task_status(task_id, TaskStatus::Queued);
  options_.tasks->queue->enqueue(task_id);
  options_.tasks->store->append_event(task_id, {}, "task.resumed", Value::object({{"source", "server"}}));
  append_audit(AuditRecord{
      .type = "task.resumed",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = task_id,
      .status_code = 202,
  });
  auto updated = options_.tasks->store->get_task(task_id);
  auto payload = updated ? task_store_snapshot_to_value(*updated) : Value::object({});
  payload["resumed"] = true;
  add_trace_payload(payload, context);
  return send_json(202, payload);
}

HttpResponse AgentServerApp::handle_task_cancel(const AgentServerRequestContext& context) {
  if (!options_.tasks || !options_.tasks->store || !options_.tasks->queue) {
    throw HttpRequestError(404, "Task runtime is not configured.",
                           Value::object({{"error", "Task runtime is not configured."}}));
  }
  assert_task_route_access(context.access, options_);
  const auto task_id = context.params.at("taskId");
  auto snapshot = options_.tasks->store->get_task(task_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Task not found: " + task_id}}));
  }
  assert_task_snapshot_access(*snapshot, context.access, options_, task_id);
  if (options_.tasks->worker) {
    options_.tasks->worker->cancel(task_id);
  } else {
    options_.tasks->queue->cancel(task_id);
  }
  options_.tasks->store->append_event(task_id, {}, "task.cancelled", Value::object({{"source", "server"}}));
  append_audit(AuditRecord{
      .type = "task.cancelled",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .task_id = task_id,
      .status_code = 200,
  });
  auto updated = options_.tasks->store->get_task(task_id);
  auto payload = updated ? task_store_snapshot_to_value(*updated) : Value::object({});
  payload["cancelled"] = true;
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_autonomous_list(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  AutonomousRunFilter filter;
  filter.status = autonomous_status_filter(context.query);
  filter.metadata = autonomous_access_filter(context.access, options_);
  Value::Array items;
  for (const auto& run : options_.autonomous->store->list_runs(filter)) {
    items.push_back(autonomous_run_to_value(run));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_autonomous_create(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  auto body = read_json_body(context.request);
  const std::string raw_goal = optional_string_field(body, "goal");
  const auto goal = ::agent::trim_copy(raw_goal);
  if (goal.empty()) {
    throw HttpRequestError(400, "Request body field \"goal\" must be a non-empty string.",
                           Value::object({{"error", "Request body field \"goal\" must be a non-empty string."},
                                          {"field", "goal"}}));
  }
  const auto auto_start = optional_bool_field_strict(body, "autoStart");
  Value metadata = optional_object_field_strict(body, "metadata");
  metadata = build_autonomous_access_metadata(std::move(metadata), context.access, options_);
  auto run = options_.autonomous->manager->create_run(CreateAutonomousRunInput{
      .id = optional_string_field_strict(body, "id"),
      .goal = goal,
      .input = body.at("input"),
      .metadata = std::move(metadata),
  });
  append_audit(AuditRecord{
      .type = "autonomous.run.created",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run.id}}),
  });
  auto snapshot = auto_start ? options_.autonomous->manager->run(run.id)
                             : *options_.autonomous->store->get_run(run.id);
  auto payload = autonomous_run_snapshot_to_value(snapshot);
  add_trace_payload(payload, context);
  return send_json(auto_start ? 200 : 202, payload);
}

HttpResponse AgentServerApp::handle_autonomous_get(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  return send_json(200, autonomous_run_snapshot_to_value(*snapshot));
}

HttpResponse AgentServerApp::handle_autonomous_resume(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  auto payload = autonomous_run_snapshot_to_value(
      options_.autonomous->manager->resume(run_id));
  append_audit(AuditRecord{
      .type = "autonomous.run.resumed",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run_id},
                               {"status", payload.at("run").at("status")}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_autonomous_cancel(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  auto body = read_json_body(context.request);
  const auto reason = optional_reason_field(body);
  auto payload = autonomous_run_snapshot_to_value(reason ? options_.autonomous->manager->cancel(run_id, *reason)
                                                        : options_.autonomous->manager->cancel(run_id));
  append_audit(AuditRecord{
      .type = "autonomous.run.cancelled",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run_id}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_autonomous_complete_step(const AgentServerRequestContext& context) {
  if (!options_.autonomous || !options_.autonomous->manager || !options_.autonomous->store) {
    throw HttpRequestError(404, "Autonomous runtime is not configured.",
                           Value::object({{"error", "Autonomous runtime is not configured."}}));
  }
  assert_autonomous_route_access(context.access, options_);
  const auto run_id = context.params.at("runId");
  auto snapshot = options_.autonomous->store->get_run(run_id);
  if (!snapshot) {
    return send_json(404, Value::object({{"error", "Autonomous run not found: " + run_id}}));
  }
  assert_autonomous_snapshot_access(*snapshot, context.access, options_, run_id);
  auto body = read_json_body(context.request);
  auto payload = autonomous_run_snapshot_to_value(
      options_.autonomous->manager->complete_waiting_step(
          run_id,
          context.params.at("stepId"),
          body.at("output")));
  append_audit(AuditRecord{
      .type = "autonomous.step.completed",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .detail = Value::object({{"runId", run_id},
                               {"stepId", context.params.at("stepId")}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_run(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto body = read_json_body(context.request);
  std::optional<WorkflowDefinition> definition;
  if (body.at("definition").is_object()) {
    definition = workflow_definition_from_value(body.at("definition"));
  } else if (workflow.definition) {
    definition = *workflow.definition;
  }
  if (!definition) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  Value run_context = merge_server_execution_context(optional_object_field(body, "context"),
                                                     context,
                                                     "workflow.run");
  auto payload = workflow_execution_result_to_value(
      workflow.engine->run(*definition, body.at("input"), std::move(run_context)));
  append_audit(AuditRecord{
      .type = "workflow.run",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = payload.at("workflowRunId").as_string(),
      .status_code = 200,
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_get(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.store) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  const auto workflow_run_id = context.params.at("workflowRunId");
  auto state = workflow.store->get_run(workflow_run_id);
  if (!state) {
    return send_json(404, Value::object({{"error", "Workflow run not found: " + workflow_run_id}}));
  }
  return send_json(200, workflow_run_state_to_value(*state));
}

HttpResponse AgentServerApp::handle_workflow_checkpoints(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.store) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  const auto workflow_run_id = context.params.at("workflowRunId");
  auto state = workflow.store->get_run(workflow_run_id);
  if (!state) {
    return send_json(404, Value::object({{"error", "Workflow run not found: " + workflow_run_id}}));
  }
  Value::Array checkpoints;
  for (const auto& checkpoint : state->checkpoints) {
    checkpoints.push_back(workflow_checkpoint_to_value(checkpoint));
  }
  return send_json(200, Value::object({{"workflowRunId", workflow_run_id},
                                      {"checkpoints", Value(std::move(checkpoints))}}));
}

HttpResponse AgentServerApp::handle_workflow_resume(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto payload = workflow_execution_result_to_value(
      workflow.engine->resume(context.params.at("workflowRunId"),
                              workflow.definition));
  append_audit(AuditRecord{
      .type = "workflow.resume",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = context.params.at("workflowRunId"),
      .status_code = 200,
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_human_response(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto body = read_json_body(context.request);
  const auto node_id = optional_string_field(body, "nodeId");
  auto payload = workflow_execution_result_to_value(
      workflow.engine->submit_human_response(
          context.params.at("workflowRunId"),
          node_id,
          body.at("payload")));
  append_audit(AuditRecord{
      .type = "workflow.human-response",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = context.params.at("workflowRunId"),
      .status_code = 200,
      .detail = Value::object({{"nodeId", node_id}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_workflow_webhook(const AgentServerRequestContext& context) {
  auto workflow = resolve_workflow_runtime(context.access);
  if (!workflow.engine) {
    throw HttpRequestError(404, "Workflow runtime is not configured.",
                           Value::object({{"error", "Workflow runtime is not configured."}}));
  }
  auto body = read_json_body(context.request);
  const auto node_id = optional_string_field(body, "nodeId");
  auto payload = workflow_execution_result_to_value(
      workflow.engine->submit_webhook_payload(
          context.params.at("workflowRunId"),
          node_id,
          body.at("payload")));
  append_audit(AuditRecord{
      .type = "workflow.webhook",
      .method = context.method,
      .path = context.path,
      .agent_id = workflow.agent_id,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .workflow_run_id = context.params.at("workflowRunId"),
      .status_code = 200,
      .detail = Value::object({{"nodeId", node_id}}),
  });
  add_trace_payload(payload, context);
  return send_json(200, payload);
}

HttpResponse AgentServerApp::handle_approval_list(const AgentServerRequestContext& context) {
  auto* queue = approval_queue();
  if (!queue) {
    throw HttpRequestError(404, "Approval runtime is not configured.",
                           Value::object({{"error", "Approval runtime is not configured."}}));
  }
  Value::Array items;
  for (const auto& record : queue->list(ApprovalRecordFilter{.status = approval_status_filter(context.query)})) {
    items.push_back(approval_record_to_value(record));
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_approval_resolve(const AgentServerRequestContext& context) {
  auto* queue = approval_queue();
  if (!queue) {
    throw HttpRequestError(404, "Approval runtime is not configured.",
                           Value::object({{"error", "Approval runtime is not configured."}}));
  }
  const auto body = read_json_body(context.request);
  const auto raw_decision = body.at("decision").as_string();
  PermissionDecision decision{
      .decision = raw_decision == "allow" ? PermissionDecisionKind::Allow
                  : raw_decision == "ask" ? PermissionDecisionKind::Ask
                                          : PermissionDecisionKind::Deny,
      .reason = body.at("reason").as_string(),
  };
  const auto approval_id = context.params.at("approvalId");
  auto record = queue->resolve(approval_id, std::move(decision));
  if (!record) {
    return send_json(404, Value::object({{"error", "Approval not found: " + approval_id}}));
  }
  append_audit(AuditRecord{
      .type = "approval.resolved",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .approval_id = approval_id,
      .status_code = 200,
      .detail = Value::object({{"decision", record->final_decision
                                                 ? to_string(record->final_decision->decision)
                                                 : std::string{}},
                               {"reason", record->final_decision
                                             ? Value(record->final_decision->reason)
                                             : Value()},
                               {"toolName", record->request.tool_name}}),
  });
  return send_json(200, approval_record_to_value(*record));
}

AgentServerApp::ResolvedWorkflowRuntime AgentServerApp::resolve_workflow_runtime(
    const AgentServerAccessContext& access) const {
  ResolvedWorkflowRuntime runtime;
  runtime.agent_id = resolve_authorized_agent_id(access, {});
  if (options_.workflow) {
    runtime.engine = options_.workflow->engine;
    runtime.store = options_.workflow->store ? options_.workflow->store
                                             : (options_.workflow->engine ? options_.workflow->engine->store()
                                                                          : nullptr);
    runtime.definition = options_.workflow->definition;
  }
  if (!runtime.engine && has_config_source()) {
    runtime.config_app = resolve_config_app(runtime.agent_id);
    if (runtime.config_app && runtime.config_app->workflow) {
      runtime.engine = runtime.config_app->workflow->engine.get();
      runtime.store = runtime.config_app->workflow->store
                          ? runtime.config_app->workflow->store.get()
                          : (runtime.engine ? runtime.engine->store() : nullptr);
    }
  }
  return runtime;
}

ManualApprovalQueue* AgentServerApp::approval_queue() const {
  return approval_queue_;
}

SessionStore* AgentServerApp::session_store(const std::string& agent_id) const {
  if (options_.session_store) {
    return options_.session_store;
  }
  if (options_.runner) {
    return options_.runner->session_store();
  }
  auto app = resolve_config_app(agent_id);
  return app && app->runner ? app->runner->session_store() : nullptr;
}

std::string AgentServerApp::resolve_session_id(const Value& body) const {
  const auto requested = body.at("sessionId").as_string();
  if (options_.session.mode == "require" && requested.empty()) {
    throw HttpRequestError(400, "Session policy requires sessionId.",
                           Value::object({{"error", "Session policy requires sessionId."}}));
  }
  std::string session_id = requested;
  if (options_.session.mode == "ephemeral" || session_id.empty()) {
    session_id = "server:" + std::to_string(now_ms()) + "-" + generate_uuid();
  }
  if (!options_.session.id_prefix.empty() && session_id.rfind(options_.session.id_prefix, 0) != 0) {
    session_id = options_.session.id_prefix + session_id;
  }
  return session_id;
}

AgentServerApp create_agent_server_app(AgentServerOptions options) {
  return AgentServerApp(std::move(options));
}

}  // namespace agent
