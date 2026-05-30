#include "agent/agent.hpp"

#include <iostream>

namespace agent {

namespace {

bool ends_with(const std::string& value, const std::string& suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Value observability_attributes(const FrameworkEvent& event) {
  return Value::object({
      {"category", event.category},
      {"target", to_string(event.target)},
      {"traceId", event.trace.trace_id.empty() ? Value() : Value(event.trace.trace_id)},
      {"spanId", event.trace.span_id.empty() ? Value() : Value(event.trace.span_id)},
      {"parentSpanId", event.trace.parent_span_id.empty() ? Value() : Value(event.trace.parent_span_id)},
      {"spanName", event.trace.span_name.empty() ? Value() : Value(event.trace.span_name)},
      {"runId", event.trace.run_id.empty() ? Value() : Value(event.trace.run_id)},
      {"workflowRunId", event.trace.workflow_run_id.empty() ? Value() : Value(event.trace.workflow_run_id)},
  });
}

StructuredLogLevel level_from_event(const FrameworkEvent& event) {
  if (ends_with(event.category, ".failed")) {
    return StructuredLogLevel::Error;
  }
  if (ends_with(event.category, ".denied") || ends_with(event.category, ".approval_requested") ||
      event.category == "retry.scheduled") {
    return StructuredLogLevel::Warn;
  }
  return StructuredLogLevel::Info;
}

void invoke_error_handler(const ObservabilityAdapterErrorHandler& handler, const std::string& adapter_type,
                          const std::string& error, const FrameworkEvent& event) {
  if (!handler) {
    return;
  }
  try {
    handler(ObservabilityAdapterError{adapter_type, error, event});
  } catch (...) {
  }
}

OpenTelemetryBridge& require_opentelemetry_bridge(OpenTelemetryBridge* bridge) {
  if (!bridge) {
    throw ConfigurationError("OpenTelemetry observability adapter requires a bridge.");
  }
  return *bridge;
}

}  // namespace

std::string to_string(StructuredLogLevel level) {
  switch (level) {
    case StructuredLogLevel::Info:
      return "info";
    case StructuredLogLevel::Warn:
      return "warn";
    case StructuredLogLevel::Error:
      return "error";
  }
  return "info";
}

std::string to_string(MetricKind kind) {
  switch (kind) {
    case MetricKind::Counter:
      return "counter";
    case MetricKind::Histogram:
      return "histogram";
  }
  return "counter";
}

std::string to_string(TraceStatus status) {
  switch (status) {
    case TraceStatus::Ok:
      return "ok";
    case TraceStatus::Error:
      return "error";
  }
  return "ok";
}

Value trace_context_to_value(const TraceContext& trace) {
  return Value::object({
      {"traceId", trace.trace_id},
      {"spanId", trace.span_id},
      {"parentSpanId", trace.parent_span_id.empty() ? Value() : Value(trace.parent_span_id)},
      {"spanName", trace.span_name.empty() ? Value() : Value(trace.span_name)},
      {"runId", trace.run_id.empty() ? Value() : Value(trace.run_id)},
      {"workflowRunId", trace.workflow_run_id.empty() ? Value() : Value(trace.workflow_run_id)},
  });
}

Value framework_event_to_value(const FrameworkEvent& event) {
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

Value structured_log_record_to_value(const StructuredLogRecord& record) {
  return Value::object({
      {"level", to_string(record.level)},
      {"message", record.message},
      {"attributes", record.attributes},
      {"event", framework_event_to_value(record.event)},
  });
}

Value metric_record_to_value(const MetricRecord& record) {
  return Value::object({
      {"name", record.name},
      {"kind", to_string(record.kind)},
      {"value", record.value},
      {"unit", record.unit.empty() ? Value() : Value(record.unit)},
      {"attributes", record.attributes},
      {"event", framework_event_to_value(record.event)},
  });
}

Value trace_record_to_value(const TraceRecord& record) {
  return Value::object({
      {"name", record.name},
      {"status", to_string(record.status)},
      {"traceContext", record.trace_context ? trace_context_to_value(*record.trace_context) : Value()},
      {"attributes", record.attributes},
      {"event", framework_event_to_value(record.event)},
  });
}

StructuredLogRecord create_structured_log_record(const FrameworkEvent& event) {
  return StructuredLogRecord{
      .level = level_from_event(event),
      .message = event.category,
      .attributes = observability_attributes(event),
      .event = event,
  };
}

std::vector<MetricRecord> create_metric_records(const FrameworkEvent& event) {
  return {
      MetricRecord{
          .name = "node_agent_events_total",
          .kind = MetricKind::Counter,
          .value = 1,
          .attributes = observability_attributes(event),
          .event = event,
      },
  };
}

TraceRecord create_trace_record(const FrameworkEvent& event) {
  TracePropagationPolicy policy;
  policy.require_span_id = false;
  const auto normalized = normalize_framework_event_trace(event.trace, std::nullopt, policy);
  return TraceRecord{
      .name = event.trace.span_name.empty() ? event.category : event.trace.span_name,
      .status = ends_with(event.category, ".failed") ? TraceStatus::Error : TraceStatus::Ok,
      .trace_context = normalized.trace_context,
      .attributes = observability_attributes(event),
      .event = event,
  };
}

ObservabilityPipeline::ObservabilityPipeline(ObservabilityAdapterErrorHandler on_adapter_error)
    : on_adapter_error_(std::move(on_adapter_error)) {}

ObservabilityPipeline::ObservabilityPipeline(ObservabilityPipelineConfig config)
    : loggers_(std::move(config.loggers)),
      metrics_(std::move(config.metrics)),
      traces_(std::move(config.traces)),
      on_adapter_error_(std::move(config.on_adapter_error)) {}

ObservabilityPipeline::ObservabilityPipeline(const ObservabilityPipeline& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  loggers_ = other.loggers_;
  metrics_ = other.metrics_;
  traces_ = other.traces_;
  on_adapter_error_ = other.on_adapter_error_;
}

ObservabilityPipeline& ObservabilityPipeline::operator=(const ObservabilityPipeline& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  loggers_ = other.loggers_;
  metrics_ = other.metrics_;
  traces_ = other.traces_;
  on_adapter_error_ = other.on_adapter_error_;
  return *this;
}

LogAdapter ObservabilityPipeline::register_logger(LogAdapter adapter) {
  std::lock_guard<std::mutex> lock(mutex_);
  loggers_.push_back(std::move(adapter));
  return loggers_.back();
}

MetricsAdapter ObservabilityPipeline::register_metrics(MetricsAdapter adapter) {
  std::lock_guard<std::mutex> lock(mutex_);
  metrics_.push_back(std::move(adapter));
  return metrics_.back();
}

TraceAdapter ObservabilityPipeline::register_trace(TraceAdapter adapter) {
  std::lock_guard<std::mutex> lock(mutex_);
  traces_.push_back(std::move(adapter));
  return traces_.back();
}

void ObservabilityPipeline::set_adapter_error_handler(ObservabilityAdapterErrorHandler handler) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_adapter_error_ = std::move(handler);
}

void ObservabilityPipeline::publish(const FrameworkEvent& event) const {
  const auto log_record = create_structured_log_record(event);
  const auto metric_records = create_metric_records(event);
  const auto trace_record = create_trace_record(event);
  std::vector<LogAdapter> loggers;
  std::vector<MetricsAdapter> metrics;
  std::vector<TraceAdapter> traces;
  ObservabilityAdapterErrorHandler on_adapter_error;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    loggers = loggers_;
    metrics = metrics_;
    traces = traces_;
    on_adapter_error = on_adapter_error_;
  }

  for (const auto& adapter : loggers) {
    try {
      adapter(log_record);
    } catch (const std::exception& error) {
      invoke_error_handler(on_adapter_error, "log", error.what(), event);
    } catch (...) {
      invoke_error_handler(on_adapter_error, "log", "unknown adapter error", event);
    }
  }

  for (const auto& adapter : metrics) {
    for (const auto& metric_record : metric_records) {
      try {
        adapter(metric_record);
      } catch (const std::exception& error) {
        invoke_error_handler(on_adapter_error, "metrics", error.what(), event);
      } catch (...) {
        invoke_error_handler(on_adapter_error, "metrics", "unknown adapter error", event);
      }
    }
  }

  for (const auto& adapter : traces) {
    try {
      adapter(trace_record);
    } catch (const std::exception& error) {
      invoke_error_handler(on_adapter_error, "trace", error.what(), event);
    } catch (...) {
      invoke_error_handler(on_adapter_error, "trace", "unknown adapter error", event);
    }
  }
}

void ObservabilityPipeline::attach(EventBus& event_bus) const {
  event_bus.register_sink([pipeline = *this](const FrameworkEvent& event) { pipeline.publish(event); });
}

const std::vector<LogAdapter>& ObservabilityPipeline::loggers() const noexcept {
  return loggers_;
}

const std::vector<MetricsAdapter>& ObservabilityPipeline::metrics() const noexcept {
  return metrics_;
}

const std::vector<TraceAdapter>& ObservabilityPipeline::traces() const noexcept {
  return traces_;
}

void ObservabilityPipeline::report_adapter_error(const std::string& adapter_type, const std::exception& error,
                                                 const FrameworkEvent& event) const {
  ObservabilityAdapterErrorHandler handler;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    handler = on_adapter_error_;
  }
  invoke_error_handler(handler, adapter_type, error.what(), event);
}

std::shared_ptr<ObservabilityPipeline> create_observability_pipeline(
    ObservabilityAdapterErrorHandler on_adapter_error) {
  return std::make_shared<ObservabilityPipeline>(std::move(on_adapter_error));
}

std::shared_ptr<ObservabilityPipeline> create_observability_pipeline(
    ObservabilityPipelineConfig config) {
  return std::make_shared<ObservabilityPipeline>(std::move(config));
}

void MemoryObservabilityCollector::reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  logs.clear();
  metrics.clear();
  traces.clear();
}

void MemoryObservabilityCollector::record_log(StructuredLogRecord record) {
  std::lock_guard<std::mutex> lock(mutex_);
  logs.push_back(std::move(record));
}

void MemoryObservabilityCollector::record_metric(MetricRecord record) {
  std::lock_guard<std::mutex> lock(mutex_);
  metrics.push_back(std::move(record));
}

void MemoryObservabilityCollector::record_trace(TraceRecord record) {
  std::lock_guard<std::mutex> lock(mutex_);
  traces.push_back(std::move(record));
}

std::vector<StructuredLogRecord> MemoryObservabilityCollector::log_records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return logs;
}

std::vector<MetricRecord> MemoryObservabilityCollector::metric_records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics;
}

std::vector<TraceRecord> MemoryObservabilityCollector::trace_records() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return traces;
}

LogAdapter create_memory_log_adapter(std::shared_ptr<MemoryObservabilityCollector> collector) {
  if (!collector) {
    collector = std::make_shared<MemoryObservabilityCollector>();
  }
  return [collector = std::move(collector)](const StructuredLogRecord& record) {
    collector->record_log(record);
  };
}

LogAdapter create_memory_log_adapter(MemoryAdapterConfig config) {
  return create_memory_log_adapter(std::move(config.collector));
}

MetricsAdapter create_memory_metrics_adapter(std::shared_ptr<MemoryObservabilityCollector> collector) {
  if (!collector) {
    collector = std::make_shared<MemoryObservabilityCollector>();
  }
  return [collector = std::move(collector)](const MetricRecord& record) {
    collector->record_metric(record);
  };
}

MetricsAdapter create_memory_metrics_adapter(MemoryAdapterConfig config) {
  return create_memory_metrics_adapter(std::move(config.collector));
}

TraceAdapter create_memory_trace_adapter(std::shared_ptr<MemoryObservabilityCollector> collector) {
  if (!collector) {
    collector = std::make_shared<MemoryObservabilityCollector>();
  }
  return [collector = std::move(collector)](const TraceRecord& record) {
    collector->record_trace(record);
  };
}

TraceAdapter create_memory_trace_adapter(MemoryAdapterConfig config) {
  return create_memory_trace_adapter(std::move(config.collector));
}

MemoryObservabilityPipeline create_memory_observability_pipeline(
    std::shared_ptr<MemoryObservabilityCollector> collector) {
  if (!collector) {
    collector = std::make_shared<MemoryObservabilityCollector>();
  }
  auto pipeline = create_observability_pipeline(ObservabilityPipelineConfig{
      .loggers = {create_memory_log_adapter(collector)},
      .metrics = {create_memory_metrics_adapter(collector)},
      .traces = {create_memory_trace_adapter(collector)},
  });
  return MemoryObservabilityPipeline{collector, pipeline};
}

MemoryObservabilityPipeline create_memory_observability_pipeline(MemoryAdapterConfig config) {
  return create_memory_observability_pipeline(std::move(config.collector));
}

LogAdapter create_console_log_adapter(std::ostream& info, std::ostream& warn, std::ostream& error) {
  return [&info, &warn, &error](const StructuredLogRecord& record) {
    std::ostream* out = &info;
    if (record.level == StructuredLogLevel::Warn) {
      out = &warn;
    } else if (record.level == StructuredLogLevel::Error) {
      out = &error;
    }
    *out << structured_log_record_to_value(record).stringify(0) << '\n';
  };
}

MetricsAdapter create_console_metrics_adapter(std::ostream& info) {
  return [&info](const MetricRecord& record) {
    info << metric_record_to_value(record).stringify(0) << '\n';
  };
}

TraceAdapter create_console_trace_adapter(std::ostream& info, std::ostream& error) {
  return [&info, &error](const TraceRecord& record) {
    auto& out = record.status == TraceStatus::Error ? error : info;
    out << trace_record_to_value(record).stringify(0) << '\n';
  };
}

std::shared_ptr<ObservabilityPipeline> create_console_observability_pipeline() {
  return create_console_observability_pipeline(std::cout, std::cerr, std::cerr);
}

std::shared_ptr<ObservabilityPipeline> create_console_observability_pipeline(
    std::ostream& info, std::ostream& warn, std::ostream& error) {
  auto pipeline = create_observability_pipeline();
  pipeline->register_logger(create_console_log_adapter(info, warn, error));
  pipeline->register_metrics(create_console_metrics_adapter(info));
  pipeline->register_trace(create_console_trace_adapter(info, error));
  return pipeline;
}

void OpenTelemetryBridge::log(const StructuredLogRecord&) {}

void OpenTelemetryBridge::counter(const std::string&, double, const Value&) {}

void OpenTelemetryBridge::histogram(const std::string&, double, const Value&) {}

std::unique_ptr<OpenTelemetrySpan> OpenTelemetryBridge::start_span(const std::string&, const Value&) {
  return nullptr;
}

LogAdapter create_opentelemetry_log_adapter(OpenTelemetryBridge& bridge) {
  return [&bridge](const StructuredLogRecord& record) {
    bridge.log(record);
  };
}

LogAdapter create_opentelemetry_log_adapter(OpenTelemetryAdapterConfig config) {
  return create_opentelemetry_log_adapter(require_opentelemetry_bridge(config.bridge));
}

MetricsAdapter create_opentelemetry_metrics_adapter(OpenTelemetryBridge& bridge) {
  return [&bridge](const MetricRecord& record) {
    if (record.kind == MetricKind::Counter) {
      bridge.counter(record.name, record.value, record.attributes);
      return;
    }
    bridge.histogram(record.name, record.value, record.attributes);
  };
}

MetricsAdapter create_opentelemetry_metrics_adapter(OpenTelemetryAdapterConfig config) {
  return create_opentelemetry_metrics_adapter(require_opentelemetry_bridge(config.bridge));
}

TraceAdapter create_opentelemetry_trace_adapter(OpenTelemetryBridge& bridge) {
  return [&bridge](const TraceRecord& record) {
    auto span = bridge.start_span(record.name, record.attributes);
    if (span) {
      span->end(record.status);
    }
  };
}

TraceAdapter create_opentelemetry_trace_adapter(OpenTelemetryAdapterConfig config) {
  return create_opentelemetry_trace_adapter(require_opentelemetry_bridge(config.bridge));
}

std::shared_ptr<ObservabilityPipeline> create_opentelemetry_observability_pipeline(OpenTelemetryBridge& bridge) {
  return create_observability_pipeline(ObservabilityPipelineConfig{
      .loggers = {create_opentelemetry_log_adapter(bridge)},
      .metrics = {create_opentelemetry_metrics_adapter(bridge)},
      .traces = {create_opentelemetry_trace_adapter(bridge)},
  });
}

std::shared_ptr<ObservabilityPipeline> create_opentelemetry_observability_pipeline(
    OpenTelemetryAdapterConfig config) {
  return create_opentelemetry_observability_pipeline(require_opentelemetry_bridge(config.bridge));
}

}  // namespace agent
