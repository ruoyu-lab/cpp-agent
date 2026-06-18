#pragma once

#include "agent/http_native.hpp"
#include "agent/mcp.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace agent {

struct MCPNativeHttpTransportConfig {
  std::string url;
  std::map<std::string, std::string> headers;
  NativeHttpClientConfig http;
  HttpTransport transport;
};

class MCPNativeHttpTransport : public MCPTransport {
 public:
  explicit MCPNativeHttpTransport(MCPNativeHttpTransportConfig config);
  void start(MCPMessageHandler handler) override;
  void send(const Value& message) override;
  void close() override;

 private:
  MCPNativeHttpTransportConfig config_;
  MCPMessageHandler handler_;
  bool started_ = false;
};

struct MCPStdioTransportConfig {
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::string cwd;
  int response_timeout_ms = 30000;
};

class MCPStdioTransport : public MCPTransport {
 public:
  explicit MCPStdioTransport(MCPStdioTransportConfig config);
  ~MCPStdioTransport() override;
  void start(MCPMessageHandler handler) override;
  void send(const Value& message) override;
  void close() override;

 private:
  void wait_for_response(const std::string& id);
  std::string read_stdout_line(std::chrono::steady_clock::time_point deadline);

  MCPStdioTransportConfig config_;
  MCPMessageHandler handler_;
  bool started_ = false;
  int child_pid_ = -1;
  int stdin_fd_ = -1;
  int stdout_fd_ = -1;
  std::string stdout_buffer_;
};

std::shared_ptr<MCPTransport> create_native_mcp_transport(
    const AnthropicMcpServerConfig& config,
    int response_timeout_ms = 30000);

}  // namespace agent
