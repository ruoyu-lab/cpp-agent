#include "agent/mcp_native.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>

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

}  // namespace agent
