#include "agent/agent.hpp"

#include <algorithm>

namespace agent {

namespace {

ToolClientMessage make_message(ToolClientMessageType type, const std::string& client_id) {
  ToolClientMessage message;
  message.type = type;
  message.client_id = client_id;
  message.message_id = generate_uuid();
  message.timestamp = now_iso8601();
  return message;
}

bool wildcard_match(const std::string& value, const std::string& pattern) {
  if (value == pattern) {
    return true;
  }
  std::size_t value_index = 0;
  std::size_t pattern_index = 0;
  std::size_t star_index = std::string::npos;
  std::size_t match_index = 0;

  while (value_index < value.size()) {
    if (pattern_index < pattern.size() && pattern[pattern_index] == value[value_index]) {
      ++pattern_index;
      ++value_index;
      continue;
    }
    if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
      star_index = pattern_index++;
      match_index = value_index;
      continue;
    }
    if (star_index != std::string::npos) {
      pattern_index = star_index + 1;
      value_index = ++match_index;
      continue;
    }
    return false;
  }

  while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
    ++pattern_index;
  }
  return pattern_index == pattern.size();
}

bool matches_any_pattern(const std::string& value, const std::vector<std::string>& patterns) {
  return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
    return wildcard_match(value, pattern);
  });
}

void assert_security_allowed(const std::vector<std::string>& allowed,
                             const std::vector<std::string>& denied,
                             const std::string& value,
                             const std::string& label) {
  if (matches_any_pattern(value, denied)) {
    throw AgentFrameworkError(label + " \"" + value + "\" is denied by tool-client security policy.");
  }
  if (!allowed.empty() && !matches_any_pattern(value, allowed)) {
    throw AgentFrameworkError(label + " \"" + value + "\" is not allowed by tool-client security policy.");
  }
}

void assert_capabilities_allowed(const std::vector<std::string>& capabilities,
                                 const ToolClientSecurityPolicy& security) {
  for (const auto& capability : capabilities) {
    assert_security_allowed(security.allowed_capabilities, security.denied_capabilities, capability, "Capability");
  }
}

void assert_serialized_size(const Value& value, std::size_t max_bytes, const std::string& label) {
  if (max_bytes == 0) {
    return;
  }
  const auto serialized = safe_json_stringify(value);
  if (serialized.size() > max_bytes) {
    throw AgentFrameworkError(label + " exceeds " + std::to_string(max_bytes) + " bytes.");
  }
}

Value tool_result_value(const ToolExecutionResult& result) {
  if (!result.result) {
    return Value();
  }
  if (std::holds_alternative<Value>(*result.result)) {
    return std::get<Value>(*result.result);
  }
  const auto& envelope = std::get<ToolResultEnvelope>(*result.result);
  if (envelope.value) {
    return *envelope.value;
  }
  return Value::object({{"output", result.output}});
}

Value string_array_to_value(const std::vector<std::string>& values) {
  Value::Array output;
  for (const auto& value : values) {
    output.emplace_back(value);
  }
  return Value(output);
}

}  // namespace

std::string to_string(ToolClientMessageType type) {
  switch (type) {
    case ToolClientMessageType::Register:
      return "register";
    case ToolClientMessageType::Registered:
      return "registered";
    case ToolClientMessageType::ToolCall:
      return "tool-call";
    case ToolClientMessageType::ToolResult:
      return "tool-result";
    case ToolClientMessageType::ToolError:
      return "tool-error";
    case ToolClientMessageType::Cancel:
      return "cancel";
    case ToolClientMessageType::Heartbeat:
      return "heartbeat";
    case ToolClientMessageType::Disconnect:
      return "disconnect";
  }
  return "heartbeat";
}

ToolClientRegistry::ToolClientRegistry(const ToolClientRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  clients_ = other.clients_;
}

ToolClientRegistry& ToolClientRegistry::operator=(const ToolClientRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  clients_ = other.clients_;
  return *this;
}

ToolClientRegistry::ToolClientRegistry(ToolClientRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  clients_ = std::move(other.clients_);
}

ToolClientRegistry& ToolClientRegistry::operator=(ToolClientRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  clients_ = std::move(other.clients_);
  return *this;
}

ToolClientInfo ToolClientRegistry::register_client(std::string client_id,
                                                   std::vector<ChatToolDescriptor> tools,
                                                   std::vector<std::string> capabilities) {
  ToolClientInfo info{std::move(client_id), std::move(tools), std::move(capabilities), now_iso8601()};
  const auto id = info.client_id;
  std::lock_guard<std::mutex> lock(mutex_);
  clients_[id] = info;
  return info;
}

std::optional<ToolClientInfo> ToolClientRegistry::touch(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto found = clients_.find(client_id);
  if (found == clients_.end()) {
    return std::nullopt;
  }
  found->second.last_seen_at = now_iso8601();
  return found->second;
}

bool ToolClientRegistry::unregister_client(const std::string& client_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  return clients_.erase(client_id) > 0;
}

std::optional<ToolClientInfo> ToolClientRegistry::get(const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = clients_.find(client_id);
  return found == clients_.end() ? std::nullopt : std::optional<ToolClientInfo>(found->second);
}

std::vector<ToolClientInfo> ToolClientRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolClientInfo> clients;
  for (const auto& [_, client] : clients_) {
    clients.push_back(client);
  }
  return clients;
}

SocketIoToolClientServerTransport::SocketIoToolClientServerTransport(
    std::shared_ptr<SocketIoToolClientServer> server,
    SocketIoToolClientServerTransportOptions options)
    : server_(std::move(server)),
      event_name_(options.event_name.empty() ? DEFAULT_TOOL_CLIENT_SOCKET_IO_EVENT
                                             : std::move(options.event_name)),
      on_handler_error_(std::move(options.on_handler_error)) {
  if (!server_) {
    throw ConfigurationError("SocketIoToolClientServerTransport requires a server.");
  }
  connection_subscription_id_ = server_->on_connection(
      [this](std::shared_ptr<SocketIoToolClientSocket> socket) {
        handle_connection(std::move(socket));
      });
}

SocketIoToolClientServerTransport::~SocketIoToolClientServerTransport() {
  stop();
}

void SocketIoToolClientServerTransport::send(const ToolClientMessage& message) {
  std::shared_ptr<SocketIoToolClientSocket> socket;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = sockets_by_client_id_.find(message.client_id);
    if (found == sockets_by_client_id_.end()) {
      throw AgentFrameworkError("Tool client \"" + message.client_id + "\" is not connected.");
    }
    socket = found->second.lock();
    if (!socket) {
      sockets_by_client_id_.erase(found);
      throw AgentFrameworkError("Tool client \"" + message.client_id + "\" is not connected.");
    }
  }
  socket->emit(event_name_, message);
}

std::string SocketIoToolClientServerTransport::subscribe(ToolClientMessageHandler handler) {
  const auto id = generate_uuid();
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[id] = std::move(handler);
  return id;
}

void SocketIoToolClientServerTransport::unsubscribe(const std::string& subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.erase(subscription_id);
}

void SocketIoToolClientServerTransport::stop() {
  std::string connection_subscription_id;
  std::vector<SocketCleanup> socket_cleanups;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    connection_subscription_id = std::move(connection_subscription_id_);
    for (auto& [_, cleanup] : socket_cleanups_) {
      socket_cleanups.push_back(std::move(cleanup));
    }
    handlers_.clear();
    sockets_by_client_id_.clear();
    client_ids_by_socket_.clear();
    socket_cleanups_.clear();
  }
  if (!connection_subscription_id.empty() && server_) {
    server_->off_connection(connection_subscription_id);
  }
  for (auto& cleanup : socket_cleanups) {
    if (cleanup.socket) {
      if (!cleanup.message_subscription_id.empty()) {
        cleanup.socket->off(cleanup.message_subscription_id);
      }
      if (!cleanup.disconnect_subscription_id.empty()) {
        cleanup.socket->off(cleanup.disconnect_subscription_id);
      }
    }
  }
}

std::shared_ptr<SocketIoToolClientSocket> SocketIoToolClientServerTransport::get_socket(
    const std::string& client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = sockets_by_client_id_.find(client_id);
  return found == sockets_by_client_id_.end() ? nullptr : found->second.lock();
}

void SocketIoToolClientServerTransport::handle_connection(std::shared_ptr<SocketIoToolClientSocket> socket) {
  if (!socket) {
    return;
  }
  auto* key = socket.get();
  SocketCleanup cleanup;
  cleanup.socket = socket;
  cleanup.message_subscription_id = socket->on_message(
      event_name_,
      [this, socket](const ToolClientMessage& message) {
        record_message_socket(message, socket);
        publish(message);
      });
  cleanup.disconnect_subscription_id = socket->on_disconnect(
      [this, socket](const std::string& reason) {
        auto* socket_key = socket.get();
        std::vector<std::string> client_ids;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          const auto found = client_ids_by_socket_.find(socket_key);
          if (found != client_ids_by_socket_.end()) {
            client_ids.assign(found->second.begin(), found->second.end());
          }
          for (const auto& client_id : client_ids) {
            const auto current = sockets_by_client_id_.find(client_id);
            if (current != sockets_by_client_id_.end()) {
              auto current_socket = current->second.lock();
              if (current_socket && current_socket.get() == socket_key) {
                sockets_by_client_id_.erase(current);
              }
            }
          }
          client_ids_by_socket_.erase(socket_key);
          socket_cleanups_.erase(socket_key);
        }
        for (const auto& client_id : client_ids) {
          auto message = make_message(ToolClientMessageType::Disconnect, client_id);
          message.reason = "Socket.IO client disconnected: " + reason;
          publish(message);
        }
      });
  bool should_unregister = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      should_unregister = true;
    } else {
      socket_cleanups_[key] = cleanup;
    }
  }
  if (should_unregister) {
    if (!cleanup.message_subscription_id.empty()) {
      socket->off(cleanup.message_subscription_id);
    }
    if (!cleanup.disconnect_subscription_id.empty()) {
      socket->off(cleanup.disconnect_subscription_id);
    }
  }
}

void SocketIoToolClientServerTransport::record_message_socket(
    const ToolClientMessage& message,
    const std::shared_ptr<SocketIoToolClientSocket>& socket) {
  switch (message.type) {
    case ToolClientMessageType::Register:
    case ToolClientMessageType::Heartbeat:
    case ToolClientMessageType::ToolResult:
    case ToolClientMessageType::ToolError:
      bind_client(message.client_id, socket);
      return;
    case ToolClientMessageType::Disconnect:
      unbind_client(message.client_id, socket);
      return;
    default:
      return;
  }
}

void SocketIoToolClientServerTransport::bind_client(
    const std::string& client_id,
    const std::shared_ptr<SocketIoToolClientSocket>& socket) {
  if (!socket) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = sockets_by_client_id_.find(client_id);
  if (current != sockets_by_client_id_.end()) {
    if (auto current_socket = current->second.lock()) {
      if (current_socket.get() != socket.get()) {
        auto current_ids = client_ids_by_socket_.find(current_socket.get());
        if (current_ids != client_ids_by_socket_.end()) {
          current_ids->second.erase(client_id);
          if (current_ids->second.empty()) {
            client_ids_by_socket_.erase(current_ids);
          }
        }
      }
    }
  }
  sockets_by_client_id_[client_id] = socket;
  client_ids_by_socket_[socket.get()].insert(client_id);
}

void SocketIoToolClientServerTransport::unbind_client(
    const std::string& client_id,
    const std::shared_ptr<SocketIoToolClientSocket>& socket) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto current = sockets_by_client_id_.find(client_id);
  if (current == sockets_by_client_id_.end()) {
    return;
  }
  auto current_socket = current->second.lock();
  if (!current_socket || (socket && current_socket.get() != socket.get())) {
    return;
  }
  sockets_by_client_id_.erase(current);
  const auto ids = client_ids_by_socket_.find(current_socket.get());
  if (ids != client_ids_by_socket_.end()) {
    ids->second.erase(client_id);
    if (ids->second.empty()) {
      client_ids_by_socket_.erase(ids);
    }
  }
}

void SocketIoToolClientServerTransport::publish(const ToolClientMessage& message) {
  std::map<std::string, ToolClientMessageHandler> handlers;
  std::function<void(const std::exception&, const ToolClientMessage&)> on_handler_error;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers = handlers_;
    on_handler_error = on_handler_error_;
  }
  for (const auto& [_, handler] : handlers) {
    try {
      handler(message);
    } catch (const std::exception& error) {
      if (on_handler_error) {
        on_handler_error(error, message);
      }
    }
  }
}

SocketIoToolClientClientTransport::SocketIoToolClientClientTransport(
    std::shared_ptr<SocketIoToolClientSocket> socket,
    SocketIoToolClientTransportOptions options)
    : socket_(std::move(socket)),
      event_name_(options.event_name.empty() ? DEFAULT_TOOL_CLIENT_SOCKET_IO_EVENT
                                             : std::move(options.event_name)) {
  if (!socket_) {
    throw ConfigurationError("SocketIoToolClientClientTransport requires a socket.");
  }
}

void SocketIoToolClientClientTransport::send(const ToolClientMessage& message) {
  socket_->emit(event_name_, message);
}

std::string SocketIoToolClientClientTransport::subscribe(ToolClientMessageHandler handler) {
  const auto id = generate_uuid();
  const auto socket_subscription_id = socket_->on_message(event_name_, std::move(handler));
  std::lock_guard<std::mutex> lock(mutex_);
  socket_subscription_ids_[id] = socket_subscription_id;
  return id;
}

void SocketIoToolClientClientTransport::unsubscribe(const std::string& subscription_id) {
  std::string socket_subscription_id;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = socket_subscription_ids_.find(subscription_id);
    if (found == socket_subscription_ids_.end()) {
      return;
    }
    socket_subscription_id = found->second;
    socket_subscription_ids_.erase(found);
  }
  socket_->off(socket_subscription_id);
}

std::optional<ToolClientInfo> ToolClientRegistry::find_client_for_tool(
    const std::string& tool_name,
    const std::string& preferred_client_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!preferred_client_id.empty()) {
    const auto preferred = clients_.find(preferred_client_id);
    if (preferred == clients_.end()) {
      return std::nullopt;
    }
    const bool provides_tool = std::any_of(preferred->second.tools.begin(), preferred->second.tools.end(),
                                           [&](const ChatToolDescriptor& tool) {
                                             return tool.name == tool_name;
                                           });
    return provides_tool ? std::optional<ToolClientInfo>(preferred->second) : std::nullopt;
  }
  for (const auto& [_, client] : clients_) {
    const bool provides_tool = std::any_of(client.tools.begin(), client.tools.end(),
                                           [&](const ChatToolDescriptor& tool) {
                                             return tool.name == tool_name;
                                           });
    if (provides_tool) {
      return client;
    }
  }
  return std::nullopt;
}

ToolClientBroker::ToolClientBroker(std::shared_ptr<ToolClientTransport> transport,
                                   ToolClientRegistry registry,
                                   ToolClientSecurityPolicy security,
                                   EventBus* event_bus,
                                   int default_timeout_ms)
    : transport_(std::move(transport)),
      registry_(std::move(registry)),
      security_(std::move(security)),
      event_bus_(event_bus),
      default_timeout_ms_(default_timeout_ms) {
  if (!transport_) {
    throw ConfigurationError("ToolClientBroker requires a transport.");
  }
  subscription_id_ = transport_->subscribe([this](const ToolClientMessage& message) {
    handle_message(message);
  });
}

ToolClientBroker::~ToolClientBroker() {
  stop();
}

ToolClientRegistry& ToolClientBroker::registry() noexcept {
  return registry_;
}

const ToolClientRegistry& ToolClientBroker::registry() const noexcept {
  return registry_;
}

std::size_t ToolClientBroker::pending_count() const noexcept {
  std::lock_guard<std::mutex> lock(pending_mutex_);
  return pending_.size();
}

void ToolClientBroker::stop() {
  if (!subscription_id_.empty() && transport_) {
    transport_->unsubscribe(subscription_id_);
    subscription_id_.clear();
  }
  std::vector<std::pair<std::string, std::shared_ptr<PendingCall>>> pending_calls;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto& [request_id, pending] : pending_) {
      pending->completed = true;
      pending->ok = false;
      pending->error = "Tool call \"" + pending->tool_call.name + "\" was cancelled.";
      pending_calls.push_back({request_id, pending});
    }
    pending_.clear();
    pending_by_idempotency_key_.clear();
  }
  pending_changed_.notify_all();
  for (const auto& [request_id, pending] : pending_calls) {
    auto cancel = make_message(ToolClientMessageType::Cancel, pending->client_id);
    cancel.request_id = request_id;
    cancel.reason = "Tool client broker stopped.";
    try {
      transport_->send(cancel);
    } catch (...) {
    }
  }
}

Value ToolClientBroker::call_tool(ToolClientBrokerCallOptions options) {
  assert_serialized_size(options.tool_call.arguments, security_.max_serialized_arguments_bytes,
                         "Tool call \"" + options.tool_call.name + "\" arguments");
  assert_security_allowed(security_.allowed_tools, security_.denied_tools, options.tool_call.name, "Tool");

  if (!options.idempotency_key.empty()) {
    std::shared_ptr<PendingCall> pending_same_key;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      const auto completed = completed_idempotent_results_.find(options.idempotency_key);
      if (completed != completed_idempotent_results_.end()) {
        return completed->second;
      }
      const auto failed = completed_idempotent_errors_.find(options.idempotency_key);
      if (failed != completed_idempotent_errors_.end()) {
        throw AgentFrameworkError(failed->second);
      }
      const auto pending = pending_by_idempotency_key_.find(options.idempotency_key);
      if (pending != pending_by_idempotency_key_.end()) {
        pending_same_key = pending->second;
      }
    }
    if (pending_same_key) {
      std::unique_lock<std::mutex> lock(pending_mutex_);
      pending_changed_.wait(lock, [&pending_same_key]() {
        return pending_same_key->completed;
      });
      if (pending_same_key->ok) {
        return pending_same_key->result;
      }
      throw AgentFrameworkError(pending_same_key->error.empty() ? "Tool client call failed."
                                                                : pending_same_key->error);
    }
  }

  const auto client = registry_.find_client_for_tool(options.tool_call.name, options.preferred_client_id);
  if (!client) {
    throw AgentFrameworkError(options.preferred_client_id.empty()
                                  ? "No online client provides tool \"" + options.tool_call.name + "\"."
                                  : "Preferred client \"" + options.preferred_client_id +
                                        "\" is not online or does not provide tool \"" + options.tool_call.name + "\".");
  }
  assert_security_allowed(security_.allowed_client_ids, security_.denied_client_ids, client->client_id, "Client");
  assert_capabilities_allowed(client->capabilities, security_);

  auto message = make_message(ToolClientMessageType::ToolCall, client->client_id);
  message.tool_call = options.tool_call;
  message.idempotency_key = options.idempotency_key;
  message.trace_context = options.trace_context;
  const auto request_id = message.message_id;
  auto pending_record = std::make_shared<PendingCall>();
  pending_record->client_id = client->client_id;
  pending_record->tool_call = options.tool_call;
  pending_record->idempotency_key = options.idempotency_key;
  pending_record->trace_context = options.trace_context;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_[request_id] = pending_record;
    if (!options.idempotency_key.empty()) {
      pending_by_idempotency_key_[options.idempotency_key] = pending_record;
    }
  }
  publish_audit("call.started",
                Value::object({{"clientId", client->client_id},
                               {"requestId", request_id},
                               {"toolName", options.tool_call.name},
                               {"toolCallId", options.tool_call.id},
                               {"idempotencyKey", options.idempotency_key.empty() ? Value() : Value(options.idempotency_key)}}),
                options.trace_context);
  try {
    transport_->send(message);
  } catch (...) {
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      pending_.erase(request_id);
      if (!pending_record->idempotency_key.empty()) {
        pending_by_idempotency_key_.erase(pending_record->idempotency_key);
      }
      pending_record->completed = true;
      pending_record->ok = false;
      pending_record->error = "Tool client transport send failed.";
    }
    pending_changed_.notify_all();
    throw;
  }

  const int effective_timeout_ms = options.timeout_ms > 0 ? options.timeout_ms : default_timeout_ms_;
  std::unique_lock<std::mutex> lock(pending_mutex_);
  const auto completed = [&pending_record]() {
    return pending_record->completed;
  };
  bool ready = true;
  if (effective_timeout_ms > 0) {
    ready = pending_changed_.wait_for(lock, std::chrono::milliseconds(effective_timeout_ms), completed);
  } else {
    pending_changed_.wait(lock, completed);
  }
  if (!ready) {
    const auto timeout_error = "Tool call \"" + options.tool_call.name + "\" timed out after " +
                               std::to_string(effective_timeout_ms) + "ms.";
    const auto pending = pending_.find(request_id);
    if (pending != pending_.end() && pending->second == pending_record) {
      pending_.erase(pending);
    }
    if (!pending_record->idempotency_key.empty()) {
      pending_by_idempotency_key_.erase(pending_record->idempotency_key);
      completed_idempotent_errors_[pending_record->idempotency_key] = timeout_error;
    }
    pending_record->completed = true;
    pending_record->ok = false;
    pending_record->error = timeout_error;
    lock.unlock();
    pending_changed_.notify_all();
    auto cancel = make_message(ToolClientMessageType::Cancel, pending_record->client_id);
    cancel.request_id = request_id;
    cancel.reason = "Tool call timed out after " + std::to_string(effective_timeout_ms) + "ms.";
    try {
      transport_->send(cancel);
    } catch (...) {
    }
    publish_audit("call.failed",
                  Value::object({{"clientId", pending_record->client_id},
                                 {"requestId", request_id},
                                 {"toolName", pending_record->tool_call.name},
                                 {"toolCallId", pending_record->tool_call.id},
                                 {"idempotencyKey", pending_record->idempotency_key.empty()
                                                        ? Value()
                                                        : Value(pending_record->idempotency_key)},
                                 {"error", timeout_error}}),
                  pending_record->trace_context);
    throw AgentFrameworkError(timeout_error);
  }
  if (pending_record->ok) {
    return pending_record->result;
  }
  throw AgentFrameworkError(pending_record->error.empty() ? "Tool client call failed." : pending_record->error);
}

void ToolClientBroker::handle_message(const ToolClientMessage& message) {
  if (message.type == ToolClientMessageType::Register) {
    assert_registration_allowed(message);
    auto client = registry_.register_client(message.client_id, message.tools, message.capabilities);
    auto registered = make_message(ToolClientMessageType::Registered, message.client_id);
    registered.tools = client.tools;
    registered.capabilities = client.capabilities;
    transport_->send(registered);
    publish_audit("client.registered", Value::object({{"clientId", message.client_id},
                                                       {"toolCount", client.tools.size()},
                                                       {"capabilities", string_array_to_value(client.capabilities)}}));
    return;
  }
  if (message.type == ToolClientMessageType::Heartbeat) {
    (void)registry_.touch(message.client_id);
    return;
  }
  if (message.type == ToolClientMessageType::Disconnect) {
    registry_.unregister_client(message.client_id);
    reject_pending_for_client(message.client_id, message.reason.empty()
                                                     ? "Tool client \"" + message.client_id + "\" disconnected."
                                                     : message.reason);
    publish_audit("client.disconnected", Value::object({{"clientId", message.client_id},
                                                        {"reason", message.reason.empty() ? Value() : Value(message.reason)}}));
    return;
  }
  if (message.type == ToolClientMessageType::ToolResult) {
    assert_serialized_size(message.result, security_.max_serialized_result_bytes,
                           "Tool call \"" + message.tool_call_id + "\" result");
    resolve_pending(message.request_id, message.result);
    return;
  }
  if (message.type == ToolClientMessageType::ToolError) {
    reject_pending(message.request_id, message.error);
    return;
  }
  if (message.type == ToolClientMessageType::Cancel) {
    reject_pending(message.request_id, message.reason.empty() ? "Tool call was cancelled." : message.reason);
  }
}

void ToolClientBroker::assert_registration_allowed(const ToolClientMessage& message) const {
  assert_security_allowed(security_.allowed_client_ids, security_.denied_client_ids, message.client_id, "Client");
  if (security_.max_registered_tools > 0 && message.tools.size() > security_.max_registered_tools) {
    throw AgentFrameworkError("Client \"" + message.client_id + "\" registered too many tools.");
  }
  assert_capabilities_allowed(message.capabilities, security_);
  for (const auto& tool : message.tools) {
    assert_security_allowed(security_.allowed_tools, security_.denied_tools, tool.name, "Tool");
  }
}

void ToolClientBroker::resolve_pending(const std::string& request_id, Value result) {
  std::shared_ptr<PendingCall> pending;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto found = pending_.find(request_id);
    if (found == pending_.end()) {
      return;
    }
    pending = found->second;
    pending_.erase(found);
    if (!pending->idempotency_key.empty()) {
      pending_by_idempotency_key_.erase(pending->idempotency_key);
      completed_idempotent_results_[pending->idempotency_key] = result;
    }
    pending->completed = true;
    pending->ok = true;
    pending->result = std::move(result);
  }
  pending_changed_.notify_all();
  publish_audit("call.completed",
                Value::object({{"clientId", pending->client_id},
                               {"requestId", request_id},
                               {"toolName", pending->tool_call.name},
                               {"toolCallId", pending->tool_call.id},
                               {"idempotencyKey", pending->idempotency_key.empty()
                                                      ? Value()
                                                      : Value(pending->idempotency_key)}}),
                pending->trace_context);
}

void ToolClientBroker::reject_pending(const std::string& request_id, std::string error) {
  std::shared_ptr<PendingCall> pending;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto found = pending_.find(request_id);
    if (found == pending_.end()) {
      return;
    }
    pending = found->second;
    pending_.erase(found);
    if (!pending->idempotency_key.empty()) {
      pending_by_idempotency_key_.erase(pending->idempotency_key);
      completed_idempotent_errors_[pending->idempotency_key] = error;
    }
    pending->completed = true;
    pending->ok = false;
    pending->error = std::move(error);
  }
  pending_changed_.notify_all();
  publish_audit("call.failed",
                Value::object({{"clientId", pending->client_id},
                               {"requestId", request_id},
                               {"toolName", pending->tool_call.name},
                               {"toolCallId", pending->tool_call.id},
                               {"error", pending->error}}),
                pending->trace_context);
}

void ToolClientBroker::reject_pending_for_client(const std::string& client_id, std::string error) {
  std::vector<std::pair<std::string, std::shared_ptr<PendingCall>>> rejected;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    for (auto it = pending_.begin(); it != pending_.end();) {
      auto& pending = it->second;
      if (pending->client_id != client_id) {
        ++it;
        continue;
      }
      const auto request_id = it->first;
      pending->completed = true;
      pending->ok = false;
      pending->error = error;
      if (!pending->idempotency_key.empty()) {
        pending_by_idempotency_key_.erase(pending->idempotency_key);
        completed_idempotent_errors_[pending->idempotency_key] = error;
      }
      rejected.push_back({request_id, pending});
      it = pending_.erase(it);
    }
  }
  pending_changed_.notify_all();
  for (const auto& [request_id, pending] : rejected) {
    publish_audit("call.failed",
                  Value::object({{"clientId", pending->client_id},
                                 {"requestId", request_id},
                                 {"toolName", pending->tool_call.name},
                                 {"toolCallId", pending->tool_call.id},
                                 {"idempotencyKey", pending->idempotency_key.empty()
                                                        ? Value()
                                                        : Value(pending->idempotency_key)},
                                 {"error", pending->error}}),
                  pending->trace_context);
  }
}

void ToolClientBroker::publish_audit(std::string lifecycle, Value payload, TraceContext trace) {
  if (!event_bus_) {
    return;
  }
  payload["lifecycle"] = std::move(lifecycle);
  event_bus_->publish("tool-client.audit", ExecutionTarget::Tool, std::move(payload), std::move(trace));
}

ToolClientRuntime::ToolClientRuntime(ToolClientRuntimeOptions options)
    : client_id_(std::move(options.client_id)),
      tools_(std::move(options.tools)),
      transport_(std::move(options.transport)),
      capabilities_(std::move(options.capabilities)),
      security_(std::move(options.security)),
      services_(std::move(options.services)),
      event_bus_(options.event_bus),
      executor_(tools_, std::move(options.permission_policy), std::move(options.approval_handler),
                options.event_bus, std::move(options.execution_policies)),
      max_idempotency_cache_size_(options.max_idempotency_cache_size) {
  if (client_id_.empty()) {
    throw ConfigurationError("ToolClientRuntime requires clientId.");
  }
  if (!transport_) {
    throw ConfigurationError("ToolClientRuntime requires a transport.");
  }
}

ToolClientRuntime::~ToolClientRuntime() {
  stop();
}

void ToolClientRuntime::start() {
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (started_) {
      return;
    }
  }
  assert_registration_allowed();
  auto subscription_id = transport_->subscribe([this](const ToolClientMessage& message) {
    handle_message(message);
  });
  bool unsubscribe_duplicate = false;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (started_) {
      unsubscribe_duplicate = true;
    } else {
      started_ = true;
      subscription_id_ = std::move(subscription_id);
    }
  }
  if (unsubscribe_duplicate) {
    transport_->unsubscribe(subscription_id);
    return;
  }
  auto message = make_message(ToolClientMessageType::Register, client_id_);
  message.tools = registered_tool_descriptors();
  message.capabilities = capabilities_;
  transport_->send(message);
  publish_audit("client.registered", Value::object({{"clientId", client_id_},
                                                    {"toolCount", message.tools.size()},
                                                    {"capabilities", string_array_to_value(capabilities_)}}));
}

void ToolClientRuntime::stop(std::string reason) {
  std::string subscription_id;
  {
    std::lock_guard<std::mutex> lock(lifecycle_mutex_);
    if (!started_) {
      return;
    }
    started_ = false;
    subscription_id = std::move(subscription_id_);
  }
  cancel_active_calls(reason.empty() ? "Tool client runtime stopped." : reason);
  if (!subscription_id.empty()) {
    transport_->unsubscribe(subscription_id);
  }
  auto message = make_message(ToolClientMessageType::Disconnect, client_id_);
  message.reason = std::move(reason);
  transport_->send(message);
}

void ToolClientRuntime::heartbeat() {
  auto message = make_message(ToolClientMessageType::Heartbeat, client_id_);
  transport_->send(message);
}

bool ToolClientRuntime::started() const noexcept {
  std::lock_guard<std::mutex> lock(lifecycle_mutex_);
  return started_;
}

const std::string& ToolClientRuntime::client_id() const noexcept {
  return client_id_;
}

void ToolClientRuntime::handle_message(const ToolClientMessage& message) {
  if (message.client_id != client_id_) {
    return;
  }
  if (message.type == ToolClientMessageType::Disconnect) {
    std::string subscription_id;
    {
      std::lock_guard<std::mutex> lock(lifecycle_mutex_);
      started_ = false;
      subscription_id = std::move(subscription_id_);
    }
    cancel_active_calls(message.reason.empty() ? "Tool client runtime disconnected." : message.reason);
    if (!subscription_id.empty()) {
      transport_->unsubscribe(subscription_id);
    }
    publish_audit("client.disconnected", Value::object({{"clientId", client_id_},
                                                        {"reason", message.reason.empty() ? Value() : Value(message.reason)}}));
    return;
  }
  if (message.type == ToolClientMessageType::Cancel) {
    (void)cancel_active_call(message.request_id,
                             message.reason.empty() ? "Tool client call was cancelled." : message.reason,
                             message.trace_context);
    return;
  }
  if (message.type != ToolClientMessageType::ToolCall) {
    return;
  }

  try {
    assert_tool_call_allowed(message.tool_call);
  } catch (const std::exception& error) {
    auto reply = make_message(ToolClientMessageType::ToolError, client_id_);
    reply.request_id = message.message_id;
    reply.tool_call_id = message.tool_call.id;
    reply.idempotency_key = message.idempotency_key;
    reply.trace_context = message.trace_context;
    reply.error = error.what();
    transport_->send(reply);
    return;
  }

  const auto idempotency_key = message.idempotency_key.empty() ? message.tool_call.id : message.idempotency_key;
  std::optional<CachedResult> cached_result;
  {
    std::lock_guard<std::mutex> lock(completed_mutex_);
    const auto cached = completed_.find(idempotency_key);
    if (cached != completed_.end()) {
      cached_result = cached->second;
    }
  }
  CachedResult result = cached_result ? *cached_result : CachedResult{};
  if (!cached_result) {
    CancellationToken cancellation;
    register_active_call(message.message_id, cancellation);
    try {
      result = execute_tool(message.tool_call, message.trace_context, &cancellation);
    } catch (const std::exception& error) {
      result = CachedResult{false, Value(), {}, error.what()};
    } catch (...) {
      result = CachedResult{false, Value(), {}, "Tool client call failed."};
    }
    unregister_active_call(message.message_id, cancellation);
    if (cancellation.cancelled()) {
      const auto reason = cancellation.reason();
      result = CachedResult{
          false,
          Value(),
          result.output,
          reason.empty() ? "Tool client call was cancelled." : reason,
      };
    } else {
      remember(idempotency_key, result);
    }
  }

  auto reply = make_message(result.ok ? ToolClientMessageType::ToolResult : ToolClientMessageType::ToolError, client_id_);
  reply.request_id = message.message_id;
  reply.tool_call_id = message.tool_call.id;
  reply.idempotency_key = message.idempotency_key;
  reply.trace_context = message.trace_context;
  reply.output = result.output;
  if (result.ok) {
    reply.result = result.result;
  } else {
    reply.error = result.error;
  }
  transport_->send(reply);
}

std::vector<ChatToolDescriptor> ToolClientRuntime::registered_tool_descriptors() const {
  std::vector<ChatToolDescriptor> descriptors;
  for (const auto& tool : tools_.list()) {
    if (tool_allowed_by_security(tool)) {
      descriptors.push_back(tool.descriptor());
    }
  }
  return descriptors;
}

void ToolClientRuntime::assert_registration_allowed() const {
  assert_security_allowed(security_.allowed_client_ids, security_.denied_client_ids, client_id_, "Client");
  assert_capabilities_allowed(capabilities_, security_);
  const auto tools = registered_tool_descriptors();
  if (security_.max_registered_tools > 0 && tools.size() > security_.max_registered_tools) {
    throw AgentFrameworkError("Client \"" + client_id_ + "\" registered too many tools.");
  }
}

void ToolClientRuntime::assert_tool_call_allowed(const ToolCall& tool_call) const {
  assert_serialized_size(tool_call.arguments, security_.max_serialized_arguments_bytes,
                         "Tool call \"" + tool_call.name + "\" arguments");
  const auto* tool = tools_.get(tool_call.name);
  if (!tool) {
    throw AgentFrameworkError("Unknown tool: " + tool_call.name);
  }
  assert_security_allowed(security_.allowed_tools, security_.denied_tools, tool_call.name, "Tool");
  assert_capabilities_allowed(tool->capabilities, security_);
}

bool ToolClientRuntime::tool_allowed_by_security(const ToolDefinition& tool) const {
  try {
    assert_security_allowed(security_.allowed_tools, security_.denied_tools, tool.name, "Tool");
    assert_capabilities_allowed(tool.capabilities, security_);
    return true;
  } catch (...) {
    return false;
  }
}

ToolClientRuntime::CachedResult ToolClientRuntime::execute_tool(const ToolCall& tool_call,
                                                                const TraceContext& trace_context,
                                                                CancellationToken* cancellation) {
  ToolExecutionContext context;
  context.services = services_;
  context.trace_context = trace_context;
  context.cancellation = cancellation;
  auto result = executor_.execute_tool_call(tool_call, std::move(context));
  if (result.ok) {
    return CachedResult{true, tool_result_value(result), result.output, {}};
  }
  return CachedResult{false, Value(), result.output, result.error.empty() ? result.output : result.error};
}

void ToolClientRuntime::register_active_call(const std::string& request_id, CancellationToken& cancellation) {
  if (request_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(active_mutex_);
  active_cancellations_[request_id] = &cancellation;
}

void ToolClientRuntime::unregister_active_call(const std::string& request_id, CancellationToken& cancellation) {
  if (request_id.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(active_mutex_);
  const auto found = active_cancellations_.find(request_id);
  if (found != active_cancellations_.end() && found->second == &cancellation) {
    active_cancellations_.erase(found);
  }
}

bool ToolClientRuntime::cancel_active_call(const std::string& request_id,
                                           const std::string& reason,
                                           TraceContext trace_context) {
  if (request_id.empty()) {
    return false;
  }
  const std::string resolved_reason = reason.empty() ? "Tool client call was cancelled." : reason;
  bool cancelled = false;
  {
    std::lock_guard<std::mutex> lock(active_mutex_);
    const auto found = active_cancellations_.find(request_id);
    if (found != active_cancellations_.end() && found->second) {
      found->second->cancel(resolved_reason);
      cancelled = true;
    }
  }
  if (cancelled) {
    publish_audit("call.cancelled",
                  Value::object({{"clientId", client_id_},
                                 {"requestId", request_id},
                                 {"reason", resolved_reason}}),
                  std::move(trace_context));
  }
  return cancelled;
}

void ToolClientRuntime::cancel_active_calls(const std::string& reason) {
  const std::string resolved_reason = reason.empty() ? "Tool client call was cancelled." : reason;
  std::vector<std::string> request_ids;
  {
    std::lock_guard<std::mutex> lock(active_mutex_);
    for (const auto& [request_id, token] : active_cancellations_) {
      if (token) {
        token->cancel(resolved_reason);
        request_ids.push_back(request_id);
      }
    }
  }
  for (const auto& request_id : request_ids) {
    publish_audit("call.cancelled",
                  Value::object({{"clientId", client_id_},
                                 {"requestId", request_id},
                                 {"reason", resolved_reason}}));
  }
}

void ToolClientRuntime::remember(std::string idempotency_key, CachedResult result) {
  if (max_idempotency_cache_size_ == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(completed_mutex_);
  completed_[std::move(idempotency_key)] = std::move(result);
  while (completed_.size() > max_idempotency_cache_size_) {
    completed_.erase(completed_.begin());
  }
}

void ToolClientRuntime::publish_audit(std::string lifecycle, Value payload, TraceContext trace) {
  if (!event_bus_) {
    return;
  }
  payload["lifecycle"] = std::move(lifecycle);
  event_bus_->publish("tool-client.audit", ExecutionTarget::Tool, std::move(payload), std::move(trace));
}

void InMemoryToolClientTransport::set_peer(InMemoryToolClientTransport* peer) {
  std::lock_guard<std::mutex> lock(mutex_);
  peer_ = peer;
}

void InMemoryToolClientTransport::send(const ToolClientMessage& message) {
  InMemoryToolClientTransport* peer = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    peer = peer_;
  }
  if (!peer) {
    return;
  }
  std::map<std::string, ToolClientMessageHandler> handlers;
  {
    std::lock_guard<std::mutex> lock(peer->mutex_);
    handlers = peer->handlers_;
  }
  for (const auto& [_, handler] : handlers) {
    handler(message);
  }
}

std::string InMemoryToolClientTransport::subscribe(ToolClientMessageHandler handler) {
  const auto id = generate_uuid();
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_[id] = std::move(handler);
  return id;
}

void InMemoryToolClientTransport::unsubscribe(const std::string& subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  handlers_.erase(subscription_id);
}

InMemoryToolClientTransportPair create_in_memory_tool_client_transport_pair() {
  auto server = std::make_shared<InMemoryToolClientTransport>();
  auto client = std::make_shared<InMemoryToolClientTransport>();
  server->set_peer(client.get());
  client->set_peer(server.get());
  return InMemoryToolClientTransportPair{server, client};
}

}  // namespace agent
