#pragma once

#include "agent/tools.hpp"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace agent {

constexpr const char* MCP_PROTOCOL_VERSION = "2025-03-26";

struct JsonRpcRequest {
  std::string id;
  std::string method;
  Value params;
};

struct JsonRpcResponse {
  std::string id;
  Value result;
  int error_code = 0;
  std::string error_message;
  Value error_data;
};

Value json_rpc_request_to_value(const JsonRpcRequest& request);
JsonRpcRequest json_rpc_request_from_value(const Value& value);
Value json_rpc_success_to_value(const std::string& id, Value result = Value::object({}));
Value json_rpc_error_to_value(const std::string& id, int code, std::string message, Value data = {});
JsonRpcResponse json_rpc_response_from_value(const Value& value);

using MCPMessageHandler = std::function<void(const Value&)>;

class MCPTransport {
 public:
  virtual ~MCPTransport() = default;
  virtual void start(MCPMessageHandler handler) = 0;
  virtual void send(const Value& message) = 0;
  virtual void close() = 0;
};

using MCPTransportExchange = std::function<Value(const Value& message)>;

class MCPCallbackTransport : public MCPTransport {
 public:
  explicit MCPCallbackTransport(MCPTransportExchange exchange);
  void start(MCPMessageHandler handler) override;
  void send(const Value& message) override;
  void close() override;

 private:
  MCPTransportExchange exchange_;
  MCPMessageHandler handler_;
  bool started_ = false;
};

class MCPHttpTransport : public MCPCallbackTransport {
 public:
  explicit MCPHttpTransport(MCPTransportExchange post);
};

class MCPLineTransport : public MCPTransport {
 public:
  MCPLineTransport(std::istream& input, std::ostream& output);
  void start(MCPMessageHandler handler) override;
  void send(const Value& message) override;
  void close() override;
  void poll();

 private:
  std::istream* input_;
  std::ostream* output_;
  MCPMessageHandler handler_;
  bool started_ = false;
};

struct MCPToolDefinition {
  std::string name;
  std::string description;
  JsonSchema input_schema;
};

struct MCPToolCallResult {
  std::vector<MessageContentPart> content;
  Value structured_content;
  bool is_error = false;
  Value metadata = Value::object({});
};

struct MCPResourceDefinition {
  std::string uri;
  std::string name;
  std::string description;
  std::string mime_type;
};

struct MCPResourceContentItem {
  std::string uri;
  std::string mime_type;
  std::string text;
  std::vector<std::uint8_t> blob;
};

struct MCPResourceReadResult {
  std::string uri;
  std::string mime_type;
  std::string text;
  std::vector<std::uint8_t> blob;
  std::vector<MCPResourceContentItem> contents;
};

struct MCPPromptArgument {
  std::string name;
  std::string description;
  bool required = false;
};

struct MCPPromptDefinition {
  std::string name;
  std::string description;
  std::vector<std::string> arguments;
  std::vector<MCPPromptArgument> argument_definitions;
  std::function<std::vector<AgentMessage>(const Value&)> render;
};

struct AnthropicMcpServerConfig {
  std::string name;
  std::string type = "stdio";
  std::string command;
  std::vector<std::string> args;
  std::map<std::string, std::string> env;
  std::string cwd;
  std::string url;
  std::map<std::string, std::string> headers;
  std::string tier;
  std::string file_path;
};

struct AnthropicMcpConfigFile {
  std::map<std::string, AnthropicMcpServerConfig> mcp_servers;
};

AnthropicMcpConfigFile parse_anthropic_mcp_config(
    const std::string& raw,
    const std::map<std::string, std::string>& env = {});
AnthropicMcpConfigFile load_anthropic_mcp_config_file(
    const std::filesystem::path& file_path,
    const std::map<std::string, std::string>& env = {});
std::optional<std::filesystem::path> find_anthropic_mcp_config_file(
    std::filesystem::path cwd = std::filesystem::current_path(),
    std::string file_name = ".mcp.json");
AnthropicMcpServerConfig resolve_anthropic_mcp_server_config(
    std::string name,
    AnthropicMcpServerConfig config,
    std::filesystem::path file_path = {});

class InProcessMCPServer {
 public:
  explicit InProcessMCPServer(std::string name = "native-agent-mcp", std::string version = "0.1.0");
  void register_tool(ToolDefinition tool);
  void register_resource(MCPResourceDefinition definition, MCPResourceReadResult contents);
  void register_prompt(MCPPromptDefinition prompt);
  [[nodiscard]] Value initialize() const;
  [[nodiscard]] std::vector<MCPToolDefinition> list_tools() const;
  MCPToolCallResult call_tool(const std::string& name, Value arguments = Value::object({}));
  [[nodiscard]] std::vector<MCPResourceDefinition> list_resources() const;
  [[nodiscard]] MCPResourceReadResult read_resource(const std::string& uri) const;
  [[nodiscard]] std::vector<MCPPromptDefinition> list_prompts() const;
  [[nodiscard]] std::vector<AgentMessage> get_prompt(const std::string& name, Value arguments = Value::object({})) const;

 private:
  std::string name_;
  std::string version_;
  ToolRegistry tools_;
  mutable std::mutex mutex_;
  std::map<std::string, MCPResourceDefinition> resources_;
  std::map<std::string, MCPResourceReadResult> resource_contents_;
  std::map<std::string, MCPPromptDefinition> prompts_;
};

class InProcessMCPClient {
 public:
  explicit InProcessMCPClient(InProcessMCPServer& server, std::string name = "native-agent-client");
  Value connect();
  [[nodiscard]] std::vector<MCPToolDefinition> list_tools() const;
  MCPToolCallResult call_tool(const std::string& name, Value arguments = Value::object({}));
  [[nodiscard]] std::vector<MCPResourceDefinition> list_resources() const;
  [[nodiscard]] MCPResourceReadResult read_resource(const std::string& uri) const;
  [[nodiscard]] std::vector<MCPPromptDefinition> list_prompts() const;
  [[nodiscard]] std::vector<AgentMessage> get_prompt(const std::string& name, Value arguments = Value::object({})) const;

 private:
  InProcessMCPServer& server_;
  std::string name_;
  bool connected_ = false;
};

class MCPClient {
 public:
  MCPClient(std::string name, std::shared_ptr<MCPTransport> transport,
            std::string client_name = "native-agent-mcp",
            std::string client_version = "0.1.0",
            Value capabilities = Value::object({}),
            int request_timeout_ms = 30000);
  MCPClient(const MCPClient&) = delete;
  MCPClient& operator=(const MCPClient&) = delete;
  [[nodiscard]] const std::string& name() const noexcept;
  Value connect();
  void close();
  Value request(const std::string& method, Value params = {});
  void notify(const std::string& method, Value params = {});
  std::vector<MCPToolDefinition> list_tools();
  MCPToolCallResult call_tool(const std::string& name, Value arguments = Value::object({}));
  std::vector<MCPResourceDefinition> list_resources();
  MCPResourceReadResult read_resource(const std::string& uri);
  std::vector<MCPPromptDefinition> list_prompts();
  std::vector<AgentMessage> get_prompt(const std::string& name, Value arguments = Value::object({}));

 private:
  void handle_message(const Value& message);
  std::string next_id();

  std::string name_;
  std::shared_ptr<MCPTransport> transport_;
  std::string client_name_;
  std::string client_version_;
  Value capabilities_;
  int request_timeout_ms_ = 30000;
  int request_id_ = 0;
  bool connected_ = false;
  bool closed_ = false;
  Value initialize_result_;
  std::map<std::string, JsonRpcResponse> responses_;
  mutable std::mutex lifecycle_mutex_;
  mutable std::mutex request_mutex_;
  mutable std::mutex responses_mutex_;
  std::condition_variable responses_cv_;
};

std::vector<ToolDefinition> create_mcp_tool_definitions(MCPClient& client,
                                                        std::string prefix = {},
                                                        bool include_server_tag = true);
std::vector<ToolDefinition> create_mcp_tool_definitions(std::shared_ptr<MCPClient> client,
                                                        std::string prefix = {},
                                                        bool include_server_tag = true);
std::string read_mcp_resource_text(MCPClient& client, const std::string& uri);
AgentMessage create_mcp_resource_message(MCPClient& client, const std::string& uri);
std::vector<AgentMessage> create_mcp_prompt_messages(MCPClient& client, const std::string& name,
                                                     Value arguments = Value::object({}));
std::string render_mcp_tool_summary(const std::vector<ToolDefinition>& tools);
std::string render_mcp_structured_value(const Value& value);

}  // namespace agent
