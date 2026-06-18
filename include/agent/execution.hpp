#pragma once

#include "agent/model.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <optional>
#include <random>
#include <thread>

namespace agent {

enum class ExecutionTarget {
  Run,
  Model,
  Tool,
  Retrieval,
  Permission,
  Workflow,
  WorkflowNode,
  ChildAgent,
  Skill,
};

std::string to_string(ExecutionTarget target);

class CancellationToken {
 public:
  using Callback = std::function<void(const std::string& reason)>;

  void cancel(std::string reason = "cancelled");
  [[nodiscard]] bool cancelled() const;
  [[nodiscard]] std::string reason() const;
  void throw_if_cancelled(ExecutionTarget target) const;
  std::size_t add_callback(Callback callback);
  void remove_callback(std::size_t callback_id);

 private:
  mutable std::mutex mutex_;
  bool cancelled_ = false;
  std::string reason_;
  std::size_t next_callback_id_ = 1;
  std::map<std::size_t, Callback> callbacks_;
};

enum class RetryStrategy {
  None,
  Fixed,
  Exponential,
};

struct RetryContext {
  ExecutionTarget target = ExecutionTarget::Run;
  int attempt = 1;
  std::string error;
  Value metadata = Value::object({});
};

struct RetryScheduledContext : public RetryContext {
  int delay_ms = 0;
};

using RetryScheduledHandler = std::function<void(const RetryScheduledContext&)>;

struct RetryPolicy {
  int max_attempts = 1;
  RetryStrategy strategy = RetryStrategy::None;
  int base_delay_ms = 0;
  int max_delay_ms = 0;
  bool jitter = false;
  std::function<bool(const RetryContext&)> retry_on;
};

inline int resolve_retry_delay_ms(const RetryPolicy& retry, int attempt) {
  const int base_delay_ms = std::max(0, retry.base_delay_ms);
  const int max_delay_ms = std::max(base_delay_ms, retry.max_delay_ms);
  if (retry.strategy == RetryStrategy::None) {
    return 0;
  }

  int delay_ms = base_delay_ms;
  if (retry.strategy == RetryStrategy::Exponential) {
    long long raw_delay_ms = base_delay_ms;
    for (int step = 0; step < std::max(0, attempt - 1); ++step) {
      if (raw_delay_ms >= max_delay_ms || raw_delay_ms > max_delay_ms / 2) {
        raw_delay_ms = max_delay_ms;
        break;
      }
      raw_delay_ms *= 2;
    }
    delay_ms = static_cast<int>(std::min<long long>(raw_delay_ms, max_delay_ms));
  }

  if (retry.jitter && delay_ms > 0) {
    thread_local std::mt19937 generator{std::random_device{}()};
    std::uniform_int_distribution<int> distribution(delay_ms / 2, delay_ms - 1);
    return distribution(generator);
  }
  return delay_ms;
}

struct TimeoutPolicy {
  std::map<ExecutionTarget, int> timeout_ms;
};

struct ExecutionPolicies {
  std::map<ExecutionTarget, RetryPolicy> retry;
  TimeoutPolicy timeout;
};

inline int timeout_ms_for_target(const TimeoutPolicy& policy, ExecutionTarget target) {
  if (target == ExecutionTarget::Permission) {
    return 0;
  }
  const auto found = policy.timeout_ms.find(target);
  return found == policy.timeout_ms.end() ? 0 : std::max(0, found->second);
}

inline void throw_if_timed_out(ExecutionTarget target,
                               int timeout_ms,
                               std::chrono::steady_clock::time_point started_at) {
  if (timeout_ms <= 0) {
    return;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at);
  if (elapsed.count() > timeout_ms) {
    throw TimeoutError("Execution timed out for " + to_string(target) + ".",
                       to_string(target),
                       timeout_ms);
  }
}

inline void sleep_with_cancellation(std::chrono::milliseconds delay,
                                    ExecutionTarget target,
                                    CancellationToken* token) {
  auto remaining = delay;
  constexpr auto chunk = std::chrono::milliseconds(50);
  while (remaining.count() > 0) {
    if (token) {
      token->throw_if_cancelled(target);
    }
    const auto current = remaining < chunk ? remaining : chunk;
    std::this_thread::sleep_for(current);
    remaining -= current;
  }
  if (token) {
    token->throw_if_cancelled(target);
  }
}

template <typename Fn>
auto execute_with_policies(ExecutionTarget target, const ExecutionPolicies& policies, const Value& metadata,
                           CancellationToken* token, Fn operation,
                           RetryScheduledHandler on_retry_scheduled = {}) -> decltype(operation()) {
  auto retry_it = policies.retry.find(target);
  const RetryPolicy retry = retry_it == policies.retry.end() ? RetryPolicy{} : retry_it->second;
  const int timeout_ms = timeout_ms_for_target(policies.timeout, target);
  const int max_attempts = std::max(1, retry.max_attempts);
  std::string last_error;
  std::exception_ptr last_exception;
  bool attempted_retry = false;

  for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    if (token) {
      token->throw_if_cancelled(target);
    }
    try {
      const auto started_at = std::chrono::steady_clock::now();
      auto result = operation();
      throw_if_timed_out(target, timeout_ms, started_at);
      if (token) {
        token->throw_if_cancelled(target);
      }
      return result;
    } catch (const std::exception& error) {
      last_error = error.what();
      last_exception = std::current_exception();
      if (token) {
        token->throw_if_cancelled(target);
      }
      if (attempt >= max_attempts) {
        break;
      }
      const bool should_retry = retry.retry_on
                                    ? retry.retry_on(RetryContext{target, attempt, last_error, metadata})
                                    : false;
      if (!should_retry) {
        break;
      }
      attempted_retry = true;
      const int delay_ms = resolve_retry_delay_ms(retry, attempt);
      if (on_retry_scheduled) {
        RetryScheduledContext scheduled;
        scheduled.target = target;
        scheduled.attempt = attempt;
        scheduled.error = last_error;
        scheduled.metadata = metadata;
        scheduled.delay_ms = delay_ms;
        on_retry_scheduled(scheduled);
      }
      if (delay_ms > 0) {
        sleep_with_cancellation(std::chrono::milliseconds(delay_ms), target, token);
      }
    }
  }

  if (!attempted_retry && last_exception) {
    std::rethrow_exception(last_exception);
  }
  throw RetryExhaustedError("Execution failed for " + to_string(target) + " after retries.",
                            to_string(target), max_attempts);
}

struct TraceContext {
  std::string trace_id;
  std::string span_id;
  std::string parent_span_id;
  std::string span_name;
  std::string run_id;
  std::string workflow_run_id;
};

struct TraceSpanDescriptor {
  std::string name;
  std::optional<ExecutionTarget> target;
  Value attributes = Value::object({});
};

struct TracePropagationPolicy {
  std::optional<bool> require_span_id;
  bool inherit_run_id = true;
  bool inherit_workflow_run_id = true;
};

struct NormalizedFrameworkEventTrace {
  std::optional<TraceContext> trace_context;
  std::string trace_id;
  std::string span_id;
  std::string parent_span_id;
  std::string span_name;
  std::string run_id;
  std::string workflow_run_id;
};

TraceContext create_trace_context(TraceContext value = {});
TraceContext derive_child_trace_context(const TraceContext* parent,
                                        TraceSpanDescriptor span = {},
                                        TraceContext overrides = {},
                                        TracePropagationPolicy policy = {});
TraceContext derive_child_trace_context(const TraceContext& parent,
                                        TraceSpanDescriptor span = {},
                                        TraceContext overrides = {},
                                        TracePropagationPolicy policy = {});
TraceContext derive_child_trace_context(const TraceContext& parent,
                                        std::string span_name,
                                        TraceContext overrides = {},
                                        TracePropagationPolicy policy = {});
TraceContext create_child_trace_context(const TraceContext& parent, TraceContext overrides = {});
std::optional<TraceContext> get_trace_context(const Value& value);
NormalizedFrameworkEventTrace normalize_framework_event_trace(
    TraceContext event_trace = {},
    std::optional<TraceContext> fallback_context = std::nullopt,
    TracePropagationPolicy policy = {});

struct FrameworkEvent {
  std::string event_id;
  std::string timestamp;
  std::string category;
  ExecutionTarget target = ExecutionTarget::Run;
  TraceContext trace;
  Value payload = Value::object({});
};

class EventBus {
 public:
  using Sink = std::function<void(const FrameworkEvent&)>;

  struct Options {
    std::vector<Sink> sinks;
    bool include_raw = false;
  };

  EventBus() = default;
  explicit EventBus(Options options);
  std::size_t register_sink(Sink sink);
  void unregister_sink(std::size_t sink_id);
  FrameworkEvent publish(std::string category, ExecutionTarget target, Value payload = Value::object({}),
                         TraceContext trace = {});
  [[nodiscard]] bool include_raw() const noexcept;

 private:
  mutable std::mutex mutex_;
  std::size_t next_sink_id_ = 1;
  bool include_raw_ = false;
  std::vector<std::pair<std::size_t, Sink>> sinks_;
};

struct RealtimeMessage {
  std::string id;
  std::string topic;
  std::string timestamp;
  Value payload;
  Value metadata = Value::object({});
};

struct RealtimePublishOptions {
  std::string id;
  std::string timestamp;
  Value metadata = Value::object({});
};

struct RealtimeSubscribeOptions {
  bool replay = false;
  std::optional<std::size_t> replay_count;
};

struct InMemoryRealtimeBusOptions {
  std::size_t replay_limit = 100;
};

using RealtimePublishFn = std::function<RealtimeMessage(std::string, Value, RealtimePublishOptions)>;

struct RealtimeSubscription {
  std::string id;
  std::string pattern;
};

class InMemoryRealtimeBus {
 public:
  using Handler = std::function<void(const RealtimeMessage&)>;

  explicit InMemoryRealtimeBus(std::size_t replay_limit = 100);
  explicit InMemoryRealtimeBus(InMemoryRealtimeBusOptions options);
  RealtimeMessage publish(std::string topic, Value payload, Value metadata = Value::object({}));
  RealtimeMessage publish(std::string topic, Value payload, RealtimePublishOptions options);
  RealtimeSubscription subscribe(std::string pattern, Handler handler, std::size_t replay = 0);
  RealtimeSubscription subscribe(std::string pattern, Handler handler, RealtimeSubscribeOptions options);
  void unsubscribe(const std::string& subscription_id);
  void unsubscribe(const RealtimeSubscription& subscription);
  [[nodiscard]] std::vector<RealtimeMessage> replay(std::string pattern, std::size_t limit = 0) const;

 private:
  [[nodiscard]] static bool matches_topic(const std::string& pattern, const std::string& topic);
  void record(const RealtimeMessage& message);

  mutable std::mutex mutex_;
  std::size_t replay_limit_;
  std::map<std::string, std::pair<RealtimeSubscription, Handler>> subscriptions_;
  std::deque<RealtimeMessage> replay_buffer_;
};

class RedisRealtimeClient {
 public:
  using PMessageHandler = std::function<void(const std::string& pattern,
                                             const std::string& channel,
                                             const std::string& message)>;

  virtual ~RedisRealtimeClient() = default;
  virtual long long publish(std::string channel, std::string message) = 0;
  virtual void psubscribe(std::string pattern) = 0;
  virtual void punsubscribe(std::string pattern) = 0;
  virtual void set_pmessage_handler(PMessageHandler handler) = 0;
  virtual void close() {}
};

struct RedisRealtimeBusOptions {
  std::shared_ptr<RedisRealtimeClient> publisher;
  std::shared_ptr<RedisRealtimeClient> subscriber;
  std::string channel_prefix = "node-agent:realtime:";
  std::size_t replay_limit = 100;
};

class RedisRealtimeBus {
 public:
  using Handler = InMemoryRealtimeBus::Handler;

  explicit RedisRealtimeBus(RedisRealtimeBusOptions options);
  ~RedisRealtimeBus();

  RedisRealtimeBus(const RedisRealtimeBus&) = delete;
  RedisRealtimeBus& operator=(const RedisRealtimeBus&) = delete;

  RealtimeMessage publish(std::string topic, Value payload, Value metadata = Value::object({}));
  RealtimeMessage publish(std::string topic, Value payload, RealtimePublishOptions options);
  RealtimeSubscription subscribe(std::string pattern, Handler handler, std::size_t replay = 0);
  RealtimeSubscription subscribe(std::string pattern, Handler handler, RealtimeSubscribeOptions options);
  void unsubscribe(const std::string& subscription_id);
  void unsubscribe(const RealtimeSubscription& subscription);
  [[nodiscard]] std::vector<RealtimeMessage> replay(std::string pattern, std::size_t limit = 0) const;
  void close();

 private:
  struct StoredSubscription {
    RealtimeSubscription subscription;
    Handler handler;
    std::string redis_pattern;
  };

  [[nodiscard]] static bool matches_topic(const std::string& pattern, const std::string& topic);
  [[nodiscard]] std::string topic_to_channel(const std::string& topic) const;
  [[nodiscard]] std::string pattern_to_channel_pattern(const std::string& pattern) const;
  [[nodiscard]] std::optional<RealtimeMessage> decode_message(const std::string& channel,
                                                               const std::string& encoded) const;
  void receive_pmessage(const std::string& pattern, const std::string& channel, const std::string& encoded);
  void deliver(const RealtimeMessage& message);
  void record(const RealtimeMessage& message);

  mutable std::mutex mutex_;
  std::shared_ptr<RedisRealtimeClient> publisher_;
  std::shared_ptr<RedisRealtimeClient> subscriber_;
  std::string channel_prefix_;
  std::size_t replay_limit_;
  bool closed_ = false;
  std::map<std::string, StoredSubscription> subscriptions_;
  std::deque<RealtimeMessage> replay_buffer_;
  std::set<std::string> locally_published_ids_;
};

struct EventBusRealtimeSinkOptions {
  std::string topic = "agent.events";
  Value metadata = Value::object({});
  std::function<std::string(const FrameworkEvent&)> topic_mapper;
  std::function<Value(const FrameworkEvent&)> metadata_mapper;
};

struct RealtimePipeEnvelope {
  std::string topic;
  Value payload;
  Value metadata = Value::object({});
};

using RealtimePipeMapper = std::function<RealtimePipeEnvelope(const Value&, std::size_t)>;

class EventBusRealtimeSink {
 public:
  using PublishFn = RealtimePublishFn;

  EventBusRealtimeSink(InMemoryRealtimeBus& bus, std::string topic = "agent.events",
                       Value metadata = Value::object({}));
  EventBusRealtimeSink(InMemoryRealtimeBus& bus, EventBusRealtimeSinkOptions options);
  EventBusRealtimeSink(RedisRealtimeBus& bus, EventBusRealtimeSinkOptions options);
  EventBusRealtimeSink(PublishFn publish, EventBusRealtimeSinkOptions options = {});
  std::size_t attach(EventBus& event_bus);
  RealtimeMessage publish(const FrameworkEvent& event);

 private:
  PublishFn publish_;
  EventBusRealtimeSinkOptions options_;
};

std::vector<RealtimeMessage> pipe_values_to_realtime(const std::vector<Value>& values,
                                                     RealtimePublishFn publish,
                                                     std::string topic,
                                                     RealtimePipeMapper mapper = {});
std::vector<RealtimeMessage> pipe_values_to_realtime(const std::vector<Value>& values,
                                                     InMemoryRealtimeBus& bus,
                                                     std::string topic,
                                                     RealtimePipeMapper mapper = {});
std::vector<RealtimeMessage> pipe_values_to_realtime(const std::vector<Value>& values,
                                                     RedisRealtimeBus& bus,
                                                     std::string topic,
                                                     RealtimePipeMapper mapper = {});

}  // namespace agent
