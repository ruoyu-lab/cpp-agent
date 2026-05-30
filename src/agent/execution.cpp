#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace agent {

std::string to_string(ExecutionTarget target) {
  switch (target) {
    case ExecutionTarget::Run:
      return "run";
    case ExecutionTarget::Model:
      return "model";
    case ExecutionTarget::Tool:
      return "tool";
    case ExecutionTarget::Retrieval:
      return "retrieval";
    case ExecutionTarget::Permission:
      return "permission";
    case ExecutionTarget::Workflow:
      return "workflow";
    case ExecutionTarget::WorkflowNode:
      return "workflow-node";
    case ExecutionTarget::ChildAgent:
      return "child-agent";
  }
  return "run";
}

void CancellationToken::cancel(std::string reason) {
  std::vector<Callback> callbacks;
  std::string resolved_reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const bool first_cancel = !cancelled_;
    cancelled_ = true;
    reason_ = std::move(reason);
    resolved_reason = reason_;
    if (first_cancel) {
      callbacks.reserve(callbacks_.size());
      for (const auto& [_, callback] : callbacks_) {
        callbacks.push_back(callback);
      }
    }
  }
  for (const auto& callback : callbacks) {
    if (callback) {
      callback(resolved_reason);
    }
  }
}

bool CancellationToken::cancelled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cancelled_;
}

std::string CancellationToken::reason() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return reason_;
}

void CancellationToken::throw_if_cancelled(ExecutionTarget target) const {
  std::string reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cancelled_) {
      return;
    }
    reason = reason_;
  }
  throw AgentFrameworkError("Execution aborted for " + to_string(target) + ": " + reason);
}

std::size_t CancellationToken::add_callback(Callback callback) {
  if (!callback) {
    return 0;
  }
  std::string cancelled_reason;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cancelled_) {
      const auto callback_id = next_callback_id_++;
      callbacks_[callback_id] = std::move(callback);
      return callback_id;
    }
    cancelled_reason = reason_;
  }
  callback(cancelled_reason);
  return 0;
}

void CancellationToken::remove_callback(std::size_t callback_id) {
  if (callback_id == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  callbacks_.erase(callback_id);
}

namespace {

TraceContext build_trace_context(TraceContext value, bool require_span_id) {
  if (value.trace_id.empty()) {
    value.trace_id = generate_uuid();
  }
  if (require_span_id && value.span_id.empty()) {
    value.span_id = generate_uuid();
  }
  return value;
}

bool policy_require_span_id(const TracePropagationPolicy& policy, bool fallback) {
  return policy.require_span_id.value_or(fallback);
}

std::string string_field_from_value(const Value& value, const std::string& key) {
  if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
    return {};
  }
  return value.at(key).as_string();
}

bool js_truthy_trace_context_value(const Value& value) {
  if (value.is_null()) {
    return false;
  }
  if (value.is_bool()) {
    return value.as_bool();
  }
  if (value.is_number()) {
    return value.as_number() != 0;
  }
  if (value.is_string()) {
    return !value.as_string().empty();
  }
  return true;
}

bool trace_context_has_values(const TraceContext& trace) {
  return !trace.trace_id.empty() || !trace.span_id.empty() || !trace.parent_span_id.empty() ||
         !trace.span_name.empty() || !trace.run_id.empty() || !trace.workflow_run_id.empty();
}

}  // namespace

TraceContext create_trace_context(TraceContext value) {
  return build_trace_context(std::move(value), true);
}

TraceContext derive_child_trace_context(const TraceContext* parent,
                                        TraceSpanDescriptor span,
                                        TraceContext overrides,
                                        TracePropagationPolicy policy) {
  const bool require_span_id = policy_require_span_id(policy, true);
  std::optional<TraceContext> normalized_parent;
  if (parent) {
    normalized_parent = build_trace_context(*parent, require_span_id);
  }

  TraceContext child;
  child.trace_id = overrides.trace_id.empty() && normalized_parent ? normalized_parent->trace_id : overrides.trace_id;
  child.span_id = std::move(overrides.span_id);
  child.parent_span_id = overrides.parent_span_id.empty() && normalized_parent
                             ? normalized_parent->span_id
                             : std::move(overrides.parent_span_id);
  child.span_name = overrides.span_name.empty() ? std::move(span.name) : std::move(overrides.span_name);
  child.run_id = overrides.run_id.empty() && policy.inherit_run_id && normalized_parent
                     ? normalized_parent->run_id
                     : std::move(overrides.run_id);
  child.workflow_run_id = overrides.workflow_run_id.empty() && policy.inherit_workflow_run_id && normalized_parent
                              ? normalized_parent->workflow_run_id
                              : std::move(overrides.workflow_run_id);
  return build_trace_context(std::move(child), require_span_id);
}

TraceContext derive_child_trace_context(const TraceContext& parent,
                                        TraceSpanDescriptor span,
                                        TraceContext overrides,
                                        TracePropagationPolicy policy) {
  return derive_child_trace_context(&parent, std::move(span), std::move(overrides), std::move(policy));
}

TraceContext derive_child_trace_context(const TraceContext& parent,
                                        std::string span_name,
                                        TraceContext overrides,
                                        TracePropagationPolicy policy) {
  return derive_child_trace_context(
      &parent,
      TraceSpanDescriptor{.name = std::move(span_name)},
      std::move(overrides),
      std::move(policy));
}

TraceContext create_child_trace_context(const TraceContext& parent, TraceContext overrides) {
  return derive_child_trace_context(parent, TraceSpanDescriptor{}, std::move(overrides));
}

std::optional<TraceContext> get_trace_context(const Value& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }
  if (value.contains("traceContext") && js_truthy_trace_context_value(value.at("traceContext"))) {
    return get_trace_context(value.at("traceContext"));
  }
  if (!value.contains("traceId") || !value.at("traceId").is_string()) {
    return std::nullopt;
  }
  return create_trace_context(TraceContext{
      .trace_id = value.at("traceId").as_string(),
      .span_id = string_field_from_value(value, "spanId"),
      .parent_span_id = string_field_from_value(value, "parentSpanId"),
      .span_name = string_field_from_value(value, "spanName"),
      .run_id = string_field_from_value(value, "runId"),
      .workflow_run_id = string_field_from_value(value, "workflowRunId"),
  });
}

NormalizedFrameworkEventTrace normalize_framework_event_trace(TraceContext event_trace,
                                                              std::optional<TraceContext> fallback_context,
                                                              TracePropagationPolicy policy) {
  const bool require_span_id = policy_require_span_id(policy, false);
  std::optional<TraceContext> base_context;
  if (!event_trace.trace_id.empty()) {
    base_context = build_trace_context(event_trace, require_span_id);
  } else if (fallback_context) {
    base_context = build_trace_context(*fallback_context, require_span_id);
  }

  NormalizedFrameworkEventTrace normalized;
  normalized.trace_context = base_context;
  if (!base_context) {
    return normalized;
  }

  normalized.trace_id = event_trace.trace_id.empty() ? base_context->trace_id : event_trace.trace_id;
  normalized.span_id = event_trace.span_id.empty() ? base_context->span_id : event_trace.span_id;
  normalized.parent_span_id = event_trace.parent_span_id.empty()
                                  ? base_context->parent_span_id
                                  : event_trace.parent_span_id;
  normalized.span_name = event_trace.span_name.empty() ? base_context->span_name : event_trace.span_name;
  normalized.run_id = event_trace.run_id.empty() && policy.inherit_run_id
                          ? base_context->run_id
                          : event_trace.run_id;
  normalized.workflow_run_id = event_trace.workflow_run_id.empty() && policy.inherit_workflow_run_id
                                   ? base_context->workflow_run_id
                                   : event_trace.workflow_run_id;
  return normalized;
}

EventBus::EventBus(Options options) : include_raw_(options.include_raw) {
  for (auto& sink : options.sinks) {
    if (sink) {
      sinks_.push_back({next_sink_id_++, std::move(sink)});
    }
  }
}

std::size_t EventBus::register_sink(Sink sink) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto id = next_sink_id_++;
  sinks_.push_back({id, std::move(sink)});
  return id;
}

void EventBus::unregister_sink(std::size_t sink_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  sinks_.erase(std::remove_if(sinks_.begin(), sinks_.end(), [&](const auto& entry) {
                 return entry.first == sink_id;
               }),
               sinks_.end());
}

FrameworkEvent EventBus::publish(std::string category, ExecutionTarget target, Value payload, TraceContext trace) {
  FrameworkEvent event;
  event.event_id = generate_uuid();
  event.timestamp = now_iso8601();
  event.category = std::move(category);
  event.target = target;
  if (trace_context_has_values(trace)) {
    TracePropagationPolicy policy;
    policy.require_span_id = false;
    auto normalized = normalize_framework_event_trace({}, std::move(trace), policy);
    if (normalized.trace_context) {
      event.trace = std::move(*normalized.trace_context);
    }
  }
  event.payload = std::move(payload);
  std::vector<std::pair<std::size_t, Sink>> sinks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    sinks = sinks_;
  }
  for (const auto& [_, sink] : sinks) {
    try {
      sink(event);
    } catch (...) {
    }
  }
  return event;
}

bool EventBus::include_raw() const noexcept {
  return include_raw_;
}

namespace {

RealtimeMessage make_realtime_message(std::string topic, Value payload, RealtimePublishOptions options) {
  RealtimeMessage message;
  message.id = options.id.empty() ? generate_uuid() : std::move(options.id);
  message.topic = std::move(topic);
  message.timestamp = options.timestamp.empty() ? now_iso8601() : std::move(options.timestamp);
  message.payload = std::move(payload);
  message.metadata = std::move(options.metadata);
  return message;
}

Value realtime_message_to_value(const RealtimeMessage& message) {
  return Value::object({{"id", message.id},
                        {"topic", message.topic},
                        {"timestamp", message.timestamp},
                        {"payload", message.payload},
                        {"metadata", message.metadata}});
}

std::size_t resolve_realtime_replay_count(const RealtimeSubscribeOptions& options, std::size_t replay_limit) {
  if (options.replay_count) {
    return *options.replay_count;
  }
  return options.replay ? replay_limit : 0;
}

}  // namespace

InMemoryRealtimeBus::InMemoryRealtimeBus(std::size_t replay_limit) : replay_limit_(replay_limit) {}

InMemoryRealtimeBus::InMemoryRealtimeBus(InMemoryRealtimeBusOptions options)
    : InMemoryRealtimeBus(options.replay_limit) {}

RealtimeMessage InMemoryRealtimeBus::publish(std::string topic, Value payload, Value metadata) {
  RealtimePublishOptions options;
  options.metadata = std::move(metadata);
  return publish(std::move(topic), std::move(payload), std::move(options));
}

RealtimeMessage InMemoryRealtimeBus::publish(std::string topic, Value payload, RealtimePublishOptions options) {
  RealtimeMessage message = make_realtime_message(std::move(topic), std::move(payload), std::move(options));
  std::vector<Handler> handlers;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    record(message);
    for (const auto& [_, subscription] : subscriptions_) {
      if (matches_topic(subscription.first.pattern, message.topic)) {
        handlers.push_back(subscription.second);
      }
    }
  }
  for (const auto& handler : handlers) {
    if (handler) {
      handler(message);
    }
  }
  return message;
}

RealtimeSubscription InMemoryRealtimeBus::subscribe(std::string pattern, Handler handler, std::size_t replay_count) {
  RealtimeSubscribeOptions options;
  options.replay_count = replay_count;
  return subscribe(std::move(pattern), std::move(handler), std::move(options));
}

RealtimeSubscription InMemoryRealtimeBus::subscribe(std::string pattern,
                                                    Handler handler,
                                                    RealtimeSubscribeOptions options) {
  const auto replay_count = resolve_realtime_replay_count(options, replay_limit_);
  RealtimeSubscription subscription{generate_uuid(), std::move(pattern)};
  const auto id = subscription.id;
  std::vector<RealtimeMessage> messages;
  Handler stored_handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    subscriptions_[id] = {subscription, std::move(handler)};
    stored_handler = subscriptions_.at(id).second;
    if (replay_count > 0) {
      for (const auto& message : replay_buffer_) {
        if (matches_topic(subscription.pattern, message.topic)) {
          messages.push_back(message);
        }
      }
      if (messages.size() > replay_count) {
        messages.erase(messages.begin(), messages.end() - static_cast<std::ptrdiff_t>(replay_count));
      }
    }
  }
  for (const auto& message : messages) {
    if (stored_handler) {
      stored_handler(message);
    }
  }
  return subscription;
}

void InMemoryRealtimeBus::unsubscribe(const std::string& subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  subscriptions_.erase(subscription_id);
}

void InMemoryRealtimeBus::unsubscribe(const RealtimeSubscription& subscription) {
  unsubscribe(subscription.id);
}

std::vector<RealtimeMessage> InMemoryRealtimeBus::replay(std::string pattern, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RealtimeMessage> messages;
  for (const auto& message : replay_buffer_) {
    if (matches_topic(pattern, message.topic)) {
      messages.push_back(message);
    }
  }
  const std::size_t resolved_limit = limit == 0 ? replay_limit_ : limit;
  if (resolved_limit > 0 && messages.size() > resolved_limit) {
    messages.erase(messages.begin(), messages.end() - static_cast<std::ptrdiff_t>(resolved_limit));
  }
  return messages;
}

bool InMemoryRealtimeBus::matches_topic(const std::string& pattern, const std::string& topic) {
  if (pattern == topic) {
    return true;
  }
  if (pattern.size() < 2 || pattern.substr(pattern.size() - 2) != ".*") {
    return false;
  }
  const std::string prefix = pattern.substr(0, pattern.size() - 2);
  return topic.rfind(prefix + ".", 0) == 0;
}

void InMemoryRealtimeBus::record(const RealtimeMessage& message) {
  if (replay_limit_ == 0) {
    return;
  }
  replay_buffer_.push_back(message);
  while (replay_buffer_.size() > replay_limit_) {
    replay_buffer_.pop_front();
  }
}

RedisRealtimeBus::RedisRealtimeBus(RedisRealtimeBusOptions options)
    : publisher_(std::move(options.publisher)),
      subscriber_(std::move(options.subscriber)),
      channel_prefix_(std::move(options.channel_prefix)),
      replay_limit_(options.replay_limit) {
  if (!publisher_) {
    throw ConfigurationError("RedisRealtimeBus requires a publisher client.");
  }
  if (!subscriber_) {
    subscriber_ = publisher_;
  }
  subscriber_->set_pmessage_handler([this](const std::string& pattern,
                                           const std::string& channel,
                                           const std::string& message) {
    receive_pmessage(pattern, channel, message);
  });
}

RedisRealtimeBus::~RedisRealtimeBus() {
  close();
}

RealtimeMessage RedisRealtimeBus::publish(std::string topic, Value payload, Value metadata) {
  RealtimePublishOptions options;
  options.metadata = std::move(metadata);
  return publish(std::move(topic), std::move(payload), std::move(options));
}

RealtimeMessage RedisRealtimeBus::publish(std::string topic, Value payload, RealtimePublishOptions options) {
  RealtimeMessage message = make_realtime_message(std::move(topic), std::move(payload), std::move(options));
  std::shared_ptr<RedisRealtimeClient> publisher;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      throw AdapterError("RedisRealtimeBus is closed.");
    }
    record(message);
    locally_published_ids_.insert(message.id);
    publisher = publisher_;
  }
  publisher->publish(topic_to_channel(message.topic), realtime_message_to_value(message).stringify(0));
  deliver(message);
  return message;
}

RealtimeSubscription RedisRealtimeBus::subscribe(std::string pattern, Handler handler, std::size_t replay_count) {
  RealtimeSubscribeOptions options;
  options.replay_count = replay_count;
  return subscribe(std::move(pattern), std::move(handler), std::move(options));
}

RealtimeSubscription RedisRealtimeBus::subscribe(std::string pattern,
                                                 Handler handler,
                                                 RealtimeSubscribeOptions options) {
  if (!handler) {
    throw ConfigurationError("RedisRealtimeBus subscription requires a handler.");
  }
  const auto replay_count = resolve_realtime_replay_count(options, replay_limit_);
  RealtimeSubscription subscription{generate_uuid(), std::move(pattern)};
  const auto id = subscription.id;
  const auto redis_pattern = pattern_to_channel_pattern(subscription.pattern);
  std::vector<RealtimeMessage> messages;
  Handler stored_handler;
  std::shared_ptr<RedisRealtimeClient> subscriber;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      throw AdapterError("RedisRealtimeBus is closed.");
    }
    subscriptions_[id] = StoredSubscription{subscription, std::move(handler), redis_pattern};
    stored_handler = subscriptions_.at(id).handler;
    subscriber = subscriber_;
    if (replay_count > 0) {
      for (const auto& message : replay_buffer_) {
        if (matches_topic(subscription.pattern, message.topic)) {
          messages.push_back(message);
        }
      }
      if (messages.size() > replay_count) {
        messages.erase(messages.begin(), messages.end() - static_cast<std::ptrdiff_t>(replay_count));
      }
    }
  }
  subscriber->psubscribe(redis_pattern);
  for (const auto& message : messages) {
    if (stored_handler) {
      stored_handler(message);
    }
  }
  return subscription;
}

void RedisRealtimeBus::unsubscribe(const std::string& subscription_id) {
  std::shared_ptr<RedisRealtimeClient> subscriber;
  std::string redis_pattern;
  bool should_unsubscribe = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = subscriptions_.find(subscription_id);
    if (found == subscriptions_.end()) {
      return;
    }
    redis_pattern = found->second.redis_pattern;
    subscriptions_.erase(found);
    const auto still_used = std::any_of(subscriptions_.begin(), subscriptions_.end(), [&](const auto& entry) {
      return entry.second.redis_pattern == redis_pattern;
    });
    should_unsubscribe = !still_used && !closed_;
    subscriber = subscriber_;
  }
  if (should_unsubscribe && subscriber) {
    subscriber->punsubscribe(redis_pattern);
  }
}

void RedisRealtimeBus::unsubscribe(const RealtimeSubscription& subscription) {
  unsubscribe(subscription.id);
}

std::vector<RealtimeMessage> RedisRealtimeBus::replay(std::string pattern, std::size_t limit) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RealtimeMessage> messages;
  for (const auto& message : replay_buffer_) {
    if (matches_topic(pattern, message.topic)) {
      messages.push_back(message);
    }
  }
  const std::size_t resolved_limit = limit == 0 ? replay_limit_ : limit;
  if (resolved_limit > 0 && messages.size() > resolved_limit) {
    messages.erase(messages.begin(), messages.end() - static_cast<std::ptrdiff_t>(resolved_limit));
  }
  return messages;
}

void RedisRealtimeBus::close() {
  std::set<std::string> patterns;
  std::shared_ptr<RedisRealtimeClient> subscriber;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    closed_ = true;
    for (const auto& [_, subscription] : subscriptions_) {
      patterns.insert(subscription.redis_pattern);
    }
    subscriptions_.clear();
    locally_published_ids_.clear();
    subscriber = subscriber_;
  }
  for (const auto& pattern : patterns) {
    try {
      subscriber->punsubscribe(pattern);
    } catch (...) {
    }
  }
  if (subscriber) {
    subscriber->set_pmessage_handler({});
  }
}

bool RedisRealtimeBus::matches_topic(const std::string& pattern, const std::string& topic) {
  if (pattern == topic) {
    return true;
  }
  if (pattern.size() < 2 || pattern.substr(pattern.size() - 2) != ".*") {
    return false;
  }
  const std::string prefix = pattern.substr(0, pattern.size() - 2);
  return topic.rfind(prefix + ".", 0) == 0;
}

std::string RedisRealtimeBus::topic_to_channel(const std::string& topic) const {
  return channel_prefix_ + topic;
}

std::string RedisRealtimeBus::pattern_to_channel_pattern(const std::string& pattern) const {
  return channel_prefix_ + pattern;
}

std::optional<RealtimeMessage> RedisRealtimeBus::decode_message(const std::string& channel,
                                                                const std::string& encoded) const {
  if (channel.rfind(channel_prefix_, 0) != 0) {
    return std::nullopt;
  }
  Value raw;
  try {
    raw = parse_json(encoded);
  } catch (...) {
    return std::nullopt;
  }
  if (!raw.is_object() || !raw.at("id").is_string() || !raw.at("topic").is_string()) {
    return std::nullopt;
  }
  RealtimeMessage message;
  message.id = raw.at("id").as_string();
  message.topic = raw.at("topic").as_string();
  message.timestamp = raw.at("timestamp").as_string(now_iso8601());
  message.payload = raw.at("payload");
  message.metadata = raw.contains("metadata") ? raw.at("metadata") : Value::object({});
  return message;
}

void RedisRealtimeBus::receive_pmessage(const std::string& pattern,
                                        const std::string& channel,
                                        const std::string& encoded) {
  (void)pattern;
  auto decoded = decode_message(channel, encoded);
  if (!decoded) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return;
    }
    if (locally_published_ids_.erase(decoded->id) > 0) {
      return;
    }
    record(*decoded);
  }
  deliver(*decoded);
}

void RedisRealtimeBus::deliver(const RealtimeMessage& message) {
  std::map<std::string, StoredSubscription> subscriptions;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    subscriptions = subscriptions_;
  }
  for (const auto& [_, subscription] : subscriptions) {
    if (matches_topic(subscription.subscription.pattern, message.topic)) {
      subscription.handler(message);
    }
  }
}

void RedisRealtimeBus::record(const RealtimeMessage& message) {
  if (replay_limit_ == 0) {
    return;
  }
  replay_buffer_.push_back(message);
  while (replay_buffer_.size() > replay_limit_) {
    replay_buffer_.pop_front();
  }
}

namespace {

Value framework_event_realtime_payload(const FrameworkEvent& event) {
  return Value::object({
      {"eventId", event.event_id},
      {"timestamp", event.timestamp},
      {"category", event.category},
      {"target", to_string(event.target)},
      {"payload", event.payload},
      {"traceId", event.trace.trace_id.empty() ? Value() : Value(event.trace.trace_id)},
      {"spanId", event.trace.span_id.empty() ? Value() : Value(event.trace.span_id)},
      {"parentSpanId", event.trace.parent_span_id.empty() ? Value() : Value(event.trace.parent_span_id)},
      {"spanName", event.trace.span_name.empty() ? Value() : Value(event.trace.span_name)},
      {"runId", event.trace.run_id.empty() ? Value() : Value(event.trace.run_id)},
      {"workflowRunId", event.trace.workflow_run_id.empty() ? Value() : Value(event.trace.workflow_run_id)},
  });
}

}  // namespace

EventBusRealtimeSink::EventBusRealtimeSink(InMemoryRealtimeBus& bus, std::string topic, Value metadata)
    : EventBusRealtimeSink(
          bus,
          EventBusRealtimeSinkOptions{
              .topic = std::move(topic),
              .metadata = std::move(metadata),
          }) {}

EventBusRealtimeSink::EventBusRealtimeSink(InMemoryRealtimeBus& bus, EventBusRealtimeSinkOptions options)
    : EventBusRealtimeSink(
          [&bus](std::string topic, Value payload, RealtimePublishOptions publish_options) {
            return bus.publish(std::move(topic), std::move(payload), std::move(publish_options));
          },
          std::move(options)) {}

EventBusRealtimeSink::EventBusRealtimeSink(RedisRealtimeBus& bus, EventBusRealtimeSinkOptions options)
    : EventBusRealtimeSink(
          [&bus](std::string topic, Value payload, RealtimePublishOptions publish_options) {
            return bus.publish(std::move(topic), std::move(payload), std::move(publish_options));
          },
          std::move(options)) {}

EventBusRealtimeSink::EventBusRealtimeSink(PublishFn publish, EventBusRealtimeSinkOptions options)
    : publish_(std::move(publish)), options_(std::move(options)) {
  if (!publish_) {
    throw ConfigurationError("EventBusRealtimeSink requires a realtime publisher.");
  }
}

std::size_t EventBusRealtimeSink::attach(EventBus& event_bus) {
  return event_bus.register_sink([this](const FrameworkEvent& event) {
    (void)publish(event);
  });
}

RealtimeMessage EventBusRealtimeSink::publish(const FrameworkEvent& event) {
  std::string topic = options_.topic_mapper ? options_.topic_mapper(event) : options_.topic;
  if (topic.empty()) {
    topic = "agent.events";
  }
  RealtimePublishOptions publish_options;
  publish_options.metadata = options_.metadata_mapper ? options_.metadata_mapper(event) : options_.metadata;
  return publish_(std::move(topic), framework_event_realtime_payload(event), std::move(publish_options));
}

std::vector<RealtimeMessage> pipe_values_to_realtime(const std::vector<Value>& values,
                                                     RealtimePublishFn publish,
                                                     std::string topic,
                                                     RealtimePipeMapper mapper) {
  if (!publish) {
    throw ConfigurationError("pipe_values_to_realtime requires a realtime publisher.");
  }
  std::vector<RealtimeMessage> messages;
  messages.reserve(values.size());
  for (std::size_t index = 0; index < values.size(); ++index) {
    RealtimePipeEnvelope envelope = mapper ? mapper(values[index], index)
                                           : RealtimePipeEnvelope{.payload = values[index]};
    if (envelope.topic.empty()) {
      envelope.topic = topic;
    }
    RealtimePublishOptions options;
    options.metadata = std::move(envelope.metadata);
    messages.push_back(publish(std::move(envelope.topic), std::move(envelope.payload), std::move(options)));
  }
  return messages;
}

std::vector<RealtimeMessage> pipe_values_to_realtime(const std::vector<Value>& values,
                                                     InMemoryRealtimeBus& bus,
                                                     std::string topic,
                                                     RealtimePipeMapper mapper) {
  return pipe_values_to_realtime(
      values,
      [&bus](std::string publish_topic, Value payload, RealtimePublishOptions options) {
        return bus.publish(std::move(publish_topic), std::move(payload), std::move(options));
      },
      std::move(topic),
      std::move(mapper));
}

std::vector<RealtimeMessage> pipe_values_to_realtime(const std::vector<Value>& values,
                                                     RedisRealtimeBus& bus,
                                                     std::string topic,
                                                     RealtimePipeMapper mapper) {
  return pipe_values_to_realtime(
      values,
      [&bus](std::string publish_topic, Value payload, RealtimePublishOptions options) {
        return bus.publish(std::move(publish_topic), std::move(payload), std::move(options));
      },
      std::move(topic),
      std::move(mapper));
}
}  // namespace agent
