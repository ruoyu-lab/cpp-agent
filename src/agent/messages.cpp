#include "agent/core_api.hpp"
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

ResolvedMedia DefaultMediaResolver::resolve(const MediaSource& source, ArtifactLookup artifact_lookup) const {
  ResolvedMedia resolved;
  resolved.source = source;

  if (source.kind == MediaSourceKind::Inline) {
    resolved.mime_type = source.mime_type;
    resolved.filename = source.filename;
    resolved.bytes = decode_base64(source.data);
    resolved.text = try_decode_text(resolved.bytes, resolved.mime_type);
    return resolved;
  }

  if (source.kind == MediaSourceKind::Path) {
    resolved.bytes = read_binary_file(source.path);
    resolved.mime_type = source.mime_type.empty() ? extension_to_mime(source.path) : source.mime_type;
    if (resolved.mime_type.empty()) {
      resolved.mime_type = "application/octet-stream";
    }
    resolved.filename = source.filename.empty() ? path_basename(source.path) : source.filename;
    resolved.text = try_decode_text(resolved.bytes, resolved.mime_type);
    return resolved;
  }

  if (source.kind == MediaSourceKind::Url) {
    if (source.url.rfind("file://", 0) == 0) {
      MediaSource file_source = source;
      file_source.kind = MediaSourceKind::Path;
      file_source.path = source.url.substr(7);
      return resolve(file_source, std::move(artifact_lookup));
    }
    if (source.url.rfind("data:", 0) == 0) {
      const auto comma = source.url.find(',');
      if (comma == std::string::npos) {
        throw ConfigurationError("Malformed data URL media source.");
      }
      const std::string metadata = source.url.substr(5, comma - 5);
      const std::string payload = source.url.substr(comma + 1);
      resolved.mime_type = source.mime_type.empty() ? metadata.substr(0, metadata.find(';')) : source.mime_type;
      if (resolved.mime_type.empty()) {
        resolved.mime_type = "text/plain";
      }
      resolved.filename = source.filename;
      resolved.bytes = metadata.find(";base64") != std::string::npos ? decode_base64(payload) : text_to_bytes(payload);
      resolved.text = try_decode_text(resolved.bytes, resolved.mime_type);
      return resolved;
    }
    throw ConfigurationError("Native standard-library URL media resolution supports file:// and data: URLs only.");
  }

  if (source.kind == MediaSourceKind::Artifact) {
    if (!artifact_lookup) {
      throw ConfigurationError("Artifact lookup is required to resolve media artifact \"" + source.key + "\".");
    }
    const Value value = artifact_lookup(source.key);
    resolved.mime_type = source.mime_type.empty() ? "text/plain" : source.mime_type;
    resolved.filename = source.filename;
    if (value.is_string()) {
      resolved.text = value.as_string();
      resolved.bytes = text_to_bytes(resolved.text);
      return resolved;
    }
    if (value.is_object()) {
      const auto& object = value.as_object();
      if (object.contains("mimeType")) {
        resolved.mime_type = object.at("mimeType").as_string(resolved.mime_type);
      }
      if (object.contains("filename")) {
        resolved.filename = object.at("filename").as_string(resolved.filename);
      }
      if (object.contains("text")) {
        resolved.text = object.at("text").as_string();
        resolved.bytes = text_to_bytes(resolved.text);
        return resolved;
      }
      if (object.contains("data")) {
        const std::string data = object.at("data").as_string();
        resolved.bytes = object.contains("base64") && object.at("base64").as_bool()
                             ? decode_base64(data)
                             : text_to_bytes(data);
        resolved.text = try_decode_text(resolved.bytes, resolved.mime_type);
        return resolved;
      }
    }
    throw ConfigurationError("Unsupported artifact media payload for key \"" + source.key + "\".");
  }

  throw ConfigurationError("Unsupported media source kind.");
}

std::string DefaultMediaResolver::extension_to_mime(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (ext == ".txt") return "text/plain";
  if (ext == ".md" || ext == ".mdx") return "text/markdown";
  if (ext == ".json") return "application/json";
  if (ext == ".html" || ext == ".htm") return "text/html";
  if (ext == ".csv") return "text/csv";
  if (ext == ".pdf") return "application/pdf";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".gif") return "image/gif";
  if (ext == ".webp") return "image/webp";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".mp4") return "video/mp4";
  if (ext == ".webm") return "video/webm";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".m4v") return "video/x-m4v";
  if (ext == ".mp3") return "audio/mpeg";
  if (ext == ".m4a") return "audio/mp4";
  if (ext == ".wav") return "audio/wav";
  if (ext == ".ogg") return "audio/ogg";
  if (ext == ".aac") return "audio/aac";
  if (ext == ".flac") return "audio/flac";
  return {};
}

std::string DefaultMediaResolver::try_decode_text(const std::vector<std::uint8_t>& bytes,
                                                  const std::string& mime_type) {
  if (mime_type.rfind("text/", 0) != 0 && mime_type != "application/json" &&
      mime_type != "text/markdown") {
    return {};
  }
  return bytes_to_text(bytes);
}

namespace {

Value normalized_metadata(const Value& value) {
  return value.is_object() ? value : Value::object({});
}

bool is_non_empty_object(const Value& value) {
  return value.is_object() && !value.as_object().empty();
}

void put_if_present(Value::Object& object, const std::string& key, const std::string& value) {
  if (!value.empty()) {
    object[key] = value;
  }
}

void put_if_present(Value::Object& object, const std::string& key,
                    const std::optional<double>& value) {
  if (value) {
    object[key] = *value;
  }
}

bool has_media_source_payload(const MediaSource& source) {
  switch (source.kind) {
    case MediaSourceKind::Inline:
      return !source.data.empty();
    case MediaSourceKind::Url:
      return !source.url.empty();
    case MediaSourceKind::Path:
      return !source.path.empty();
    case MediaSourceKind::Artifact:
      return !source.key.empty();
  }
  return false;
}

Value media_source_to_contract_value(const MediaSource& source) {
  Value::Object object;
  object["kind"] = media_source_kind_label(source.kind);
  if (source.kind == MediaSourceKind::Inline) {
    object["data"] = source.data;
  } else if (source.kind == MediaSourceKind::Url) {
    object["url"] = source.url;
  } else if (source.kind == MediaSourceKind::Path) {
    object["path"] = source.path;
  } else if (source.kind == MediaSourceKind::Artifact) {
    object["key"] = source.key;
  }
  put_if_present(object, "mimeType", source.mime_type);
  put_if_present(object, "filename", source.filename);
  return Value(std::move(object));
}

Value content_part_to_contract_value(const MessageContentPart& part) {
  Value::Object object;
  object["type"] = content_part_type_label(part.type);
  if (part.type == ContentPartType::Text) {
    object["text"] = part.text;
  } else if (part.type == ContentPartType::Image) {
    object["source"] = media_source_to_contract_value(part.source);
    put_if_present(object, "altText", part.alt_text);
    put_if_present(object, "detail", part.detail);
  } else if (part.type == ContentPartType::File) {
    object["source"] = media_source_to_contract_value(part.source);
    put_if_present(object, "title", part.title);
    put_if_present(object, "textHint", part.text_hint);
  } else if (part.type == ContentPartType::Audio) {
    object["source"] = media_source_to_contract_value(part.source);
    put_if_present(object, "title", part.title);
    put_if_present(object, "transcriptHint", part.transcript_hint);
  } else if (part.type == ContentPartType::Video) {
    object["source"] = media_source_to_contract_value(part.source);
    put_if_present(object, "title", part.title);
    put_if_present(object, "textHint", part.text_hint);
    put_if_present(object, "transcriptHint", part.transcript_hint);
    put_if_present(object, "startTimeSeconds", part.start_time_seconds);
    put_if_present(object, "endTimeSeconds", part.end_time_seconds);
    put_if_present(object, "frameRateHint", part.frame_rate_hint);
  }
  if (is_non_empty_object(part.metadata)) {
    object["metadata"] = part.metadata;
  }
  return Value(std::move(object));
}

MediaSource normalize_media_source_from_value(const Value& value) {
  if (!value.is_object()) {
    throw ConfigurationError("Media source must be an object.");
  }

  MediaSource source;
  const auto kind = value.at("kind").as_string();
  if (kind == "inline") {
    source.kind = MediaSourceKind::Inline;
    source.data = value.at("data").as_string();
    source.mime_type = value.at("mimeType").as_string();
    source.filename = value.at("filename").as_string();
    if (source.data.empty() || source.mime_type.empty()) {
      throw ConfigurationError("Inline media source requires data and mimeType.");
    }
    return source;
  }
  if (kind == "url") {
    source.kind = MediaSourceKind::Url;
    source.url = value.at("url").as_string();
    source.mime_type = value.at("mimeType").as_string();
    source.filename = value.at("filename").as_string();
    if (source.url.empty()) {
      throw ConfigurationError("URL media source requires url.");
    }
    return source;
  }
  if (kind == "path") {
    source.kind = MediaSourceKind::Path;
    source.path = value.at("path").as_string();
    source.mime_type = value.at("mimeType").as_string();
    source.filename = value.at("filename").as_string();
    if (source.path.empty()) {
      throw ConfigurationError("Path media source requires path.");
    }
    return source;
  }
  if (kind == "artifact") {
    source.kind = MediaSourceKind::Artifact;
    source.key = value.at("key").as_string();
    source.mime_type = value.at("mimeType").as_string();
    source.filename = value.at("filename").as_string();
    if (source.key.empty()) {
      throw ConfigurationError("Artifact media source requires key.");
    }
    return source;
  }

  throw ConfigurationError("Unsupported media source kind: " + kind);
}

MessageContentPart normalize_content_part_from_value(const Value& value) {
  if (!value.is_object()) {
    throw ConfigurationError("Content part must be an object.");
  }

  const auto type = value.at("type").as_string();
  if (type == "text") {
    return text_part(value.at("text").as_string(), normalized_metadata(value.at("metadata")));
  }
  if (type == "image") {
    const auto detail = value.at("detail").as_string();
    return image_part(
        normalize_media_source_from_value(value.at("source")),
        value.at("altText").as_string(),
        (detail == "low" || detail == "high" || detail == "auto") ? detail : "",
        normalized_metadata(value.at("metadata")));
  }
  if (type == "file") {
    return file_part(
        normalize_media_source_from_value(value.at("source")),
        value.at("title").as_string(),
        value.at("textHint").as_string(),
        normalized_metadata(value.at("metadata")));
  }
  if (type == "audio") {
    return audio_part(
        normalize_media_source_from_value(value.at("source")),
        value.at("title").as_string(),
        value.at("transcriptHint").as_string(),
        normalized_metadata(value.at("metadata")));
  }
  if (type == "video") {
    const auto start_time = value.at("startTimeSeconds");
    const auto end_time = value.at("endTimeSeconds");
    const auto frame_rate = value.at("frameRateHint");
    return video_part(
        normalize_media_source_from_value(value.at("source")),
        value.at("title").as_string(),
        value.at("textHint").as_string(),
        value.at("transcriptHint").as_string(),
        start_time.is_number() ? std::optional<double>(start_time.as_number()) : std::nullopt,
        end_time.is_number() ? std::optional<double>(end_time.as_number()) : std::nullopt,
        frame_rate.is_number() ? std::optional<double>(frame_rate.as_number()) : std::nullopt,
        normalized_metadata(value.at("metadata")));
  }

  throw ConfigurationError("Unsupported content part type: " + type);
}

std::vector<MessageContentPart> normalize_message_content_from_value(const Value& value) {
  if (value.is_null()) {
    return {};
  }
  if (value.is_string()) {
    const auto text = value.as_string();
    return text.empty() ? std::vector<MessageContentPart>{}
                        : std::vector<MessageContentPart>{text_part(text)};
  }
  if (value.is_array()) {
    std::vector<MessageContentPart> content;
    for (const auto& item : value.as_array()) {
      content.push_back(normalize_content_part_from_value(item));
    }
    return content;
  }
  if (value.is_object()) {
    return {normalize_content_part_from_value(value)};
  }

  throw ConfigurationError("Content part must be an object.");
}

Value normalize_tool_arguments_from_value(const Value& value) {
  if (value.is_object()) {
    return value;
  }
  if (value.is_string()) {
    const auto text = value.as_string();
    try {
      auto parsed = parse_json(text);
      return parsed.is_object() ? parsed : Value::object({{"value", parsed}});
    } catch (...) {
      return Value::object({{"raw", text}});
    }
  }
  return Value::object({});
}

ToolCall normalize_tool_call_from_value(const Value& value, std::size_t index) {
  if (!value.is_object()) {
    throw ConfigurationError("Tool call must be an object.");
  }
  ToolCall tool_call;
  tool_call.id = value.at("id").as_string("tool_call_" + std::to_string(index + 1));
  tool_call.name = value.at("name").as_string();
  tool_call.arguments = normalize_tool_arguments_from_value(value.at("arguments"));
  return tool_call;
}

MessageRole normalize_message_role_from_value(const Value& value) {
  const auto role = value.as_string();
  if (role == "system") return MessageRole::System;
  if (role == "user") return MessageRole::User;
  if (role == "assistant") return MessageRole::Assistant;
  if (role == "tool") return MessageRole::Tool;
  throw ConfigurationError("Unsupported message role: " + role);
}

}  // namespace

MessageContentPart text_part(std::string text, Value metadata) {
  MessageContentPart part;
  part.type = ContentPartType::Text;
  part.text = std::move(text);
  part.metadata = std::move(metadata);
  return part;
}

MessageContentPart image_part(MediaSource source, std::string alt_text, std::string detail, Value metadata) {
  MessageContentPart part;
  part.type = ContentPartType::Image;
  part.source = std::move(source);
  part.alt_text = std::move(alt_text);
  part.detail = std::move(detail);
  part.metadata = std::move(metadata);
  return part;
}

MessageContentPart file_part(MediaSource source, std::string title, std::string text_hint, Value metadata) {
  MessageContentPart part;
  part.type = ContentPartType::File;
  part.source = std::move(source);
  part.title = std::move(title);
  part.text_hint = std::move(text_hint);
  part.metadata = std::move(metadata);
  return part;
}

MessageContentPart audio_part(MediaSource source, std::string title, std::string transcript_hint,
                              Value metadata) {
  MessageContentPart part;
  part.type = ContentPartType::Audio;
  part.source = std::move(source);
  part.title = std::move(title);
  part.transcript_hint = std::move(transcript_hint);
  part.metadata = std::move(metadata);
  return part;
}

MessageContentPart video_part(MediaSource source, std::string title, std::string text_hint,
                              std::string transcript_hint,
                              std::optional<double> start_time_seconds,
                              std::optional<double> end_time_seconds,
                              std::optional<double> frame_rate_hint,
                              Value metadata) {
  MessageContentPart part;
  part.type = ContentPartType::Video;
  part.source = std::move(source);
  part.title = std::move(title);
  part.text_hint = std::move(text_hint);
  part.transcript_hint = std::move(transcript_hint);
  part.start_time_seconds = start_time_seconds;
  part.end_time_seconds = end_time_seconds;
  part.frame_rate_hint = frame_rate_hint;
  part.metadata = std::move(metadata);
  return part;
}

AgentMessage create_message(MessageRole role, std::string content, Value metadata) {
  return create_message(role, std::vector<MessageContentPart>{text_part(std::move(content))}, std::move(metadata));
}

AgentMessage create_message(MessageRole role, std::vector<MessageContentPart> content, Value metadata) {
  AgentMessage message;
  message.role = role;
  message.content = std::move(content);
  message.metadata = std::move(metadata);
  if (role == MessageRole::Tool && message.name.empty()) {
    throw ConfigurationError("Tool messages require a name.");
  }
  return message;
}

AgentMessage create_tool_result_message(std::string tool_call_id, std::string name, std::string content,
                                        Value metadata) {
  AgentMessage message;
  message.role = MessageRole::Tool;
  message.name = std::move(name);
  message.tool_call_id = std::move(tool_call_id);
  message.content = {text_part(std::move(content))};
  message.metadata = std::move(metadata);
  return message;
}

std::string extract_text_content(const std::vector<MessageContentPart>& content) {
  std::string text;
  for (const auto& part : content) {
    if (part.type == ContentPartType::Text) {
      text += part.text;
    }
  }
  return text;
}

Value agent_message_to_value(const AgentMessage& message) {
  Value::Array content;
  for (const auto& part : message.content) {
    content.push_back(content_part_to_contract_value(part));
  }
  Value::Array tool_calls;
  for (const auto& tool_call : message.tool_calls) {
    tool_calls.push_back(Value::object({{"id", tool_call.id},
                                       {"name", tool_call.name},
                                       {"arguments", tool_call.arguments}}));
  }
  return Value::object({{"role", message_role_label(message.role)},
                        {"content", Value(content)},
                        {"name", message.name.empty() ? Value() : Value(message.name)},
                        {"toolCallId", message.tool_call_id.empty() ? Value() : Value(message.tool_call_id)},
                        {"toolCalls", Value(tool_calls)},
                        {"metadata", message.metadata}});
}

AgentMessage agent_message_from_value(const Value& value) {
  if (!value.is_object()) {
    throw ConfigurationError("Message must be an object.");
  }
  AgentMessage message;
  message.role = normalize_message_role_from_value(value.at("role"));
  message.name = value.at("name").as_string();
  message.tool_call_id = value.at("toolCallId").as_string();
  message.metadata = normalized_metadata(value.at("metadata"));
  message.content = normalize_message_content_from_value(value.at("content"));
  const auto& tool_calls = value.at("toolCalls");
  if (!tool_calls.is_null() && !tool_calls.is_array()) {
    throw ConfigurationError("toolCalls must be an array.");
  }
  std::size_t index = 0;
  for (const auto& item : tool_calls.as_array()) {
    message.tool_calls.push_back(normalize_tool_call_from_value(item, index++));
  }
  if (message.role == MessageRole::Tool && message.name.empty()) {
    throw ConfigurationError("Tool messages require a name.");
  }
  return message;
}

AgentMessage assistant_message_from_output(const AgentOutput& response) {
  AgentMessage message;
  message.role = MessageRole::Assistant;
  message.content = response.content.empty() && !response.text.empty()
                        ? std::vector<MessageContentPart>{text_part(response.text)}
                        : response.content;
  message.tool_calls = response.tool_calls;
  message.metadata = Value::object({
      {"provider", response.provider.empty() ? Value() : Value(response.provider)},
      {"model", response.model.empty() ? Value() : Value(response.model)},
      {"finishReason", response.finish_reason.empty() ? Value() : Value(response.finish_reason)},
      {"reasoning", response.reasoning ? Value::object({{"text", response.reasoning->text},
                                                        {"format", response.reasoning->format}})
                                      : Value()},
  });
  return message;
}

Value agent_artifact_to_value(const AgentArtifact& artifact, bool include_inline_data) {
  MediaSource source = artifact.source;
  if (!include_inline_data && source.kind == MediaSourceKind::Inline) {
    source.data.clear();
  }
  Value::Object object{
      {"id", artifact.id.empty() ? Value() : Value(artifact.id)},
      {"kind", artifact.kind},
      {"source", has_media_source_payload(source) ? media_source_to_contract_value(source) : Value()},
      {"mimeType", artifact.mime_type.empty() ? Value() : Value(artifact.mime_type)},
      {"filename", artifact.filename.empty() ? Value() : Value(artifact.filename)},
      {"byteLength", static_cast<long long>(artifact.byte_length ? artifact.byte_length : artifact.bytes.size())},
      {"providerAssetId", artifact.provider_asset_id.empty() ? Value() : Value(artifact.provider_asset_id)},
      {"metadata", artifact.metadata},
  };
  if (include_inline_data && !artifact.bytes.empty()) {
    object["bytes"] = base64_encode(artifact.bytes);
    object["encoding"] = "base64";
  }
  return Value(std::move(object));
}

Value agent_output_to_value(const AgentOutput& output, bool include_inline_data) {
  Value::Array content;
  content.reserve(output.content.size());
  for (const auto& part : output.content) {
    content.push_back(content_part_to_contract_value(part));
  }
  Value::Array tool_calls;
  tool_calls.reserve(output.tool_calls.size());
  for (const auto& tool_call : output.tool_calls) {
    tool_calls.push_back(Value::object({
        {"id", tool_call.id},
        {"name", tool_call.name},
        {"arguments", tool_call.arguments},
    }));
  }
  Value::Array artifacts;
  artifacts.reserve(output.artifacts.size());
  for (const auto& artifact : output.artifacts) {
    artifacts.push_back(agent_artifact_to_value(artifact, include_inline_data));
  }
  return Value::object({
      {"id", output.id.empty() ? Value() : Value(output.id)},
      {"provider", output.provider},
      {"model", output.model},
      {"content", Value(std::move(content))},
      {"text", output.text},
      {"reasoning", output.reasoning ? Value::object({{"text", output.reasoning->text},
                                                      {"format", output.reasoning->format}})
                                    : Value()},
      {"toolCalls", Value(std::move(tool_calls))},
      {"finishReason", output.finish_reason},
      {"artifacts", Value(std::move(artifacts))},
      {"usage", output.usage},
      {"metadata", output.metadata},
      {"raw", output.raw},
  });
}

AgentOutput agent_output_from_value(const Value& value) {
  AgentOutput output;
  if (!value.is_object()) {
    return output;
  }
  output.id = value.at("id").as_string();
  output.provider = value.at("provider").as_string();
  output.model = value.at("model").as_string();
  output.content = normalize_message_content_from_value(value.at("content"));
  output.text = value.at("text").as_string();
  if (value.at("reasoning").is_object()) {
    output.reasoning = ModelReasoning{
        value.at("reasoning").at("text").as_string(),
        value.at("reasoning").at("format").as_string("summary"),
    };
  }
  const auto& tool_calls = value.at("toolCalls");
  if (tool_calls.is_array()) {
    std::size_t index = 0;
    for (const auto& item : tool_calls.as_array()) {
      output.tool_calls.push_back(normalize_tool_call_from_value(item, index++));
    }
  }
  output.finish_reason = value.at("finishReason").as_string("stop");
  const auto& artifacts = value.at("artifacts");
  if (artifacts.is_array()) {
    for (const auto& item : artifacts.as_array()) {
      if (!item.is_object()) {
        continue;
      }
      AgentArtifact artifact;
      artifact.id = item.at("id").as_string();
      artifact.kind = item.at("kind").as_string();
      if (item.at("source").is_object()) {
        artifact.source = normalize_media_source_from_value(item.at("source"));
      }
      artifact.mime_type = item.at("mimeType").as_string();
      artifact.filename = item.at("filename").as_string();
      if (item.at("byteLength").is_number()) {
        artifact.byte_length = static_cast<std::size_t>(std::max<long long>(0, item.at("byteLength").as_integer()));
      }
      if (item.at("bytes").is_string() && item.at("encoding").as_string() == "base64") {
        artifact.bytes = decode_base64(item.at("bytes").as_string());
        artifact.byte_length = artifact.bytes.size();
      }
      artifact.provider_asset_id = item.at("providerAssetId").as_string();
      artifact.metadata = normalized_metadata(item.at("metadata"));
      output.artifacts.push_back(std::move(artifact));
    }
  }
  output.usage = value.at("usage");
  output.metadata = normalized_metadata(value.at("metadata"));
  output.raw = value.at("raw");
  return output;
}

std::string normalize_finish_reason(std::string raw) {
  std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  if (raw.empty() || raw == "stop" || raw == "end_turn" || raw == "stop_sequence") {
    return "stop";
  }
  if (raw == "length" || raw == "max_tokens" || raw == "max_output_tokens") {
    return "length";
  }
  if (raw == "tool_calls" || raw == "tool_use" || raw == "function_call") {
    return "tool_calls";
  }
  if (raw == "content_filter") {
    return "content_filter";
  }
  if (raw == "safety") {
    return "safety";
  }
  if (raw == "recitation") {
    return "recitation";
  }
  return "other";
}

bool is_incomplete_finish_reason(const std::string& value) {
  const auto normalized = normalize_finish_reason(value);
  return normalized == "length" || normalized == "content_filter" || normalized == "safety" ||
         normalized == "recitation";
}
}  // namespace agent
