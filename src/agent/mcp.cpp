#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent {

namespace {

std::string env_value(const std::map<std::string, std::string>& env, const std::string& key) {
  const auto found = env.find(key);
  if (found != env.end()) {
    return found->second;
  }
  if (const char* value = std::getenv(key.c_str())) {
    return value;
  }
  return {};
}

std::string expand_env_string(const std::string& value, const std::map<std::string, std::string>& env) {
  std::string output;
  std::size_t pos = 0;
  while (pos < value.size()) {
    const auto start = value.find("${", pos);
    if (start == std::string::npos) {
      output += value.substr(pos);
      break;
    }
    output += value.substr(pos, start - pos);
    const auto end = value.find('}', start + 2);
    if (end == std::string::npos) {
      output += value.substr(start);
      break;
    }
    std::string expression = value.substr(start + 2, end - start - 2);
    std::string fallback;
    const auto fallback_pos = expression.find(":-");
    if (fallback_pos != std::string::npos) {
      fallback = expression.substr(fallback_pos + 2);
      expression = expression.substr(0, fallback_pos);
    }
    const auto resolved = env_value(env, expression);
    output += resolved.empty() ? fallback : resolved;
    pos = end + 1;
  }
  return output;
}

std::vector<std::string> string_array_from_value(const Value& value,
                                                 const std::map<std::string, std::string>& env) {
  std::vector<std::string> result;
  for (const auto& item : value.as_array()) {
    result.push_back(expand_env_string(item.as_string(), env));
  }
  return result;
}

std::map<std::string, std::string> string_map_from_value(const Value& value,
                                                         const std::map<std::string, std::string>& env) {
  std::map<std::string, std::string> result;
  for (const auto& [key, item] : value.as_object()) {
    result[key] = expand_env_string(item.as_string(), env);
  }
  return result;
}

std::string json_rpc_id_to_string(const Value& value) {
  if (value.is_string()) {
    return value.as_string();
  }
  if (value.is_number()) {
    std::ostringstream stream;
    stream << std::setprecision(17) << value.as_number();
    return stream.str();
  }
  return {};
}

std::string normalize_mcp_tool_name(std::string value) {
  for (char& ch : value) {
    const bool ok = std::isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-' || ch == '.';
    if (!ok) {
      ch = '-';
    }
  }
  return value;
}

std::string lower_ascii_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool starts_with_http_scheme(const std::string& value) {
  const auto normalized = lower_ascii_copy(trim_copy(value));
  return normalized.rfind("http://", 0) == 0;
}

bool starts_with_https_scheme(const std::string& value) {
  const auto normalized = lower_ascii_copy(trim_copy(value));
  return normalized.rfind("https://", 0) == 0;
}

std::string resolve_mcp_transport_type(const AnthropicMcpServerConfig& config) {
  auto type = lower_ascii_copy(trim_copy(config.type));
  if (type.empty()) {
    return config.url.empty() ? "stdio" : "http";
  }
  if (type == "stdio" && config.command.empty() && !config.url.empty()) {
    return "http";
  }
  return type;
}

void assert_native_mcp_http_url(const std::string& url) {
  if (trim_copy(url).empty()) {
    throw ConfigurationError("MCP HTTP transport requires a URL.");
  }
  if (starts_with_https_scheme(url)) {
    throw ConfigurationError(
        "Native MCP HTTP transport does not provide TLS. Use an injected HTTPS-capable transport.");
  }
  if (!starts_with_http_scheme(url)) {
    throw ConfigurationError("Native MCP HTTP transport requires an absolute http:// URL.");
  }
}

void dispatch_mcp_http_response(const Value& response, const MCPMessageHandler& handler) {
  if (response.is_null()) {
    return;
  }
  if (response.is_array()) {
    for (const auto& item : response.as_array()) {
      if (!item.is_null()) {
        handler(item);
      }
    }
    return;
  }
  if (!response.is_object()) {
    throw AdapterError("MCP HTTP transport expected a JSON-RPC response object or array.",
                       safe_json_stringify(response));
  }
  handler(response);
}

bool is_success_http_status(int status) {
  return status >= 200 && status < 300;
}

bool has_header_case_insensitive(const std::map<std::string, std::string>& headers,
                                 const std::string& name) {
  const auto expected = lower_ascii_copy(name);
  for (const auto& [key, _] : headers) {
    if (lower_ascii_copy(key) == expected) {
      return true;
    }
  }
  return false;
}

void set_default_header(std::map<std::string, std::string>& headers,
                        const std::string& name,
                        std::string value) {
  if (!has_header_case_insensitive(headers, name)) {
    headers[name] = std::move(value);
  }
}

std::vector<MessageContentPart> mcp_content_to_parts(const Value& content) {
  std::vector<MessageContentPart> parts;
  for (const auto& item : content.as_array()) {
    const auto type = item.at("type").as_string("text");
    if (type == "text") {
      parts.push_back(text_part(item.at("text").as_string()));
      continue;
    }
    if (type == "image") {
      MediaSource source;
      source.kind = MediaSourceKind::Inline;
      source.data = item.at("data").as_string();
      source.mime_type = item.at("mimeType").as_string("image/png");
      parts.push_back(image_part(std::move(source)));
      continue;
    }
    if (type == "resource") {
      const auto& resource = item.at("resource");
      if (resource.at("text").is_string()) {
        parts.push_back(text_part(resource.at("text").as_string(),
                                  Value::object({{"uri", resource.at("uri").as_string()},
                                                 {"mimeType", resource.at("mimeType").as_string()}})));
        continue;
      }
      if (resource.at("blob").is_string()) {
        MediaSource source;
        source.kind = MediaSourceKind::Inline;
        source.data = resource.at("blob").as_string();
        source.mime_type = resource.at("mimeType").as_string("application/octet-stream");
        source.filename = path_basename(resource.at("uri").as_string());
        parts.push_back(file_part(std::move(source)));
      }
    }
  }
  return parts;
}

Value tool_call_result_to_value(const MCPToolCallResult& result) {
  Value::Array content;
  for (const auto& part : result.content) {
    if (part.type == ContentPartType::Text) {
      content.push_back(Value::object({{"type", "text"}, {"text", part.text}}));
    } else if (part.type == ContentPartType::Image) {
      content.push_back(Value::object({{"type", "image"},
                                      {"data", part.source.data},
                                      {"mimeType", part.source.mime_type}}));
    } else if (part.type == ContentPartType::File) {
      content.push_back(Value::object({{"type", "resource"},
                                      {"resource", Value::object({{"uri", part.source.filename},
                                                                  {"mimeType", part.source.mime_type},
                                                                  {"blob", part.source.data}})}}));
    }
  }
  return Value::object({{"content", Value(content)},
                        {"structuredContent", result.structured_content},
                        {"isError", result.is_error},
                        {"_meta", result.metadata}});
}

MCPToolCallResult tool_call_result_from_value(const Value& value) {
  MCPToolCallResult result;
  result.content = mcp_content_to_parts(value.at("content"));
  result.structured_content = value.at("structuredContent");
  result.is_error = value.at("isError").as_bool(false);
  result.metadata = value.at("_meta").is_object() ? value.at("_meta") : Value::object({});
  return result;
}

void append_mcp_error_piece(std::string& message, const std::string& piece) {
  if (piece.empty()) {
    return;
  }
  if (!message.empty()) {
    message += "\n";
  }
  message += piece;
}

std::string mcp_tool_error_message(const MCPToolCallResult& result, const std::string& fallback) {
  std::string message;
  for (const auto& part : result.content) {
    if (part.type == ContentPartType::Text) {
      append_mcp_error_piece(message, part.text);
    } else {
      append_mcp_error_piece(message, content_part_type_label(part.type));
    }
  }
  return message.empty() ? fallback : message;
}

std::string mcp_tool_error_details(const MCPToolCallResult& result) {
  if (!result.structured_content.is_null()) {
    return safe_json_stringify(result.structured_content);
  }
  if (result.metadata.is_object() && !result.metadata.as_object().empty()) {
    return safe_json_stringify(result.metadata);
  }
  return safe_json_stringify(result.structured_content);
}

JsonSchema default_mcp_tool_input_schema() {
  JsonSchema schema;
  schema.type = JsonSchemaType::Object;
  return schema;
}

std::vector<AgentMessage> prompt_messages_from_value(const Value& value) {
  std::vector<AgentMessage> messages;
  for (const auto& item : value.at("messages").as_array()) {
    auto parts = mcp_content_to_parts(Value::array({item.at("content")}));
    if (parts.empty()) {
      parts.push_back(text_part(""));
    }
    const auto role = item.at("role").as_string("user") == "assistant" ? MessageRole::Assistant : MessageRole::User;
    messages.push_back(create_message(role, std::move(parts),
                                      Value::object({{"source", "mcp-prompt"}})));
  }
  return messages;
}

void normalize_resource_read_result(MCPResourceReadResult& result, const std::string& fallback_uri) {
  if (result.uri.empty()) {
    result.uri = fallback_uri;
  }
  if (result.contents.empty() && (!result.text.empty() || !result.blob.empty() || !result.mime_type.empty())) {
    result.contents.push_back(MCPResourceContentItem{
        .uri = result.uri.empty() ? fallback_uri : result.uri,
        .mime_type = result.mime_type,
        .text = result.text,
        .blob = result.blob,
    });
  }
  if (result.contents.empty()) {
    return;
  }

  if (result.mime_type.empty()) {
    result.mime_type = result.contents.front().mime_type;
  }
  if (result.text.empty()) {
    for (const auto& item : result.contents) {
      if (item.text.empty()) {
        continue;
      }
      if (!result.text.empty()) {
        result.text += "\n\n";
      }
      result.text += item.text;
    }
  }
  if (result.blob.empty()) {
    for (const auto& item : result.contents) {
      result.blob.insert(result.blob.end(), item.blob.begin(), item.blob.end());
    }
  }
}

#ifndef _WIN32
void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void throw_errno_error(const std::string& message) {
  throw AgentFrameworkError(message + ": " + std::strerror(errno));
}

void write_all_to_fd(int fd, const std::string& data) {
  std::size_t offset = 0;
  while (offset < data.size()) {
    const auto written = ::write(fd, data.data() + offset, data.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno_error("Failed to write to MCP stdio transport");
    }
    if (written == 0) {
      throw AgentFrameworkError("Failed to write to MCP stdio transport.");
    }
    offset += static_cast<std::size_t>(written);
  }
}
#endif

}  // namespace

Value json_rpc_request_to_value(const JsonRpcRequest& request) {
  Value::Object object{{"jsonrpc", "2.0"}, {"id", request.id}, {"method", request.method}};
  if (!request.params.is_null()) {
    object["params"] = request.params;
  }
  return Value(object);
}

JsonRpcRequest json_rpc_request_from_value(const Value& value) {
  return JsonRpcRequest{json_rpc_id_to_string(value.at("id")), value.at("method").as_string(), value.at("params")};
}

Value json_rpc_success_to_value(const std::string& id, Value result) {
  return Value::object({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

Value json_rpc_error_to_value(const std::string& id, int code, std::string message, Value data) {
  return Value::object({{"jsonrpc", "2.0"},
                        {"id", id.empty() ? Value() : Value(id)},
                        {"error", Value::object({{"code", code}, {"message", std::move(message)}, {"data", data}})}});
}

JsonRpcResponse json_rpc_response_from_value(const Value& value) {
  JsonRpcResponse response;
  response.id = json_rpc_id_to_string(value.at("id"));
  if (value.at("error").is_object()) {
    response.error_code = static_cast<int>(value.at("error").at("code").as_integer());
    response.error_message = value.at("error").at("message").as_string();
    response.error_data = value.at("error").at("data");
    return response;
  }
  response.result = value.at("result");
  return response;
}

MCPCallbackTransport::MCPCallbackTransport(MCPTransportExchange exchange)
    : exchange_(std::move(exchange)) {
  if (!exchange_) {
    throw ConfigurationError("MCP callback transport requires an exchange function.");
  }
}

void MCPCallbackTransport::start(MCPMessageHandler handler) {
  handler_ = std::move(handler);
  started_ = true;
}

void MCPCallbackTransport::send(const Value& message) {
  if (!started_ || !handler_) {
    throw AgentFrameworkError("MCP transport is not started.");
  }
  auto response = exchange_(message);
  if (!response.is_null()) {
    handler_(response);
  }
}

void MCPCallbackTransport::close() {
  handler_ = {};
  started_ = false;
}

MCPHttpTransport::MCPHttpTransport(MCPTransportExchange post) : MCPCallbackTransport(std::move(post)) {}

MCPNativeHttpTransport::MCPNativeHttpTransport(MCPNativeHttpTransportConfig config)
    : config_(std::move(config)) {
  assert_native_mcp_http_url(config_.url);
  if (config_.http.timeout_ms <= 0) {
    config_.http.timeout_ms = 30000;
  }
  if (!config_.transport) {
    config_.transport = create_native_http_transport(config_.http);
  }
}

void MCPNativeHttpTransport::start(MCPMessageHandler handler) {
  handler_ = std::move(handler);
  started_ = true;
}

void MCPNativeHttpTransport::send(const Value& message) {
  if (!started_ || !handler_) {
    throw AgentFrameworkError("MCP HTTP transport is not started.");
  }
  auto headers = config_.headers;
  set_default_header(headers, "accept", "application/json");
  set_default_header(headers, "content-type", "application/json");
  const auto response = config_.transport(HttpRequest{
      .url = config_.url,
      .method = "POST",
      .headers = std::move(headers),
      .body = message.stringify(0),
  });
  const auto data = parse_http_response_body(response.body);
  if (!is_success_http_status(response.status)) {
    throw AdapterError("MCP HTTP request failed with status " + std::to_string(response.status) + ".",
                       safe_json_stringify(data));
  }
  dispatch_mcp_http_response(data, handler_);
}

void MCPNativeHttpTransport::close() {
  handler_ = {};
  started_ = false;
}

MCPLineTransport::MCPLineTransport(std::istream& input, std::ostream& output)
    : input_(&input), output_(&output) {}

void MCPLineTransport::start(MCPMessageHandler handler) {
  handler_ = std::move(handler);
  started_ = true;
}

void MCPLineTransport::send(const Value& message) {
  if (!started_ || !output_) {
    throw AgentFrameworkError("MCP line transport is not started.");
  }
  (*output_) << message.stringify() << "\n";
  output_->flush();
}

void MCPLineTransport::close() {
  handler_ = {};
  started_ = false;
}

void MCPLineTransport::poll() {
  if (!started_ || !handler_ || !input_) {
    return;
  }
  std::string line;
  while (std::getline(*input_, line)) {
    line = trim_copy(std::move(line));
    if (!line.empty()) {
      handler_(parse_json(line));
    }
  }
}

MCPStdioTransport::MCPStdioTransport(MCPStdioTransportConfig config)
    : config_(std::move(config)) {
  if (config_.command.empty()) {
    throw ConfigurationError("MCP stdio transport requires a command.");
  }
  if (config_.response_timeout_ms <= 0) {
    config_.response_timeout_ms = 30000;
  }
}

MCPStdioTransport::~MCPStdioTransport() {
  close();
}

void MCPStdioTransport::start(MCPMessageHandler handler) {
  if (started_) {
    handler_ = std::move(handler);
    return;
  }
  handler_ = std::move(handler);
#ifdef _WIN32
  throw AdapterError("MCP stdio transport is not available on this platform.");
#else
  if (!config_.cwd.empty()) {
    std::error_code error;
    if (!std::filesystem::is_directory(config_.cwd, error)) {
      throw ConfigurationError("MCP stdio transport cwd does not exist or is not a directory: " + config_.cwd);
    }
  }
  int input_pipe[2]{-1, -1};
  int output_pipe[2]{-1, -1};
  if (::pipe(input_pipe) != 0) {
    throw_errno_error("Failed to create MCP stdin pipe");
  }
  if (::pipe(output_pipe) != 0) {
    close_fd(input_pipe[0]);
    close_fd(input_pipe[1]);
    throw_errno_error("Failed to create MCP stdout pipe");
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    close_fd(input_pipe[0]);
    close_fd(input_pipe[1]);
    close_fd(output_pipe[0]);
    close_fd(output_pipe[1]);
    throw_errno_error("Failed to fork MCP stdio process");
  }

  if (pid == 0) {
    ::dup2(input_pipe[0], STDIN_FILENO);
    ::dup2(output_pipe[1], STDOUT_FILENO);
    const int dev_null = ::open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      ::dup2(dev_null, STDERR_FILENO);
      ::close(dev_null);
    }
    ::close(input_pipe[0]);
    ::close(input_pipe[1]);
    ::close(output_pipe[0]);
    ::close(output_pipe[1]);
    if (!config_.cwd.empty()) {
      (void)::chdir(config_.cwd.c_str());
    }
    for (const auto& [key, value] : config_.env) {
      ::setenv(key.c_str(), value.c_str(), 1);
    }
    std::vector<std::string> argv_storage;
    argv_storage.reserve(config_.args.size() + 1);
    argv_storage.push_back(config_.command);
    for (const auto& arg : config_.args) {
      argv_storage.push_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& arg : argv_storage) {
      argv.push_back(arg.data());
    }
    argv.push_back(nullptr);
    ::execvp(config_.command.c_str(), argv.data());
    ::_exit(127);
  }

  close_fd(input_pipe[0]);
  close_fd(output_pipe[1]);
  child_pid_ = static_cast<int>(pid);
  stdin_fd_ = input_pipe[1];
  stdout_fd_ = output_pipe[0];
  started_ = true;
#endif
}

void MCPStdioTransport::send(const Value& message) {
  if (!started_ || stdin_fd_ < 0) {
    throw AgentFrameworkError("MCP transport is not started.");
  }
  const auto target_id = json_rpc_id_to_string(message.at("id"));
#ifdef _WIN32
  (void)message;
  throw AdapterError("MCP stdio transport is not available on this platform.");
#else
  write_all_to_fd(stdin_fd_, message.stringify() + "\n");
  if (!target_id.empty()) {
    wait_for_response(target_id);
  }
#endif
}

void MCPStdioTransport::close() {
  handler_ = {};
  stdout_buffer_.clear();
#ifdef _WIN32
  started_ = false;
#else
  close_fd(stdin_fd_);
  close_fd(stdout_fd_);
  if (child_pid_ > 0) {
    int status = 0;
    const pid_t pid = static_cast<pid_t>(child_pid_);
    if (::waitpid(pid, &status, WNOHANG) == 0) {
      ::kill(pid, SIGTERM);
      bool exited = false;
      for (int attempt = 0; attempt < 50; ++attempt) {
        if (::waitpid(pid, &status, WNOHANG) != 0) {
          exited = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      if (!exited) {
        ::kill(pid, SIGKILL);
        (void)::waitpid(pid, &status, 0);
      }
    }
  }
  child_pid_ = -1;
  started_ = false;
#endif
}

void MCPStdioTransport::wait_for_response(const std::string& id) {
  if (!handler_) {
    throw AgentFrameworkError("MCP stdio transport has no message handler.");
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(config_.response_timeout_ms);
  while (true) {
    auto line = trim_copy(read_stdout_line(deadline));
    if (line.empty()) {
      continue;
    }
    auto message = parse_json(line);
    handler_(message);
    if (json_rpc_id_to_string(message.at("id")) == id) {
      return;
    }
  }
}

std::string MCPStdioTransport::read_stdout_line(std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
  (void)deadline;
  throw AdapterError("MCP stdio transport is not available on this platform.");
#else
  while (true) {
    const auto separator = stdout_buffer_.find('\n');
    if (separator != std::string::npos) {
      auto line = stdout_buffer_.substr(0, separator);
      stdout_buffer_.erase(0, separator + 1);
      return line;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      throw TimeoutError("Timed out waiting for MCP stdio response.", config_.command,
                         config_.response_timeout_ms);
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    timeval timeout{};
    timeout.tv_sec = static_cast<long>(remaining.count() / 1000000);
    timeout.tv_usec = static_cast<long>(remaining.count() % 1000000);
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(stdout_fd_, &read_fds);
    const int ready = ::select(stdout_fd_ + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno_error("Failed to read MCP stdio response");
    }
    if (ready == 0) {
      throw TimeoutError("Timed out waiting for MCP stdio response.", config_.command,
                         config_.response_timeout_ms);
    }
    char buffer[4096];
    const auto received = ::read(stdout_fd_, buffer, sizeof(buffer));
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno_error("Failed to read MCP stdio response");
    }
    if (received == 0) {
      throw AgentFrameworkError("MCP stdio process closed stdout before responding.");
    }
    stdout_buffer_.append(buffer, static_cast<std::size_t>(received));
  }
#endif
}

AnthropicMcpConfigFile parse_anthropic_mcp_config(const std::string& raw,
                                                  const std::map<std::string, std::string>& env) {
  const auto parsed = parse_json(raw);
  if (!parsed.is_object()) {
    throw ConfigurationError("Anthropic MCP config must be a JSON object.");
  }
  AnthropicMcpConfigFile file;
  for (const auto& [name, server] : parsed.at("mcpServers").as_object()) {
    if (!server.is_object()) {
      throw ConfigurationError("MCP server \"" + name + "\" must be an object.");
    }
    AnthropicMcpServerConfig config;
    config.name = name;
    config.command = expand_env_string(server.at("command").as_string(), env);
    config.args = string_array_from_value(server.at("args"), env);
    config.env = string_map_from_value(server.at("env"), env);
    config.cwd = expand_env_string(server.at("cwd").as_string(), env);
    config.url = expand_env_string(server.at("url").as_string(), env);
    config.type = server.at("type").as_string(config.url.empty() ? "stdio" : "http");
    config.headers = string_map_from_value(server.at("headers"), env);
    config.tier = server.at("tier").as_string();
    file.mcp_servers[name] = std::move(config);
  }
  return file;
}

AnthropicMcpConfigFile load_anthropic_mcp_config_file(const std::filesystem::path& file_path,
                                                      const std::map<std::string, std::string>& env) {
  auto config = parse_anthropic_mcp_config(bytes_to_text(read_binary_file(file_path)), env);
  for (auto& [name, server] : config.mcp_servers) {
    server = resolve_anthropic_mcp_server_config(name, std::move(server), file_path);
  }
  return config;
}

std::optional<std::filesystem::path> find_anthropic_mcp_config_file(std::filesystem::path cwd,
                                                                    std::string file_name) {
  std::error_code error;
  auto current = std::filesystem::absolute(cwd, error).lexically_normal();
  while (true) {
    const auto candidate = current / file_name;
    if (std::filesystem::exists(candidate, error)) {
      return candidate;
    }
    const auto parent = current.parent_path();
    if (parent.empty() || parent == current) {
      return std::nullopt;
    }
    current = parent;
  }
}

AnthropicMcpServerConfig resolve_anthropic_mcp_server_config(std::string name,
                                                             AnthropicMcpServerConfig config,
                                                             std::filesystem::path file_path) {
  config.name = std::move(name);
  config.file_path = file_path.string();
  if (!config.command.empty() && config.command.front() == '.') {
    const auto base = file_path.empty() ? std::filesystem::current_path() : file_path.parent_path();
    config.command = (base / config.command).lexically_normal().string();
  }
  if (!config.cwd.empty() && config.cwd.front() == '.') {
    const auto base = file_path.empty() ? std::filesystem::current_path() : file_path.parent_path();
    config.cwd = (base / config.cwd).lexically_normal().string();
  }
  return config;
}

std::shared_ptr<MCPTransport> create_native_mcp_transport(const AnthropicMcpServerConfig& config,
                                                          int response_timeout_ms) {
  const auto type = resolve_mcp_transport_type(config);
  if (type == "http") {
    NativeHttpClientConfig http_config;
    http_config.timeout_ms = response_timeout_ms > 0 ? response_timeout_ms : 30000;
    return std::make_shared<MCPNativeHttpTransport>(MCPNativeHttpTransportConfig{
        .url = config.url,
        .headers = config.headers,
        .http = http_config,
    });
  }
  if (type != "stdio") {
    throw ConfigurationError("MCP transport \"" + type + "\" is not implemented. Supported native transports: stdio, http.");
  }
  if (config.command.empty()) {
    throw ConfigurationError("MCP server \"" + config.name + "\" requires a command for stdio transport.");
  }
  std::string cwd = config.cwd;
  if (cwd.empty() && !config.file_path.empty()) {
    cwd = std::filesystem::path(config.file_path).parent_path().string();
  }
  return std::make_shared<MCPStdioTransport>(MCPStdioTransportConfig{
      .command = config.command,
      .args = config.args,
      .env = config.env,
      .cwd = std::move(cwd),
      .response_timeout_ms = response_timeout_ms,
  });
}

InProcessMCPServer::InProcessMCPServer(std::string name, std::string version)
    : name_(std::move(name)), version_(std::move(version)) {}

void InProcessMCPServer::register_tool(ToolDefinition tool) {
  tools_.register_tool(std::move(tool));
}

void InProcessMCPServer::register_resource(MCPResourceDefinition definition, MCPResourceReadResult contents) {
  normalize_resource_read_result(contents, definition.uri);
  std::lock_guard<std::mutex> lock(mutex_);
  resource_contents_[definition.uri] = std::move(contents);
  resources_[definition.uri] = std::move(definition);
}

void InProcessMCPServer::register_prompt(MCPPromptDefinition prompt) {
  std::lock_guard<std::mutex> lock(mutex_);
  prompts_[prompt.name] = std::move(prompt);
}

Value InProcessMCPServer::initialize() const {
  return Value::object({{"protocolVersion", MCP_PROTOCOL_VERSION},
                        {"serverInfo", Value::object({{"name", name_}, {"version", version_}})},
                        {"capabilities", Value::object({{"tools", Value::object({{"listChanged", false}})},
                                                        {"resources", Value::object({{"listChanged", false}})},
                                                        {"prompts", Value::object({{"listChanged", false}})}})}});
}

std::vector<MCPToolDefinition> InProcessMCPServer::list_tools() const {
  std::vector<MCPToolDefinition> tools;
  for (const auto& tool : tools_.list()) {
    tools.push_back(MCPToolDefinition{tool.name, tool.description, tool.input_schema});
  }
  return tools;
}

MCPToolCallResult InProcessMCPServer::call_tool(const std::string& name, Value arguments) {
  ToolExecutor executor(tools_);
  auto result = executor.execute_tool_call(ToolCall{generate_uuid(), name, std::move(arguments)});
  return MCPToolCallResult{{result.message.content.empty() ? text_part(result.output) : result.message.content.front()},
                           result.result && std::holds_alternative<Value>(*result.result)
                               ? std::get<Value>(*result.result)
                               : Value{},
                           !result.ok,
                           Value::object({{"toolCallId", result.tool_call.id}, {"error", result.error}})};
}

std::vector<MCPResourceDefinition> InProcessMCPServer::list_resources() const {
  std::vector<MCPResourceDefinition> resources;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [_, resource] : resources_) {
    resources.push_back(resource);
  }
  return resources;
}

MCPResourceReadResult InProcessMCPServer::read_resource(const std::string& uri) const {
  MCPResourceReadResult result;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = resource_contents_.find(uri);
    if (found == resource_contents_.end()) {
      throw AgentFrameworkError("MCP resource not found: " + uri);
    }
    result = found->second;
  }
  normalize_resource_read_result(result, uri);
  return result;
}

std::vector<MCPPromptDefinition> InProcessMCPServer::list_prompts() const {
  std::vector<MCPPromptDefinition> prompts;
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [_, prompt] : prompts_) {
    prompts.push_back(prompt);
  }
  return prompts;
}

std::vector<AgentMessage> InProcessMCPServer::get_prompt(const std::string& name, Value arguments) const {
  MCPPromptDefinition prompt;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = prompts_.find(name);
    if (found == prompts_.end()) {
      throw AgentFrameworkError("MCP prompt not found: " + name);
    }
    prompt = found->second;
  }
  return prompt.render ? prompt.render(arguments) : std::vector<AgentMessage>{};
}

InProcessMCPClient::InProcessMCPClient(InProcessMCPServer& server, std::string name)
    : server_(server), name_(std::move(name)) {}

Value InProcessMCPClient::connect() {
  connected_ = true;
  auto result = server_.initialize();
  result["clientInfo"] = Value::object({{"name", name_}});
  return result;
}

std::vector<MCPToolDefinition> InProcessMCPClient::list_tools() const {
  return server_.list_tools();
}

MCPToolCallResult InProcessMCPClient::call_tool(const std::string& name, Value arguments) {
  if (!connected_) {
    throw AgentFrameworkError("MCP client is not connected.");
  }
  return server_.call_tool(name, std::move(arguments));
}

std::vector<MCPResourceDefinition> InProcessMCPClient::list_resources() const {
  return server_.list_resources();
}

MCPResourceReadResult InProcessMCPClient::read_resource(const std::string& uri) const {
  return server_.read_resource(uri);
}

std::vector<MCPPromptDefinition> InProcessMCPClient::list_prompts() const {
  return server_.list_prompts();
}

std::vector<AgentMessage> InProcessMCPClient::get_prompt(const std::string& name, Value arguments) const {
  return server_.get_prompt(name, std::move(arguments));
}

MCPClient::MCPClient(std::string name, std::shared_ptr<MCPTransport> transport,
                     std::string client_name, std::string client_version, Value capabilities,
                     int request_timeout_ms)
    : name_(std::move(name)),
      transport_(std::move(transport)),
      client_name_(std::move(client_name)),
      client_version_(std::move(client_version)),
      capabilities_(capabilities.is_object() ? std::move(capabilities) : Value::object({})),
      request_timeout_ms_(request_timeout_ms > 0 ? request_timeout_ms : 30000) {
  if (name_.empty()) {
    throw ConfigurationError("MCPClient requires a name.");
  }
  if (!transport_) {
    throw ConfigurationError("MCPClient requires a transport.");
  }
}

const std::string& MCPClient::name() const noexcept {
  return name_;
}

std::string MCPClient::next_id() {
  request_id_ += 1;
  return std::to_string(request_id_);
}

void MCPClient::handle_message(const Value& message) {
  if (!message.contains("id")) {
    return;
  }
  auto response = json_rpc_response_from_value(message);
  if (!response.id.empty()) {
    const auto response_id = response.id;
    {
      std::lock_guard<std::mutex> lock(responses_mutex_);
      if (closed_) {
        return;
      }
      responses_[response_id] = std::move(response);
    }
    responses_cv_.notify_all();
  }
}

Value MCPClient::connect() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  if (connected_) {
    return initialize_result_;
  }
  transport_->start([this](const Value& message) {
    handle_message(message);
  });
  {
    std::lock_guard<std::mutex> lock(responses_mutex_);
    closed_ = false;
    responses_.clear();
  }
  initialize_result_ = request("initialize", Value::object({
      {"protocolVersion", MCP_PROTOCOL_VERSION},
      {"capabilities", capabilities_},
      {"clientInfo", Value::object({{"name", client_name_}, {"version", client_version_}})},
  }));
  notify("notifications/initialized");
  connected_ = true;
  return initialize_result_;
}

void MCPClient::close() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  connected_ = false;
  initialize_result_ = Value{};
  {
    std::lock_guard<std::mutex> lock(responses_mutex_);
    closed_ = true;
    responses_.clear();
  }
  responses_cv_.notify_all();
  transport_->close();
}

Value MCPClient::request(const std::string& method, Value params) {
  std::string id;
  {
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    id = next_id();
    transport_->send(json_rpc_request_to_value(JsonRpcRequest{id, method, std::move(params)}));
  }

  JsonRpcResponse response;
  {
    std::unique_lock<std::mutex> lock(responses_mutex_);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(request_timeout_ms_);
    responses_cv_.wait_until(lock, deadline, [&]() {
      return responses_.find(id) != responses_.end() || closed_;
    });
    const auto found = responses_.find(id);
    if (found == responses_.end()) {
      if (closed_) {
        throw AgentFrameworkError("MCP client closed before response for request " + id + ".");
      }
      throw TimeoutError("Timed out waiting for MCP response.", method, request_timeout_ms_);
    }
    response = found->second;
    responses_.erase(found);
  }

  if (response.error_code != 0 || !response.error_message.empty()) {
    throw AgentFrameworkError(response.error_message.empty() ? "MCP request failed." : response.error_message,
                              safe_json_stringify(response.error_data));
  }
  return response.result;
}

void MCPClient::notify(const std::string& method, Value params) {
  Value::Object message{{"jsonrpc", "2.0"}, {"method", method}};
  if (!params.is_null()) {
    message["params"] = params;
  }
  std::lock_guard<std::mutex> request_lock(request_mutex_);
  transport_->send(Value(message));
}

std::vector<MCPToolDefinition> MCPClient::list_tools() {
  const auto result = request("tools/list");
  std::vector<MCPToolDefinition> tools;
  for (const auto& item : result.at("tools").as_array()) {
    JsonSchema input_schema = default_mcp_tool_input_schema();
    if (item.at("inputSchema").is_object()) {
      input_schema = json_schema_from_value(item.at("inputSchema"));
    }
    tools.push_back(MCPToolDefinition{
        item.at("name").as_string(),
        item.at("description").as_string(),
        std::move(input_schema),
    });
  }
  return tools;
}

MCPToolCallResult MCPClient::call_tool(const std::string& name, Value arguments) {
  return tool_call_result_from_value(request("tools/call", Value::object({
      {"name", name},
      {"arguments", arguments.is_object() ? arguments : Value::object({})},
  })));
}

std::vector<MCPResourceDefinition> MCPClient::list_resources() {
  const auto result = request("resources/list");
  std::vector<MCPResourceDefinition> resources;
  for (const auto& item : result.at("resources").as_array()) {
    resources.push_back(MCPResourceDefinition{
        item.at("uri").as_string(),
        item.at("name").as_string(),
        item.at("description").as_string(),
        item.at("mimeType").as_string(),
    });
  }
  return resources;
}

MCPResourceReadResult MCPClient::read_resource(const std::string& uri) {
  const auto result = request("resources/read", Value::object({{"uri", uri}}));
  MCPResourceReadResult combined;
  combined.uri = uri;
  bool first = true;
  for (const auto& item : result.at("contents").as_array()) {
    MCPResourceContentItem content;
    content.uri = item.at("uri").as_string(uri);
    content.mime_type = item.at("mimeType").as_string();
    content.text = item.at("text").as_string();
    const auto blob = item.at("blob").as_string();
    if (!blob.empty()) {
      content.blob = decode_base64(blob);
    }
    combined.contents.push_back(content);
    if (first) {
      combined.mime_type = content.mime_type;
      first = false;
    }
    if (!content.text.empty()) {
      if (!combined.text.empty()) {
        combined.text += "\n\n";
      }
      combined.text += content.text;
    }
    if (!content.blob.empty()) {
      combined.blob.insert(combined.blob.end(), content.blob.begin(), content.blob.end());
    }
  }
  normalize_resource_read_result(combined, uri);
  return combined;
}

std::vector<MCPPromptDefinition> MCPClient::list_prompts() {
  const auto result = request("prompts/list");
  std::vector<MCPPromptDefinition> prompts;
  for (const auto& item : result.at("prompts").as_array()) {
    std::vector<std::string> arguments;
    std::vector<MCPPromptArgument> argument_definitions;
    for (const auto& arg : item.at("arguments").as_array()) {
      MCPPromptArgument definition;
      if (arg.is_string()) {
        definition.name = arg.as_string();
      } else {
        definition.name = arg.at("name").as_string();
        definition.description = arg.at("description").as_string();
        definition.required = arg.at("required").as_bool(false);
      }
      if (!definition.name.empty()) {
        arguments.push_back(definition.name);
        argument_definitions.push_back(std::move(definition));
      }
    }
    prompts.push_back(MCPPromptDefinition{
        item.at("name").as_string(),
        item.at("description").as_string(),
        std::move(arguments),
        std::move(argument_definitions),
        {},
    });
  }
  return prompts;
}

std::vector<AgentMessage> MCPClient::get_prompt(const std::string& name, Value arguments) {
  return prompt_messages_from_value(request("prompts/get", Value::object({
      {"name", name},
      {"arguments", arguments.is_object() ? arguments : Value::object({})},
  })));
}

std::vector<ToolDefinition> create_mcp_tool_definitions(MCPClient& client,
                                                        std::string prefix,
                                                        bool include_server_tag) {
  if (prefix.empty()) {
    prefix = normalize_mcp_tool_name(client.name());
  }
  std::vector<ToolDefinition> tools;
  for (const auto& tool : client.list_tools()) {
    const auto remote_name = tool.name;
    std::vector<std::string> tags{"mcp", "mcp:" + client.name()};
    if (include_server_tag) {
      tags.push_back("server:" + client.name());
    }
    tools.push_back(define_tool(ToolDefinition{
        .name = prefix + "." + normalize_mcp_tool_name(tool.name),
        .description = tool.description.empty() ? "MCP tool " + tool.name + " from " + client.name()
                                                : tool.description,
        .input_schema = tool.input_schema,
        .tags = tags,
        .bundle = "mcp",
        .builtin = false,
        .execute = [&client, remote_name](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
          auto result = client.call_tool(remote_name, input);
          if (result.is_error) {
            throw AgentFrameworkError(mcp_tool_error_message(result, "MCP tool " + remote_name + " failed."),
                                      mcp_tool_error_details(result));
          }
          return ToolResultEnvelope{
              .content = result.content.empty() ? std::nullopt
                                                : std::optional<std::vector<MessageContentPart>>(result.content),
              .value = result.structured_content.is_null()
                           ? std::optional<Value>(tool_call_result_to_value(result).at("content"))
                           : std::optional<Value>(result.structured_content),
              .metadata = Value::object({{"server", client.name()}, {"toolName", remote_name}}),
          };
        },
    }));
  }
  return tools;
}

std::vector<ToolDefinition> create_mcp_tool_definitions(std::shared_ptr<MCPClient> client,
                                                        std::string prefix,
                                                        bool include_server_tag) {
  if (!client) {
    throw ConfigurationError("MCP client is required.");
  }
  if (prefix.empty()) {
    prefix = normalize_mcp_tool_name(client->name());
  }
  const std::string server_name = client->name();
  std::vector<ToolDefinition> tools;
  for (const auto& tool : client->list_tools()) {
    const auto remote_name = tool.name;
    std::vector<std::string> tags{"mcp", "mcp:" + server_name};
    if (include_server_tag) {
      tags.push_back("server:" + server_name);
    }
    tools.push_back(define_tool(ToolDefinition{
        .name = prefix + "." + normalize_mcp_tool_name(tool.name),
        .description = tool.description.empty() ? "MCP tool " + tool.name + " from " + server_name
                                                : tool.description,
        .input_schema = tool.input_schema,
        .tags = tags,
        .bundle = "mcp",
        .builtin = false,
        .execute = [client, remote_name, server_name](const Value& input,
                                                      ToolExecutionContext&) -> ToolInvokeResult {
          auto result = client->call_tool(remote_name, input);
          if (result.is_error) {
            throw AgentFrameworkError(mcp_tool_error_message(result, "MCP tool " + remote_name + " failed."),
                                      mcp_tool_error_details(result));
          }
          return ToolResultEnvelope{
              .content = result.content.empty() ? std::nullopt
                                                : std::optional<std::vector<MessageContentPart>>(result.content),
              .value = result.structured_content.is_null()
                           ? std::optional<Value>(tool_call_result_to_value(result).at("content"))
                           : std::optional<Value>(result.structured_content),
              .metadata = Value::object({{"server", server_name}, {"toolName", remote_name}}),
          };
        },
    }));
  }
  return tools;
}

std::string read_mcp_resource_text(MCPClient& client, const std::string& uri) {
  auto result = client.read_resource(uri);
  normalize_resource_read_result(result, uri);
  std::string text;
  for (const auto& item : result.contents) {
    std::string entry;
    if (!item.text.empty()) {
      entry = item.text;
    } else if (!item.blob.empty()) {
      entry = "[binary:" + (item.mime_type.empty() ? std::string("application/octet-stream") : item.mime_type) + "]";
    }
    if (entry.empty()) {
      continue;
    }
    if (!text.empty()) {
      text += "\n\n";
    }
    text += entry;
  }
  return text;
}

AgentMessage create_mcp_resource_message(MCPClient& client, const std::string& uri) {
  auto result = client.read_resource(uri);
  normalize_resource_read_result(result, uri);
  std::vector<MessageContentPart> parts;
  for (const auto& item : result.contents) {
    const auto item_uri = item.uri.empty() ? uri : item.uri;
    if (!item.text.empty()) {
      parts.push_back(text_part(item.text, Value::object({{"uri", item_uri}, {"mimeType", item.mime_type}})));
      continue;
    }
    if (!item.blob.empty()) {
      MediaSource source;
      source.kind = MediaSourceKind::Inline;
      source.data = base64_encode(item.blob);
      source.mime_type = item.mime_type.empty() ? "application/octet-stream" : item.mime_type;
      source.filename = path_basename(item_uri);
      parts.push_back(file_part(std::move(source)));
    }
  }
  if (parts.empty()) {
    parts.push_back(text_part(""));
  }
  return create_message(MessageRole::System, std::move(parts),
                        Value::object({{"source", "mcp-resource"},
                                       {"server", client.name()},
                                       {"uri", uri}}));
}

std::vector<AgentMessage> create_mcp_prompt_messages(MCPClient& client, const std::string& name, Value arguments) {
  return client.get_prompt(name, std::move(arguments));
}

std::string render_mcp_tool_summary(const std::vector<ToolDefinition>& tools) {
  std::ostringstream output;
  for (std::size_t index = 0; index < tools.size(); ++index) {
    if (index > 0) {
      output << '\n';
    }
    output << "- " << tools[index].name << ": " << tools[index].description;
  }
  return output.str();
}

std::string render_mcp_structured_value(const Value& value) {
  return value.is_string() ? value.as_string() : safe_json_stringify(value);
}
}  // namespace agent
