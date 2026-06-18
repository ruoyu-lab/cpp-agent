#pragma once

#include "agent/core.hpp"

namespace agent {

enum class MessageRole {
  System,
  User,
  Assistant,
  Tool,
};

enum class MediaSourceKind {
  Inline,
  Url,
  Path,
  Artifact,
};

struct MediaSource {
  MediaSourceKind kind = MediaSourceKind::Inline;
  std::string data;
  std::string url;
  std::string path;
  std::string key;
  std::string mime_type;
  std::string filename;
};

struct ResolvedMedia {
  MediaSource source;
  std::string mime_type;
  std::string filename;
  std::vector<std::uint8_t> bytes;
  std::string text;
};

class DefaultMediaResolver {
 public:
  using ArtifactLookup = std::function<Value(const std::string& key)>;

  ResolvedMedia resolve(const MediaSource& source, ArtifactLookup artifact_lookup = {}) const;
  [[nodiscard]] static std::string extension_to_mime(const std::filesystem::path& path);
  [[nodiscard]] static std::string try_decode_text(const std::vector<std::uint8_t>& bytes,
                                                   const std::string& mime_type);
};

enum class ContentPartType {
  Text,
  Image,
  File,
  Audio,
  Video,
};

struct MessageContentPart {
  ContentPartType type = ContentPartType::Text;
  std::string text;
  MediaSource source;
  std::string alt_text;
  std::string detail;
  std::string title;
  std::string text_hint;
  std::string transcript_hint;
  std::optional<double> start_time_seconds;
  std::optional<double> end_time_seconds;
  std::optional<double> frame_rate_hint;
  Value metadata = Value::object({});
};

struct ToolCall {
  std::string id;
  std::string name;
  Value arguments = Value::object({});
};

struct ModelReasoning {
  std::string text;
  std::string format = "summary";
};

struct AgentArtifact {
  std::string id;
  std::string kind;
  MediaSource source;
  std::string mime_type;
  std::string filename;
  std::vector<std::uint8_t> bytes;
  std::size_t byte_length = 0;
  std::string provider_asset_id;
  Value metadata = Value::object({});
};

struct AgentOutput {
  std::string id;
  std::string provider;
  std::string model;
  std::vector<MessageContentPart> content;
  std::string text;
  std::optional<ModelReasoning> reasoning;
  std::vector<ToolCall> tool_calls;
  std::string finish_reason = "stop";
  std::vector<AgentArtifact> artifacts;
  Value usage = Value::object({});
  Value metadata = Value::object({});
  Value raw;
};

struct AgentMessage {
  MessageRole role = MessageRole::User;
  std::vector<MessageContentPart> content;
  std::string name;
  std::string tool_call_id;
  std::vector<ToolCall> tool_calls;
  Value metadata = Value::object({});
};

struct ChatToolDescriptor {
  std::string name;
  std::string description;
  JsonSchema input_schema;
  bool read_only = false;
  bool mutates_files = false;
  bool interactive = false;
  bool long_running = false;
  bool batchable = false;
  std::string concurrency_key;
  std::string side_effect_level = "unknown";
};

MessageContentPart text_part(std::string text, Value metadata = Value::object({}));
MessageContentPart image_part(MediaSource source, std::string alt_text = {}, std::string detail = {},
                              Value metadata = Value::object({}));
MessageContentPart file_part(MediaSource source, std::string title = {}, std::string text_hint = {},
                             Value metadata = Value::object({}));
MessageContentPart audio_part(MediaSource source, std::string title = {},
                              std::string transcript_hint = {},
                              Value metadata = Value::object({}));
MessageContentPart video_part(MediaSource source, std::string title = {}, std::string text_hint = {},
                              std::string transcript_hint = {},
                              std::optional<double> start_time_seconds = std::nullopt,
                              std::optional<double> end_time_seconds = std::nullopt,
                              std::optional<double> frame_rate_hint = std::nullopt,
                              Value metadata = Value::object({}));
AgentMessage create_message(MessageRole role, std::string content, Value metadata = Value::object({}));
AgentMessage create_message(MessageRole role, std::vector<MessageContentPart> content,
                            Value metadata = Value::object({}));
AgentMessage create_tool_result_message(std::string tool_call_id, std::string name,
                                        std::string content, Value metadata = Value::object({}));
std::string extract_text_content(const std::vector<MessageContentPart>& content);
Value agent_message_to_value(const AgentMessage& message);
AgentMessage agent_message_from_value(const Value& value);
AgentMessage assistant_message_from_output(const AgentOutput& output);
Value agent_artifact_to_value(const AgentArtifact& artifact, bool include_inline_data = false);
Value agent_output_to_value(const AgentOutput& output, bool include_inline_data = false);
AgentOutput agent_output_from_value(const Value& value);

}  // namespace agent
