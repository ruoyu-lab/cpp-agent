#pragma once

#include "agent/agent.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent {

const Value& null_value();
std::string stringify_impl(const Value& value, int indent, int depth);
bool schema_type_matches(JsonSchemaType type, const Value& value);
std::string schema_type_name(JsonSchemaType type);
std::string message_role_label(MessageRole role);
MessageRole message_role_from_label(const std::string& role);
std::string media_source_kind_label(MediaSourceKind kind);
MediaSourceKind media_source_kind_from_label(const std::string& kind);
std::string content_part_type_label(ContentPartType type);
ContentPartType content_part_type_from_label(const std::string& type);
long long time_point_to_ms(std::chrono::system_clock::time_point value);
std::chrono::system_clock::time_point ms_to_time_point(long long value);
EmbeddingVector normalize_vector(EmbeddingVector vector);
double dot_product(const EmbeddingVector& left, const EmbeddingVector& right);
std::uint32_t hash_token(const std::string& token);
std::vector<std::string> tokenize(const std::string& text);
std::string summarize_messages(const std::vector<AgentMessage>& messages);
std::string trim_copy(std::string value);
std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path);
void write_text_file(const std::filesystem::path& path, const std::string& text);
std::string bytes_to_text(const std::vector<std::uint8_t>& bytes);
std::vector<std::uint8_t> text_to_bytes(const std::string& text);
std::vector<std::uint8_t> decode_base64(const std::string& input);
Value parse_json_text(const std::string& text);
std::string html_to_text(std::string html);
std::string path_basename(const std::string& path);
std::string html_escape(const std::string& value);
std::string sanitize_segment(std::string value);
std::string encode_uri_component(const std::string& value);
std::optional<std::string> decode_uri_component(const std::string& value);
Value select_json_path(const Value& value, const std::string& path);

// Math: safe expression evaluator (shunting-yard). No identifiers, no I/O,
// no recursion past parser depth. Throws ConfigurationError on bad input.
double evaluate_math_expression(const std::string& expression);

// Text diff/patch: produce and consume unified diff format (3 lines of context).
std::string compute_unified_diff(const std::string& before, const std::string& after,
                                 const std::string& label_before = "a",
                                 const std::string& label_after = "b");
struct UnifiedPatchResult {
  std::string text;
  std::vector<std::string> conflicts;
  bool applied_cleanly = true;
};
UnifiedPatchResult apply_unified_patch(const std::string& source, const std::string& patch);

// Token-aware output truncation. Keeps the head and tail of large outputs and
// replaces the middle with a marker. Sizes are byte-based (UTF-8 unaware) for
// simplicity and predictability.
struct TruncatedOutput {
  std::string text;
  bool truncated = false;
  std::size_t original_bytes = 0;
  std::size_t kept_bytes = 0;
  std::size_t omitted_bytes = 0;
  std::size_t total_lines = 0;
};

TruncatedOutput truncate_for_model(const std::string& output,
                                   std::size_t max_bytes = 16 * 1024,
                                   std::size_t head_lines = 100,
                                   std::size_t tail_lines = 100);

// Serializes a `TruncatedOutput` into a stable Value shape:
// `{ text, truncated, totalLines, omittedBytes, kept_bytes }` (mixed
// camelCase / snake_case intentional, matches existing serializers).
Value truncated_output_to_value(const TruncatedOutput& output);

}  // namespace agent
