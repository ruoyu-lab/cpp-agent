#pragma once

#include "agent/tools.hpp"

#include <mutex>

namespace agent {

enum class ToolClientMessageType {
  Register,
  Registered,
  ToolCall,
  ToolResult,
  ToolError,
  Cancel,
  Heartbeat,
  Disconnect,
};

std::string to_string(ToolClientMessageType type);

struct ToolClientMessage {
  ToolClientMessageType type = ToolClientMessageType::Heartbeat;
  std::string message_id;
  std::string client_id;
  std::string timestamp;
  std::vector<ChatToolDescriptor> tools;
  std::vector<std::string> capabilities;
  ToolCall tool_call;
  std::string request_id;
  std::string tool_call_id;
  std::string idempotency_key;
  TraceContext trace_context;
  Value result;
  std::string output;
  std::string error;
  std::string reason;
};

using ToolClientMessageHandler = std::function<void(const ToolClientMessage&)>;

class ToolClientTransport {
 public:
  virtual ~ToolClientTransport() = default;
  virtual void send(const ToolClientMessage& message) = 0;
  virtual std::string subscribe(ToolClientMessageHandler handler) = 0;
  virtual void unsubscribe(const std::string& subscription_id) = 0;
};

inline constexpr const char* DEFAULT_TOOL_CLIENT_SOCKET_IO_EVENT = "node-agent:tool-client";

using SocketIoToolClientMessageListener = std::function<void(const ToolClientMessage&)>;
using SocketIoToolClientDisconnectListener = std::function<void(const std::string&)>;
using SocketIoToolClientConnectionListener =
    std::function<void(std::shared_ptr<class SocketIoToolClientSocket>)>;

class SocketIoToolClientSocket {
 public:
  virtual ~SocketIoToolClientSocket() = default;
  virtual void emit(std::string event_name, ToolClientMessage message) = 0;
  virtual std::string on_message(std::string event_name, SocketIoToolClientMessageListener listener) = 0;
  virtual std::string on_disconnect(SocketIoToolClientDisconnectListener listener) = 0;
  virtual void off(const std::string& subscription_id) = 0;
};

class SocketIoToolClientServer {
 public:
  virtual ~SocketIoToolClientServer() = default;
  virtual std::string on_connection(SocketIoToolClientConnectionListener listener) = 0;
  virtual void off_connection(const std::string& subscription_id) = 0;
};

struct SocketIoToolClientTransportOptions {
  std::string event_name = DEFAULT_TOOL_CLIENT_SOCKET_IO_EVENT;
};

struct SocketIoToolClientServerTransportOptions : SocketIoToolClientTransportOptions {
  std::function<void(const std::exception&, const ToolClientMessage&)> on_handler_error;
};

class SocketIoToolClientServerTransport : public ToolClientTransport {
 public:
  SocketIoToolClientServerTransport(std::shared_ptr<SocketIoToolClientServer> server,
                                    SocketIoToolClientServerTransportOptions options = {});
  ~SocketIoToolClientServerTransport() override;

  void send(const ToolClientMessage& message) override;
  std::string subscribe(ToolClientMessageHandler handler) override;
  void unsubscribe(const std::string& subscription_id) override;
  void stop();
  [[nodiscard]] std::shared_ptr<SocketIoToolClientSocket> get_socket(const std::string& client_id) const;

 private:
  struct SocketCleanup {
    std::shared_ptr<SocketIoToolClientSocket> socket;
    std::string message_subscription_id;
    std::string disconnect_subscription_id;
  };

  void handle_connection(std::shared_ptr<SocketIoToolClientSocket> socket);
  void record_message_socket(const ToolClientMessage& message,
                             const std::shared_ptr<SocketIoToolClientSocket>& socket);
  void bind_client(const std::string& client_id, const std::shared_ptr<SocketIoToolClientSocket>& socket);
  void unbind_client(const std::string& client_id, const std::shared_ptr<SocketIoToolClientSocket>& socket);
  void publish(const ToolClientMessage& message);

  std::shared_ptr<SocketIoToolClientServer> server_;
  std::string event_name_;
  std::function<void(const std::exception&, const ToolClientMessage&)> on_handler_error_;
  mutable std::mutex mutex_;
  std::string connection_subscription_id_;
  std::map<std::string, ToolClientMessageHandler> handlers_;
  std::map<std::string, std::weak_ptr<SocketIoToolClientSocket>> sockets_by_client_id_;
  std::map<SocketIoToolClientSocket*, std::set<std::string>> client_ids_by_socket_;
  std::map<SocketIoToolClientSocket*, SocketCleanup> socket_cleanups_;
  bool stopped_ = false;
};

class SocketIoToolClientClientTransport : public ToolClientTransport {
 public:
  SocketIoToolClientClientTransport(std::shared_ptr<SocketIoToolClientSocket> socket,
                                    SocketIoToolClientTransportOptions options = {});

  void send(const ToolClientMessage& message) override;
  std::string subscribe(ToolClientMessageHandler handler) override;
  void unsubscribe(const std::string& subscription_id) override;

 private:
  std::shared_ptr<SocketIoToolClientSocket> socket_;
  std::string event_name_;
  mutable std::mutex mutex_;
  std::map<std::string, std::string> socket_subscription_ids_;
};

struct ToolClientInfo {
  std::string client_id;
  std::vector<ChatToolDescriptor> tools;
  std::vector<std::string> capabilities;
  std::string last_seen_at;
};

struct ToolClientSecurityPolicy {
  std::vector<std::string> allowed_client_ids;
  std::vector<std::string> denied_client_ids;
  std::vector<std::string> allowed_tools;
  std::vector<std::string> denied_tools;
  std::vector<std::string> allowed_capabilities;
  std::vector<std::string> denied_capabilities;
  std::size_t max_registered_tools = 128;
  std::size_t max_serialized_arguments_bytes = 256 * 1024;
  std::size_t max_serialized_result_bytes = 1024 * 1024;
};

class ToolClientRegistry {
 public:
  ToolClientRegistry() = default;
  ToolClientRegistry(const ToolClientRegistry& other);
  ToolClientRegistry& operator=(const ToolClientRegistry& other);
  ToolClientRegistry(ToolClientRegistry&& other) noexcept;
  ToolClientRegistry& operator=(ToolClientRegistry&& other) noexcept;
  ToolClientInfo register_client(std::string client_id,
                                 std::vector<ChatToolDescriptor> tools,
                                 std::vector<std::string> capabilities = {});
  std::optional<ToolClientInfo> touch(const std::string& client_id);
  bool unregister_client(const std::string& client_id);
  [[nodiscard]] std::optional<ToolClientInfo> get(const std::string& client_id) const;
  [[nodiscard]] std::vector<ToolClientInfo> list() const;
  [[nodiscard]] std::optional<ToolClientInfo> find_client_for_tool(
      const std::string& tool_name,
      const std::string& preferred_client_id = {}) const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, ToolClientInfo> clients_;
};

struct ToolClientBrokerCallOptions {
  ToolCall tool_call;
  std::string preferred_client_id;
  std::string idempotency_key;
  TraceContext trace_context;
  int timeout_ms = 0;
};

class ToolClientBroker {
 public:
  ToolClientBroker(std::shared_ptr<ToolClientTransport> transport,
                   ToolClientRegistry registry = {},
                   ToolClientSecurityPolicy security = {},
                   EventBus* event_bus = nullptr,
                   int default_timeout_ms = 30000);
  ~ToolClientBroker();

  [[nodiscard]] ToolClientRegistry& registry() noexcept;
  [[nodiscard]] const ToolClientRegistry& registry() const noexcept;
  [[nodiscard]] std::size_t pending_count() const noexcept;
  void stop();
  Value call_tool(ToolClientBrokerCallOptions options);

 private:
  struct PendingCall {
    std::string client_id;
    ToolCall tool_call;
    std::string idempotency_key;
    TraceContext trace_context;
    bool completed = false;
    bool ok = false;
    Value result;
    std::string error;
  };

  void handle_message(const ToolClientMessage& message);
  void assert_registration_allowed(const ToolClientMessage& message) const;
  void resolve_pending(const std::string& request_id, Value result);
  void reject_pending(const std::string& request_id, std::string error);
  void reject_pending_for_client(const std::string& client_id, std::string error);
  void publish_audit(std::string lifecycle, Value payload, TraceContext trace = {});

  std::shared_ptr<ToolClientTransport> transport_;
  ToolClientRegistry registry_;
  ToolClientSecurityPolicy security_;
  EventBus* event_bus_ = nullptr;
  int default_timeout_ms_ = 30000;
  std::string subscription_id_;
  mutable std::mutex pending_mutex_;
  std::condition_variable pending_changed_;
  std::map<std::string, std::shared_ptr<PendingCall>> pending_;
  std::map<std::string, std::shared_ptr<PendingCall>> pending_by_idempotency_key_;
  std::map<std::string, Value> completed_idempotent_results_;
  std::map<std::string, std::string> completed_idempotent_errors_;
};

struct ToolClientRuntimeOptions {
  std::string client_id;
  std::vector<ToolDefinition> tools;
  std::shared_ptr<ToolClientTransport> transport;
  std::vector<std::string> capabilities;
  ToolClientSecurityPolicy security;
  Value services = Value::object({});
  PermissionPolicy permission_policy;
  PermissionApprovalHandler approval_handler;
  EventBus* event_bus = nullptr;
  ExecutionPolicies execution_policies;
  std::size_t max_idempotency_cache_size = 500;
};

class ToolClientRuntime {
 public:
  explicit ToolClientRuntime(ToolClientRuntimeOptions options);
  ~ToolClientRuntime();

  void start();
  void stop(std::string reason = {});
  void heartbeat();
  [[nodiscard]] bool started() const noexcept;
  [[nodiscard]] const std::string& client_id() const noexcept;

 private:
  struct CachedResult {
    bool ok = false;
    Value result;
    std::string output;
    std::string error;
  };

  void handle_message(const ToolClientMessage& message);
  std::vector<ChatToolDescriptor> registered_tool_descriptors() const;
  void assert_registration_allowed() const;
  void assert_tool_call_allowed(const ToolCall& tool_call) const;
  bool tool_allowed_by_security(const ToolDefinition& tool) const;
  CachedResult execute_tool(const ToolCall& tool_call, const TraceContext& trace_context,
                            CancellationToken* cancellation);
  void register_active_call(const std::string& request_id, CancellationToken& cancellation);
  void unregister_active_call(const std::string& request_id, CancellationToken& cancellation);
  bool cancel_active_call(const std::string& request_id, const std::string& reason,
                          TraceContext trace_context = {});
  void cancel_active_calls(const std::string& reason);
  void remember(std::string idempotency_key, CachedResult result);
  void publish_audit(std::string lifecycle, Value payload, TraceContext trace = {});

  std::string client_id_;
  ToolRegistry tools_;
  std::shared_ptr<ToolClientTransport> transport_;
  std::vector<std::string> capabilities_;
  ToolClientSecurityPolicy security_;
  Value services_;
  EventBus* event_bus_ = nullptr;
  ToolExecutor executor_;
  std::size_t max_idempotency_cache_size_ = 500;
  mutable std::mutex lifecycle_mutex_;
  bool started_ = false;
  std::string subscription_id_;
  mutable std::mutex completed_mutex_;
  std::map<std::string, CachedResult> completed_;
  mutable std::mutex active_mutex_;
  std::map<std::string, CancellationToken*> active_cancellations_;
};

class InMemoryToolClientTransport : public ToolClientTransport {
 public:
  void set_peer(InMemoryToolClientTransport* peer);
  void send(const ToolClientMessage& message) override;
  std::string subscribe(ToolClientMessageHandler handler) override;
  void unsubscribe(const std::string& subscription_id) override;

 private:
  mutable std::mutex mutex_;
  InMemoryToolClientTransport* peer_ = nullptr;
  std::map<std::string, ToolClientMessageHandler> handlers_;
};

struct InMemoryToolClientTransportPair {
  std::shared_ptr<InMemoryToolClientTransport> server;
  std::shared_ptr<InMemoryToolClientTransport> client;
};

InMemoryToolClientTransportPair create_in_memory_tool_client_transport_pair();

}  // namespace agent
