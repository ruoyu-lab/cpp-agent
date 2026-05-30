#pragma once

#include "agent/execution.hpp"

#include <iosfwd>
#include <mutex>

namespace agent {

enum class StructuredLogLevel {
  Info,
  Warn,
  Error,
};

enum class MetricKind {
  Counter,
  Histogram,
};

enum class TraceStatus {
  Ok,
  Error,
};

std::string to_string(StructuredLogLevel level);
std::string to_string(MetricKind kind);
std::string to_string(TraceStatus status);

struct StructuredLogRecord {
  StructuredLogLevel level = StructuredLogLevel::Info;
  std::string message;
  Value attributes = Value::object({});
  FrameworkEvent event;
};

struct MetricRecord {
  std::string name;
  MetricKind kind = MetricKind::Counter;
  double value = 0;
  std::string unit;
  Value attributes = Value::object({});
  FrameworkEvent event;
};

struct TraceRecord {
  std::string name;
  TraceStatus status = TraceStatus::Ok;
  std::optional<TraceContext> trace_context;
  Value attributes = Value::object({});
  FrameworkEvent event;
};

struct ObservabilityAdapterError {
  std::string adapter_type;
  std::string error;
  FrameworkEvent event;
};

using LogAdapter = std::function<void(const StructuredLogRecord&)>;
using MetricsAdapter = std::function<void(const MetricRecord&)>;
using TraceAdapter = std::function<void(const TraceRecord&)>;
using ObservabilityAdapterErrorHandler = std::function<void(const ObservabilityAdapterError&)>;

struct ObservabilityPipelineConfig {
  std::vector<LogAdapter> loggers = {};
  std::vector<MetricsAdapter> metrics = {};
  std::vector<TraceAdapter> traces = {};
  ObservabilityAdapterErrorHandler on_adapter_error = {};
};

Value trace_context_to_value(const TraceContext& trace);
Value framework_event_to_value(const FrameworkEvent& event);
Value structured_log_record_to_value(const StructuredLogRecord& record);
Value metric_record_to_value(const MetricRecord& record);
Value trace_record_to_value(const TraceRecord& record);

StructuredLogRecord create_structured_log_record(const FrameworkEvent& event);
std::vector<MetricRecord> create_metric_records(const FrameworkEvent& event);
TraceRecord create_trace_record(const FrameworkEvent& event);

class ObservabilityPipeline {
 public:
  ObservabilityPipeline() = default;
  explicit ObservabilityPipeline(ObservabilityAdapterErrorHandler on_adapter_error);
  explicit ObservabilityPipeline(ObservabilityPipelineConfig config);
  ObservabilityPipeline(const ObservabilityPipeline& other);
  ObservabilityPipeline& operator=(const ObservabilityPipeline& other);

  LogAdapter register_logger(LogAdapter adapter);
  MetricsAdapter register_metrics(MetricsAdapter adapter);
  TraceAdapter register_trace(TraceAdapter adapter);
  void set_adapter_error_handler(ObservabilityAdapterErrorHandler handler);

  void publish(const FrameworkEvent& event) const;
  void attach(EventBus& event_bus) const;

  [[nodiscard]] const std::vector<LogAdapter>& loggers() const noexcept;
  [[nodiscard]] const std::vector<MetricsAdapter>& metrics() const noexcept;
  [[nodiscard]] const std::vector<TraceAdapter>& traces() const noexcept;

 private:
  void report_adapter_error(const std::string& adapter_type, const std::exception& error,
                            const FrameworkEvent& event) const;

  mutable std::mutex mutex_;
  std::vector<LogAdapter> loggers_;
  std::vector<MetricsAdapter> metrics_;
  std::vector<TraceAdapter> traces_;
  ObservabilityAdapterErrorHandler on_adapter_error_;
};

std::shared_ptr<ObservabilityPipeline> create_observability_pipeline(
    ObservabilityAdapterErrorHandler on_adapter_error = {});
std::shared_ptr<ObservabilityPipeline> create_observability_pipeline(
    ObservabilityPipelineConfig config);

class MemoryObservabilityCollector {
 public:
  void reset();
  void record_log(StructuredLogRecord record);
  void record_metric(MetricRecord record);
  void record_trace(TraceRecord record);
  [[nodiscard]] std::vector<StructuredLogRecord> log_records() const;
  [[nodiscard]] std::vector<MetricRecord> metric_records() const;
  [[nodiscard]] std::vector<TraceRecord> trace_records() const;

  std::vector<StructuredLogRecord> logs;
  std::vector<MetricRecord> metrics;
  std::vector<TraceRecord> traces;

 private:
  mutable std::mutex mutex_;
};

struct MemoryObservabilityPipeline {
  std::shared_ptr<MemoryObservabilityCollector> collector;
  std::shared_ptr<ObservabilityPipeline> pipeline;
};

struct MemoryAdapterConfig {
  std::shared_ptr<MemoryObservabilityCollector> collector = {};
};

LogAdapter create_memory_log_adapter(std::shared_ptr<MemoryObservabilityCollector> collector);
LogAdapter create_memory_log_adapter(MemoryAdapterConfig config);
MetricsAdapter create_memory_metrics_adapter(std::shared_ptr<MemoryObservabilityCollector> collector);
MetricsAdapter create_memory_metrics_adapter(MemoryAdapterConfig config);
TraceAdapter create_memory_trace_adapter(std::shared_ptr<MemoryObservabilityCollector> collector);
TraceAdapter create_memory_trace_adapter(MemoryAdapterConfig config);
MemoryObservabilityPipeline create_memory_observability_pipeline(
    std::shared_ptr<MemoryObservabilityCollector> collector = {});
MemoryObservabilityPipeline create_memory_observability_pipeline(
    MemoryAdapterConfig config);

LogAdapter create_console_log_adapter(std::ostream& info, std::ostream& warn, std::ostream& error);
MetricsAdapter create_console_metrics_adapter(std::ostream& info);
TraceAdapter create_console_trace_adapter(std::ostream& info, std::ostream& error);
std::shared_ptr<ObservabilityPipeline> create_console_observability_pipeline();
std::shared_ptr<ObservabilityPipeline> create_console_observability_pipeline(
    std::ostream& info, std::ostream& warn, std::ostream& error);

class OpenTelemetrySpan {
 public:
  virtual ~OpenTelemetrySpan() = default;
  virtual void end(TraceStatus status) = 0;
};

class OpenTelemetryBridge {
 public:
  virtual ~OpenTelemetryBridge() = default;
  virtual void log(const StructuredLogRecord& record);
  virtual void counter(const std::string& name, double value, const Value& attributes);
  virtual void histogram(const std::string& name, double value, const Value& attributes);
  virtual std::unique_ptr<OpenTelemetrySpan> start_span(const std::string& name, const Value& attributes);
};

struct OpenTelemetryAdapterConfig {
  OpenTelemetryBridge* bridge = nullptr;
};

LogAdapter create_opentelemetry_log_adapter(OpenTelemetryBridge& bridge);
LogAdapter create_opentelemetry_log_adapter(OpenTelemetryAdapterConfig config);
MetricsAdapter create_opentelemetry_metrics_adapter(OpenTelemetryBridge& bridge);
MetricsAdapter create_opentelemetry_metrics_adapter(OpenTelemetryAdapterConfig config);
TraceAdapter create_opentelemetry_trace_adapter(OpenTelemetryBridge& bridge);
TraceAdapter create_opentelemetry_trace_adapter(OpenTelemetryAdapterConfig config);
std::shared_ptr<ObservabilityPipeline> create_opentelemetry_observability_pipeline(OpenTelemetryBridge& bridge);
std::shared_ptr<ObservabilityPipeline> create_opentelemetry_observability_pipeline(OpenTelemetryAdapterConfig config);

}  // namespace agent
