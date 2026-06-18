#pragma once

#include "agent/tool_runs.hpp"
#include "agent/tools.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

namespace agent {

enum class RealtimeSessionState {
  Idle,
  Connecting,
  Open,
  Listening,
  Responding,
  ToolWaiting,
  Interrupted,
  Closing,
  Closed,
  Failed,
};

enum class RealtimeSessionEventType {
  SessionOpened,
  SessionUpdated,
  SessionClosed,
  SessionError,
  InputText,
  InputAudioDelta,
  InputAudioCommitted,
  InputTranscriptDelta,
  InputTranscriptDone,
  ResponseStarted,
  ResponseDone,
  ResponseInterrupted,
  OutputTextDelta,
  OutputTextDone,
  OutputAudioDelta,
  OutputAudioDone,
  OutputTranscriptDelta,
  OutputTranscriptDone,
  ToolCallStarted,
  ToolCallArgumentsDelta,
  ToolCallReady,
  ToolCallResult,
  ToolCallError,
  Custom,
};

std::string to_string(RealtimeSessionState state);
RealtimeSessionState realtime_session_state_from_string(
    const std::string& value,
    RealtimeSessionState fallback = RealtimeSessionState::Idle);
std::string to_string(RealtimeSessionEventType type);
RealtimeSessionEventType realtime_session_event_type_from_string(
    const std::string& value,
    RealtimeSessionEventType fallback = RealtimeSessionEventType::Custom);

struct RealtimeAudioChunk {
  std::string data;
  std::string encoding;
  std::string mime_type;
  int sample_rate = 0;
  int channels = 0;
  std::uint64_t sequence = 0;
  Value metadata = Value::object({});
};

struct RealtimeToolCall {
  std::string id;
  std::string name;
  Value arguments = Value::object({});
  std::string arguments_text;
  Value metadata = Value::object({});
};

struct RealtimeToolResult {
  std::string tool_call_id;
  std::string name;
  bool ok = false;
  Value output;
  std::string error;
  Value metadata = Value::object({});
};

struct RealtimeSessionEvent {
  std::string id;
  std::string session_id;
  std::uint64_t sequence = 0;
  std::string timestamp;
  RealtimeSessionEventType type = RealtimeSessionEventType::Custom;
  std::string custom_type;
  std::string turn_id;
  std::string response_id;
  std::string item_id;
  std::string tool_call_id;
  std::optional<RealtimeSessionState> state;
  std::string provider;
  std::string model;
  std::string text;
  std::string delta;
  std::optional<RealtimeAudioChunk> audio;
  std::optional<RealtimeToolCall> tool_call;
  std::optional<RealtimeToolResult> tool_result;
  std::string error;
  Value trace_context = Value::object({});
  Value metadata = Value::object({});
};

struct RealtimeSessionConfig {
  std::string session_id;
  std::string provider;
  std::string model;
  Value settings = Value::object({});
  Value metadata = Value::object({});
  CancellationToken* cancellation = nullptr;
};

struct RealtimeProviderCapabilities {
  std::string provider;
  std::vector<std::string> models;
  std::vector<std::string> modalities;
  bool input_audio = false;
  bool output_audio = false;
  bool interruption = false;
  bool tool_calling = false;
  Value settings = Value::object({});
};

Value realtime_audio_chunk_to_value(const RealtimeAudioChunk& chunk);
Value realtime_tool_call_to_value(const RealtimeToolCall& call);
Value realtime_tool_result_to_value(const RealtimeToolResult& result);
Value realtime_session_event_to_value(const RealtimeSessionEvent& event);
Value realtime_provider_capabilities_to_value(const RealtimeProviderCapabilities& capabilities);

class RealtimeSession {
 public:
  virtual ~RealtimeSession() = default;

  [[nodiscard]] virtual std::string id() const = 0;
  [[nodiscard]] virtual RealtimeSessionState state() const = 0;
  virtual void send_text(std::string text, Value metadata = Value::object({})) = 0;
  virtual void send_audio(RealtimeAudioChunk chunk) = 0;
  virtual void commit_input_audio(Value metadata = Value::object({})) = 0;
  virtual void clear_input_audio(Value metadata = Value::object({})) = 0;
  virtual void request_response(Value metadata = Value::object({})) = 0;
  virtual void cancel_response(std::string reason = {}) = 0;
  virtual void send_tool_result(RealtimeToolResult result) = 0;
  virtual void update_settings(Value settings) = 0;
  virtual void interrupt(std::string reason = {}) = 0;
  virtual void close(std::string reason = {}) = 0;
  virtual bool next_event(RealtimeSessionEvent& out_event) = 0;
};

class RealtimeSessionProvider {
 public:
  virtual ~RealtimeSessionProvider() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual RealtimeProviderCapabilities capabilities() const {
    return RealtimeProviderCapabilities{.provider = name()};
  }
  virtual std::unique_ptr<RealtimeSession> open_session(RealtimeSessionConfig config) = 0;
};

struct RealtimeToolBridgeOptions {
  ToolExecutor* executor = nullptr;
  ToolExecutionContext context;
};

class RealtimeToolBridge {
 public:
  explicit RealtimeToolBridge(RealtimeToolBridgeOptions options);
  RealtimeToolResult execute(const RealtimeToolCall& call);
  bool execute_event(const RealtimeSessionEvent& event, RealtimeToolResult& out_result);

 private:
  ToolExecutor* executor_;
  ToolExecutionContext context_;
  std::map<std::string, std::string> argument_buffers_;
};

}  // namespace agent
