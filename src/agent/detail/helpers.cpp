#include "helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>

namespace agent {

const Value& null_value() {
  static const Value value;
  return value;
}

std::string escape_json(const std::string& value) {
  std::ostringstream out;
  for (const unsigned char ch : value) {
    switch (ch) {
      case '"':
        out << "\\\"";
        break;
      case '\\':
        out << "\\\\";
        break;
      case '\b':
        out << "\\b";
        break;
      case '\f':
        out << "\\f";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
        } else {
          out << static_cast<char>(ch);
        }
    }
  }
  return out.str();
}

std::string indent_string(int count) {
  return std::string(std::max(0, count), ' ');
}

std::string stringify_impl(const Value& value, int indent, int depth) {
  if (value.is_null()) {
    return "null";
  }
  if (value.is_bool()) {
    return value.as_bool() ? "true" : "false";
  }
  if (value.is_number()) {
    std::ostringstream out;
    out << std::setprecision(15) << value.as_number();
    return out.str();
  }
  if (value.is_string()) {
    return "\"" + escape_json(value.as_string()) + "\"";
  }
  if (value.is_array()) {
    const auto& items = value.as_array();
    if (items.empty()) {
      return "[]";
    }
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < items.size(); ++index) {
      if (index > 0) {
        out << ",";
      }
      if (indent > 0) {
        out << "\n" << indent_string(depth + indent);
      }
      out << stringify_impl(items[index], indent, depth + indent);
    }
    if (indent > 0) {
      out << "\n" << indent_string(depth);
    }
    out << "]";
    return out.str();
  }

  const auto& object = value.as_object();
  if (object.empty()) {
    return "{}";
  }
  std::ostringstream out;
  out << "{";
  std::size_t index = 0;
  for (const auto& [key, child] : object) {
    if (index++ > 0) {
      out << ",";
    }
    if (indent > 0) {
      out << "\n" << indent_string(depth + indent);
    }
    out << "\"" << escape_json(key) << "\":";
    if (indent > 0) {
      out << " ";
    }
    out << stringify_impl(child, indent, depth + indent);
  }
  if (indent > 0) {
    out << "\n" << indent_string(depth);
  }
  out << "}";
  return out.str();
}

bool schema_type_matches(JsonSchemaType type, const Value& value) {
  switch (type) {
    case JsonSchemaType::String:
      return value.is_string();
    case JsonSchemaType::Number:
      return value.is_number();
    case JsonSchemaType::Integer:
      return value.is_number() && std::floor(value.as_number()) == value.as_number();
    case JsonSchemaType::Boolean:
      return value.is_bool();
    case JsonSchemaType::Array:
      return value.is_array();
    case JsonSchemaType::Object:
      return value.is_object();
    case JsonSchemaType::Null:
      return value.is_null();
  }
  return false;
}

std::string schema_type_name(JsonSchemaType type) {
  switch (type) {
    case JsonSchemaType::String:
      return "string";
    case JsonSchemaType::Number:
      return "number";
    case JsonSchemaType::Integer:
      return "integer";
    case JsonSchemaType::Boolean:
      return "boolean";
    case JsonSchemaType::Array:
      return "array";
    case JsonSchemaType::Object:
      return "object";
    case JsonSchemaType::Null:
      return "null";
  }
  return "unknown";
}

std::string message_role_label(MessageRole role) {
  switch (role) {
    case MessageRole::System:
      return "system";
    case MessageRole::User:
      return "user";
    case MessageRole::Assistant:
      return "assistant";
    case MessageRole::Tool:
      return "tool";
  }
  return "user";
}

MessageRole message_role_from_label(const std::string& role) {
  if (role == "system") return MessageRole::System;
  if (role == "assistant") return MessageRole::Assistant;
  if (role == "tool") return MessageRole::Tool;
  return MessageRole::User;
}

std::string media_source_kind_label(MediaSourceKind kind) {
  switch (kind) {
    case MediaSourceKind::Inline: return "inline";
    case MediaSourceKind::Url: return "url";
    case MediaSourceKind::Path: return "path";
    case MediaSourceKind::Artifact: return "artifact";
  }
  return "inline";
}

MediaSourceKind media_source_kind_from_label(const std::string& kind) {
  if (kind == "url") return MediaSourceKind::Url;
  if (kind == "path") return MediaSourceKind::Path;
  if (kind == "artifact") return MediaSourceKind::Artifact;
  return MediaSourceKind::Inline;
}

std::string content_part_type_label(ContentPartType type) {
  switch (type) {
    case ContentPartType::Text: return "text";
    case ContentPartType::Image: return "image";
    case ContentPartType::File: return "file";
  }
  return "text";
}

ContentPartType content_part_type_from_label(const std::string& type) {
  if (type == "image") return ContentPartType::Image;
  if (type == "file") return ContentPartType::File;
  return ContentPartType::Text;
}

long long time_point_to_ms(std::chrono::system_clock::time_point value) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(value.time_since_epoch()).count();
}

std::chrono::system_clock::time_point ms_to_time_point(long long value) {
  return std::chrono::system_clock::time_point(std::chrono::milliseconds(value));
}

EmbeddingVector normalize_vector(EmbeddingVector vector) {
  double magnitude = 0;
  for (const double value : vector) {
    magnitude += value * value;
  }
  magnitude = std::sqrt(magnitude);
  if (magnitude <= std::numeric_limits<double>::epsilon()) {
    return vector;
  }
  for (double& value : vector) {
    value /= magnitude;
  }
  return vector;
}

double dot_product(const EmbeddingVector& left, const EmbeddingVector& right) {
  const std::size_t length = std::min(left.size(), right.size());
  double total = 0;
  for (std::size_t index = 0; index < length; ++index) {
    total += left[index] * right[index];
  }
  return total;
}

std::uint32_t hash_token(const std::string& token) {
  std::uint32_t hash = 2166136261u;
  for (const unsigned char ch : token) {
    hash ^= ch;
    hash *= 16777619u;
  }
  return hash;
}

std::vector<std::string> tokenize(const std::string& text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const unsigned char ch : text) {
    const bool token_char = std::isalnum(ch) || ch >= 128;
    if (token_char) {
      current.push_back(static_cast<char>(std::tolower(ch)));
    } else if (!current.empty()) {
      tokens.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    tokens.push_back(current);
  }
  return tokens;
}

std::string summarize_messages(const std::vector<AgentMessage>& messages) {
  std::string summary;
  for (const auto& message : messages) {
    summary += "[" + message_role_label(message.role);
    if (!message.name.empty()) {
      summary += ":" + message.name;
    }
    summary += "] " + extract_text_content(message.content) + "\n";
  }
  if (summary.size() > 4000) {
    return summary.substr(summary.size() - 4000);
  }
  return summary;
}

std::string trim_copy(std::string value) {
  const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  });
  const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
    return std::isspace(ch) != 0;
  }).base();
  if (begin >= end) {
    return {};
  }
  return std::string(begin, end);
}

std::vector<std::uint8_t> read_binary_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw ConfigurationError("Unable to read file: " + path.string());
  }
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

void write_text_file(const std::filesystem::path& path, const std::string& text) {
  if (!path.parent_path().empty()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw ConfigurationError("Unable to write file: " + path.string());
  }
  output << text;
}

std::string bytes_to_text(const std::vector<std::uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> text_to_bytes(const std::string& text) {
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

std::vector<std::uint8_t> decode_base64(const std::string& input) {
  static const std::string chars =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<std::uint8_t> output;
  int value = 0;
  int bits = -8;
  for (const unsigned char ch : input) {
    if (std::isspace(ch)) {
      continue;
    }
    if (ch == '=') {
      break;
    }
    const auto index = chars.find(static_cast<char>(ch));
    if (index == std::string::npos) {
      continue;
    }
    value = (value << 6) + static_cast<int>(index);
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return output;
}

class JsonParser {
 public:
  explicit JsonParser(std::string text) : text_(std::move(text)) {}

  Value parse() {
    skip_ws();
    Value value = parse_value();
    skip_ws();
    if (pos_ != text_.size()) {
      throw ConfigurationError("Unexpected trailing JSON input.");
    }
    return value;
  }

 private:
  Value parse_value() {
    skip_ws();
    if (pos_ >= text_.size()) {
      throw ConfigurationError("Unexpected end of JSON input.");
    }
    const char ch = text_[pos_];
    if (ch == '"') return parse_string();
    if (ch == '{') return parse_object();
    if (ch == '[') return parse_array();
    if (ch == 't') return parse_literal("true", true);
    if (ch == 'f') return parse_literal("false", false);
    if (ch == 'n') return parse_null();
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch))) return parse_number();
    throw ConfigurationError("Invalid JSON value.");
  }

  Value parse_object() {
    consume('{');
    Value::Object object;
    skip_ws();
    if (peek('}')) {
      consume('}');
      return object;
    }
    while (true) {
      skip_ws();
      const std::string key = parse_string().as_string();
      skip_ws();
      consume(':');
      object[key] = parse_value();
      skip_ws();
      if (peek('}')) {
        consume('}');
        break;
      }
      consume(',');
    }
    return object;
  }

  Value parse_array() {
    consume('[');
    Value::Array array;
    skip_ws();
    if (peek(']')) {
      consume(']');
      return array;
    }
    while (true) {
      array.push_back(parse_value());
      skip_ws();
      if (peek(']')) {
        consume(']');
        break;
      }
      consume(',');
    }
    return array;
  }

  Value parse_string() {
    consume('"');
    std::string output;
    while (pos_ < text_.size()) {
      const char ch = text_[pos_++];
      if (ch == '"') {
        return output;
      }
      if (ch != '\\') {
        output.push_back(ch);
        continue;
      }
      if (pos_ >= text_.size()) {
        throw ConfigurationError("Invalid JSON escape.");
      }
      const char escaped = text_[pos_++];
      switch (escaped) {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u': {
          // Standard JSON \uXXXX decode. Reads four hex digits, handles UTF-16
          // surrogate pairs for codepoints above the BMP, and emits UTF-8.
          auto read_hex4 = [&](std::size_t at) -> std::uint32_t {
            if (at + 4 > text_.size()) {
              throw ConfigurationError("Invalid JSON unicode escape.");
            }
            std::uint32_t value = 0;
            for (int i = 0; i < 4; ++i) {
              const char hex = text_[at + i];
              value <<= 4;
              if (hex >= '0' && hex <= '9') {
                value |= static_cast<std::uint32_t>(hex - '0');
              } else if (hex >= 'a' && hex <= 'f') {
                value |= static_cast<std::uint32_t>(hex - 'a' + 10);
              } else if (hex >= 'A' && hex <= 'F') {
                value |= static_cast<std::uint32_t>(hex - 'A' + 10);
              } else {
                throw ConfigurationError("Invalid JSON unicode escape hex.");
              }
            }
            return value;
          };
          std::uint32_t codepoint = read_hex4(pos_);
          pos_ += 4;
          // High surrogate: expect a following \uXXXX low surrogate so we can
          // assemble the supplementary-plane codepoint.
          if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            if (pos_ + 6 <= text_.size() && text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              const std::uint32_t low = read_hex4(pos_ + 2);
              if (low >= 0xDC00u && low <= 0xDFFFu) {
                codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
                pos_ += 6;
              }
            }
          }
          if (codepoint < 0x80u) {
            output.push_back(static_cast<char>(codepoint));
          } else if (codepoint < 0x800u) {
            output.push_back(static_cast<char>(0xC0u | (codepoint >> 6)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
          } else if (codepoint < 0x10000u) {
            output.push_back(static_cast<char>(0xE0u | (codepoint >> 12)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
          } else {
            output.push_back(static_cast<char>(0xF0u | (codepoint >> 18)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu)));
            output.push_back(static_cast<char>(0x80u | (codepoint & 0x3Fu)));
          }
          break;
        }
        default:
          throw ConfigurationError("Invalid JSON escape.");
      }
    }
    throw ConfigurationError("Unterminated JSON string.");
  }

  Value parse_number() {
    const std::size_t start = pos_;
    if (peek('-')) ++pos_;
    while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    if (peek('.')) {
      ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    if (peek('e') || peek('E')) {
      ++pos_;
      if (peek('+') || peek('-')) ++pos_;
      while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) ++pos_;
    }
    return std::stod(text_.substr(start, pos_ - start));
  }

  Value parse_literal(const std::string& literal, bool value) {
    if (text_.substr(pos_, literal.size()) != literal) {
      throw ConfigurationError("Invalid JSON literal.");
    }
    pos_ += literal.size();
    return value;
  }

  Value parse_null() {
    if (text_.substr(pos_, 4) != "null") {
      throw ConfigurationError("Invalid JSON literal.");
    }
    pos_ += 4;
    return nullptr;
  }

  void skip_ws() {
    while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
      ++pos_;
    }
  }

  [[nodiscard]] bool peek(char ch) const {
    return pos_ < text_.size() && text_[pos_] == ch;
  }

  void consume(char ch) {
    skip_ws();
    if (!peek(ch)) {
      throw ConfigurationError(std::string("Expected JSON character: ") + ch);
    }
    ++pos_;
  }

  std::string text_;
  std::size_t pos_ = 0;
};

std::string html_to_text(std::string html) {
  std::string text;
  bool in_tag = false;
  for (const char ch : html) {
    if (ch == '<') {
      in_tag = true;
      text.push_back(' ');
      continue;
    }
    if (ch == '>') {
      in_tag = false;
      continue;
    }
    if (!in_tag) {
      text.push_back(ch);
    }
  }
  return trim_copy(text);
}

std::string path_basename(const std::string& path) {
  return std::filesystem::path(path).filename().string();
}

std::string html_escape(const std::string& value) {
  std::string output;
  for (const char ch : value) {
    switch (ch) {
      case '&':
        output += "&amp;";
        break;
      case '<':
        output += "&lt;";
        break;
      case '>':
        output += "&gt;";
        break;
      case '"':
        output += "&quot;";
        break;
      default:
        output.push_back(ch);
    }
  }
  return output;
}

std::string sanitize_segment(std::string value) {
  for (char& ch : value) {
    const bool safe = std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-';
    if (!safe) {
      ch = '-';
    }
  }
  value = trim_copy(value);
  return value.empty() ? "run" : value;
}

bool is_uri_component_safe(unsigned char ch) {
  return std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '!' || ch == '~' || ch == '*' ||
         ch == '\'' || ch == '(' || ch == ')';
}

std::string encode_uri_component(const std::string& value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char ch : value) {
    if (is_uri_component_safe(ch)) {
      encoded.push_back(static_cast<char>(ch));
      continue;
    }
    encoded.push_back('%');
    encoded.push_back(hex[(ch >> 4) & 0x0f]);
    encoded.push_back(hex[ch & 0x0f]);
  }
  return encoded;
}

std::optional<int> hex_digit_value(char ch) {
  if (ch >= '0' && ch <= '9') {
    return ch - '0';
  }
  if (ch >= 'A' && ch <= 'F') {
    return ch - 'A' + 10;
  }
  if (ch >= 'a' && ch <= 'f') {
    return ch - 'a' + 10;
  }
  return std::nullopt;
}

std::optional<std::string> decode_uri_component(const std::string& value) {
  std::string decoded;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (ch != '%') {
      decoded.push_back(ch);
      continue;
    }
    if (index + 2 >= value.size()) {
      return std::nullopt;
    }
    const auto high = hex_digit_value(value[index + 1]);
    const auto low = hex_digit_value(value[index + 2]);
    if (!high || !low) {
      return std::nullopt;
    }
    decoded.push_back(static_cast<char>((*high << 4) | *low));
    index += 2;
  }
  return decoded;
}

Value select_json_path(const Value& value, const std::string& path) {
  if (path.empty()) {
    return value;
  }
  const Value* current = &value;
  std::size_t start = 0;
  while (start <= path.size()) {
    const std::size_t dot = path.find('.', start);
    const std::string key = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (!key.empty()) {
      if (current->is_object()) {
        if (!current->contains(key)) {
          return {};
        }
        current = &current->at(key);
      } else if (current->is_array() &&
                 std::all_of(key.begin(), key.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        std::size_t index = 0;
        for (const auto ch : key) {
          const auto digit = static_cast<std::size_t>(ch - '0');
          if (index > (std::numeric_limits<std::size_t>::max() - digit) / 10) {
            return {};
          }
          index = index * 10 + digit;
        }
        const auto& array = current->as_array();
        if (index >= array.size()) {
          return {};
        }
        current = &array[index];
      } else {
        return {};
      }
    }
    if (dot == std::string::npos) {
      break;
    }
    start = dot + 1;
  }
  return *current;
}

Value parse_json_text(const std::string& text) {
  return JsonParser(text).parse();
}

// ---------------------------------------------------------------------------
// Math expression evaluator — shunting-yard / RPN.
// ---------------------------------------------------------------------------

namespace {

struct MathToken {
  enum class Kind { Number, Op, LParen, RParen, Comma, Function };
  Kind kind = Kind::Number;
  double number = 0;
  std::string text;
};

int op_precedence(const std::string& op) {
  if (op == "u-" || op == "u+") return 5;
  if (op == "^") return 4;
  if (op == "*" || op == "/" || op == "%") return 3;
  if (op == "+" || op == "-") return 2;
  return 0;
}

bool op_right_assoc(const std::string& op) {
  return op == "^" || op == "u-" || op == "u+";
}

int op_arity(const std::string& op) {
  return (op == "u-" || op == "u+") ? 1 : 2;
}

std::vector<MathToken> tokenize_math(const std::string& expression) {
  std::vector<MathToken> tokens;
  std::size_t pos = 0;
  auto last_was_operand = [&] {
    if (tokens.empty()) return false;
    const auto& last = tokens.back();
    return last.kind == MathToken::Kind::Number || last.kind == MathToken::Kind::RParen;
  };
  while (pos < expression.size()) {
    const char ch = expression[pos];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      ++pos;
      continue;
    }
    if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') {
      std::size_t start = pos;
      while (pos < expression.size() &&
             (std::isdigit(static_cast<unsigned char>(expression[pos])) || expression[pos] == '.')) {
        ++pos;
      }
      if (pos < expression.size() && (expression[pos] == 'e' || expression[pos] == 'E')) {
        ++pos;
        if (pos < expression.size() && (expression[pos] == '+' || expression[pos] == '-')) ++pos;
        while (pos < expression.size() && std::isdigit(static_cast<unsigned char>(expression[pos]))) ++pos;
      }
      MathToken token;
      token.kind = MathToken::Kind::Number;
      try {
        token.number = std::stod(expression.substr(start, pos - start));
      } catch (const std::exception&) {
        throw ConfigurationError("Invalid number in math expression.");
      }
      tokens.push_back(token);
      continue;
    }
    if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
      std::size_t start = pos;
      while (pos < expression.size() &&
             (std::isalnum(static_cast<unsigned char>(expression[pos])) || expression[pos] == '_')) {
        ++pos;
      }
      std::string ident = expression.substr(start, pos - start);
      std::transform(ident.begin(), ident.end(), ident.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (ident == "pi") {
        MathToken token;
        token.kind = MathToken::Kind::Number;
        token.number = 3.141592653589793238462643383279502884;
        tokens.push_back(token);
        continue;
      }
      if (ident == "e") {
        MathToken token;
        token.kind = MathToken::Kind::Number;
        token.number = 2.718281828459045235360287471352662497;
        tokens.push_back(token);
        continue;
      }
      if (ident == "tau") {
        MathToken token;
        token.kind = MathToken::Kind::Number;
        token.number = 6.283185307179586476925286766559005768;
        tokens.push_back(token);
        continue;
      }
      MathToken token;
      token.kind = MathToken::Kind::Function;
      token.text = ident;
      tokens.push_back(token);
      continue;
    }
    if (ch == '(') {
      tokens.push_back(MathToken{.kind = MathToken::Kind::LParen});
      ++pos;
      continue;
    }
    if (ch == ')') {
      tokens.push_back(MathToken{.kind = MathToken::Kind::RParen});
      ++pos;
      continue;
    }
    if (ch == ',') {
      tokens.push_back(MathToken{.kind = MathToken::Kind::Comma});
      ++pos;
      continue;
    }
    if (ch == '+' || ch == '-' || ch == '*' || ch == '/' || ch == '%' || ch == '^') {
      std::string op(1, ch);
      // Detect unary +/-: when at start or after another op/paren/comma.
      if ((ch == '-' || ch == '+') && !last_was_operand()) {
        op = std::string("u") + ch;
      }
      // Handle '**' as alias for '^'.
      if (ch == '*' && pos + 1 < expression.size() && expression[pos + 1] == '*') {
        op = "^";
        ++pos;
      }
      tokens.push_back(MathToken{.kind = MathToken::Kind::Op, .text = op});
      ++pos;
      continue;
    }
    throw ConfigurationError(std::string("Unexpected character in math expression: ") + ch);
  }
  return tokens;
}

double call_math_function(const std::string& name, const std::vector<double>& args) {
  auto need = [&](std::size_t n) {
    if (args.size() != n) {
      throw ConfigurationError("Math function '" + name + "' got wrong arity.");
    }
  };
  if (name == "abs") { need(1); return std::fabs(args[0]); }
  if (name == "sqrt") { need(1); return std::sqrt(args[0]); }
  if (name == "cbrt") { need(1); return std::cbrt(args[0]); }
  if (name == "exp") { need(1); return std::exp(args[0]); }
  if (name == "log" || name == "ln") { need(1); return std::log(args[0]); }
  if (name == "log2") { need(1); return std::log2(args[0]); }
  if (name == "log10") { need(1); return std::log10(args[0]); }
  if (name == "sin") { need(1); return std::sin(args[0]); }
  if (name == "cos") { need(1); return std::cos(args[0]); }
  if (name == "tan") { need(1); return std::tan(args[0]); }
  if (name == "asin") { need(1); return std::asin(args[0]); }
  if (name == "acos") { need(1); return std::acos(args[0]); }
  if (name == "atan") { need(1); return std::atan(args[0]); }
  if (name == "sinh") { need(1); return std::sinh(args[0]); }
  if (name == "cosh") { need(1); return std::cosh(args[0]); }
  if (name == "tanh") { need(1); return std::tanh(args[0]); }
  if (name == "floor") { need(1); return std::floor(args[0]); }
  if (name == "ceil") { need(1); return std::ceil(args[0]); }
  if (name == "round") { need(1); return std::round(args[0]); }
  if (name == "trunc") { need(1); return std::trunc(args[0]); }
  if (name == "sign") { need(1); return (args[0] > 0) - (args[0] < 0); }
  if (name == "pow") { need(2); return std::pow(args[0], args[1]); }
  if (name == "atan2") { need(2); return std::atan2(args[0], args[1]); }
  if (name == "hypot") { need(2); return std::hypot(args[0], args[1]); }
  if (name == "mod") {
    need(2);
    if (args[1] == 0) throw ConfigurationError("mod() second argument must be non-zero.");
    return std::fmod(args[0], args[1]);
  }
  if (name == "min" || name == "max") {
    if (args.empty()) throw ConfigurationError(name + "() requires at least one argument.");
    double acc = args[0];
    for (std::size_t i = 1; i < args.size(); ++i) {
      acc = (name == "min") ? std::min(acc, args[i]) : std::max(acc, args[i]);
    }
    return acc;
  }
  throw ConfigurationError("Unknown math function: " + name);
}

}  // namespace

double evaluate_math_expression(const std::string& expression) {
  const auto tokens = tokenize_math(expression);
  // Shunting-yard → RPN
  std::vector<MathToken> output;
  std::vector<MathToken> stack;
  std::vector<int> func_argc;  // arg counter per function nesting
  for (std::size_t index = 0; index < tokens.size(); ++index) {
    const auto& token = tokens[index];
    switch (token.kind) {
      case MathToken::Kind::Number:
        output.push_back(token);
        if (!func_argc.empty() && func_argc.back() == 0) func_argc.back() = 1;
        break;
      case MathToken::Kind::Function:
        stack.push_back(token);
        break;
      case MathToken::Kind::Comma:
        while (!stack.empty() && stack.back().kind != MathToken::Kind::LParen) {
          output.push_back(stack.back());
          stack.pop_back();
        }
        if (stack.empty()) throw ConfigurationError("Misplaced comma in math expression.");
        if (!func_argc.empty()) ++func_argc.back();
        break;
      case MathToken::Kind::Op:
        while (!stack.empty() && stack.back().kind == MathToken::Kind::Op &&
               ((op_right_assoc(token.text)
                     ? op_precedence(stack.back().text) > op_precedence(token.text)
                     : op_precedence(stack.back().text) >= op_precedence(token.text)))) {
          output.push_back(stack.back());
          stack.pop_back();
        }
        stack.push_back(token);
        break;
      case MathToken::Kind::LParen:
        stack.push_back(token);
        if (!stack.empty() && stack.size() >= 2 &&
            stack[stack.size() - 2].kind == MathToken::Kind::Function) {
          func_argc.push_back(0);
        }
        break;
      case MathToken::Kind::RParen: {
        while (!stack.empty() && stack.back().kind != MathToken::Kind::LParen) {
          output.push_back(stack.back());
          stack.pop_back();
        }
        if (stack.empty()) throw ConfigurationError("Mismatched parentheses in math expression.");
        stack.pop_back();  // pop the LParen
        if (!stack.empty() && stack.back().kind == MathToken::Kind::Function) {
          auto fn = stack.back();
          stack.pop_back();
          int argc = func_argc.empty() ? 1 : func_argc.back();
          if (!func_argc.empty()) func_argc.pop_back();
          MathToken call;
          call.kind = MathToken::Kind::Function;
          call.text = fn.text + "/" + std::to_string(argc);
          output.push_back(call);
        }
        break;
      }
    }
  }
  while (!stack.empty()) {
    if (stack.back().kind == MathToken::Kind::LParen ||
        stack.back().kind == MathToken::Kind::RParen) {
      throw ConfigurationError("Mismatched parentheses in math expression.");
    }
    output.push_back(stack.back());
    stack.pop_back();
  }
  // Evaluate RPN.
  std::vector<double> values;
  values.reserve(output.size());
  for (const auto& token : output) {
    if (token.kind == MathToken::Kind::Number) {
      values.push_back(token.number);
      continue;
    }
    if (token.kind == MathToken::Kind::Op) {
      if (op_arity(token.text) == 1) {
        if (values.empty()) throw ConfigurationError("Math expression underflow.");
        double a = values.back();
        values.pop_back();
        values.push_back(token.text == "u-" ? -a : a);
      } else {
        if (values.size() < 2) throw ConfigurationError("Math expression underflow.");
        double b = values.back();
        values.pop_back();
        double a = values.back();
        values.pop_back();
        if (token.text == "+") values.push_back(a + b);
        else if (token.text == "-") values.push_back(a - b);
        else if (token.text == "*") values.push_back(a * b);
        else if (token.text == "/") {
          if (b == 0) throw ConfigurationError("Division by zero in math expression.");
          values.push_back(a / b);
        }
        else if (token.text == "%") {
          if (b == 0) throw ConfigurationError("Modulo by zero in math expression.");
          values.push_back(std::fmod(a, b));
        }
        else if (token.text == "^") {
          if (std::fabs(b) > 64) {
            throw ConfigurationError("Exponent magnitude too large (max 64).");
          }
          values.push_back(std::pow(a, b));
        }
        else throw ConfigurationError("Unknown math operator: " + token.text);
      }
      continue;
    }
    if (token.kind == MathToken::Kind::Function) {
      const auto slash = token.text.find('/');
      const std::string name = slash == std::string::npos ? token.text : token.text.substr(0, slash);
      const int argc = slash == std::string::npos ? 1 : std::stoi(token.text.substr(slash + 1));
      if (static_cast<int>(values.size()) < argc) throw ConfigurationError("Math expression underflow.");
      std::vector<double> args(values.end() - argc, values.end());
      values.resize(values.size() - argc);
      values.push_back(call_math_function(name, args));
      continue;
    }
    throw ConfigurationError("Unexpected RPN token.");
  }
  if (values.size() != 1) throw ConfigurationError("Math expression did not reduce to a single value.");
  return values.back();
}

// ---------------------------------------------------------------------------
// Unified diff / patch.
// ---------------------------------------------------------------------------

namespace {

bool starts_with(const std::string& s, const std::string& prefix) {
  return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin());
}

std::vector<std::string> split_keep_lines(const std::string& text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\n') {
      lines.emplace_back(text.substr(start, i - start + 1));
      start = i + 1;
    }
  }
  if (start < text.size()) {
    lines.emplace_back(text.substr(start));
  }
  return lines;
}

// Compute LCS-based edit script using Myers' diff (length-limited).
struct DiffOp {
  enum class Kind { Equal, Insert, Delete };
  Kind kind;
  std::string line;
};

std::vector<DiffOp> myers_diff(const std::vector<std::string>& a, const std::vector<std::string>& b) {
  const int n = static_cast<int>(a.size());
  const int m = static_cast<int>(b.size());
  const int max = n + m;
  if (max == 0) return {};
  std::vector<std::vector<int>> trace;
  std::vector<int> v(static_cast<std::size_t>(2 * max + 1), 0);
  auto idx = [&](int k) { return static_cast<std::size_t>(k + max); };
  for (int d = 0; d <= max; ++d) {
    trace.push_back(v);
    for (int k = -d; k <= d; k += 2) {
      int x;
      if (k == -d || (k != d && v[idx(k - 1)] < v[idx(k + 1)])) {
        x = v[idx(k + 1)];
      } else {
        x = v[idx(k - 1)] + 1;
      }
      int y = x - k;
      while (x < n && y < m && a[static_cast<std::size_t>(x)] == b[static_cast<std::size_t>(y)]) {
        ++x;
        ++y;
      }
      v[idx(k)] = x;
      if (x >= n && y >= m) {
        // Backtrack.
        std::vector<DiffOp> ops;
        int xx = n, yy = m;
        for (int dd = d; dd > 0; --dd) {
          const auto& vv = trace[static_cast<std::size_t>(dd)];
          int kk = xx - yy;
          int prev_k;
          if (kk == -dd || (kk != dd && vv[idx(kk - 1)] < vv[idx(kk + 1)])) {
            prev_k = kk + 1;
          } else {
            prev_k = kk - 1;
          }
          int prev_x = vv[idx(prev_k)];
          int prev_y = prev_x - prev_k;
          while (xx > prev_x && yy > prev_y) {
            ops.push_back(DiffOp{DiffOp::Kind::Equal, a[static_cast<std::size_t>(xx - 1)]});
            --xx;
            --yy;
          }
          if (dd > 0) {
            if (xx == prev_x) {
              ops.push_back(DiffOp{DiffOp::Kind::Insert, b[static_cast<std::size_t>(yy - 1)]});
              --yy;
            } else {
              ops.push_back(DiffOp{DiffOp::Kind::Delete, a[static_cast<std::size_t>(xx - 1)]});
              --xx;
            }
          }
        }
        while (xx > 0 && yy > 0 && a[static_cast<std::size_t>(xx - 1)] == b[static_cast<std::size_t>(yy - 1)]) {
          ops.push_back(DiffOp{DiffOp::Kind::Equal, a[static_cast<std::size_t>(xx - 1)]});
          --xx;
          --yy;
        }
        while (xx > 0) {
          ops.push_back(DiffOp{DiffOp::Kind::Delete, a[static_cast<std::size_t>(xx - 1)]});
          --xx;
        }
        while (yy > 0) {
          ops.push_back(DiffOp{DiffOp::Kind::Insert, b[static_cast<std::size_t>(yy - 1)]});
          --yy;
        }
        std::reverse(ops.begin(), ops.end());
        return ops;
      }
    }
  }
  return {};
}

}  // namespace

std::string compute_unified_diff(const std::string& before, const std::string& after,
                                 const std::string& label_before, const std::string& label_after) {
  const auto a = split_keep_lines(before);
  const auto b = split_keep_lines(after);
  const auto ops = myers_diff(a, b);
  if (ops.empty() || std::all_of(ops.begin(), ops.end(),
                                  [](const DiffOp& op) { return op.kind == DiffOp::Kind::Equal; })) {
    return "";
  }
  // Group ops into hunks with 3 lines of context.
  constexpr int kContext = 3;
  std::ostringstream out;
  out << "--- " << label_before << "\n";
  out << "+++ " << label_after << "\n";
  std::size_t i = 0;
  int line_a = 1, line_b = 1;
  while (i < ops.size()) {
    // Skip leading equal runs.
    while (i < ops.size() && ops[i].kind == DiffOp::Kind::Equal) {
      ++i;
      ++line_a;
      ++line_b;
    }
    if (i >= ops.size()) break;
    std::size_t hunk_start = i > static_cast<std::size_t>(kContext) ? i - kContext : 0;
    // Compute line numbers at hunk_start by walking back.
    int hunk_line_a = line_a;
    int hunk_line_b = line_b;
    for (std::size_t back = i; back > hunk_start; --back) {
      const auto& op = ops[back - 1];
      if (op.kind == DiffOp::Kind::Equal) {
        --hunk_line_a;
        --hunk_line_b;
      }
    }
    std::vector<DiffOp> hunk_ops;
    for (std::size_t k = hunk_start; k < i; ++k) hunk_ops.push_back(ops[k]);
    // Collect changes + trailing context.
    int trailing_equal = 0;
    while (i < ops.size()) {
      if (ops[i].kind == DiffOp::Kind::Equal) {
        ++trailing_equal;
        hunk_ops.push_back(ops[i]);
        ++i;
        ++line_a;
        ++line_b;
        if (trailing_equal >= 2 * kContext) {
          // Check whether more changes follow within kContext.
          bool more = false;
          for (std::size_t look = i; look < ops.size() && look < i + kContext; ++look) {
            if (ops[look].kind != DiffOp::Kind::Equal) {
              more = true;
              break;
            }
          }
          if (!more) {
            // Trim last kContext equal lines? Actually keep the surrounding context but stop here.
            while (trailing_equal > kContext && !hunk_ops.empty() &&
                   hunk_ops.back().kind == DiffOp::Kind::Equal) {
              hunk_ops.pop_back();
              --trailing_equal;
              --line_a;
              --line_b;
            }
            break;
          }
        }
      } else {
        trailing_equal = 0;
        hunk_ops.push_back(ops[i]);
        if (ops[i].kind == DiffOp::Kind::Delete) ++line_a;
        else ++line_b;
        ++i;
      }
    }
    int hunk_count_a = 0, hunk_count_b = 0;
    for (const auto& op : hunk_ops) {
      if (op.kind != DiffOp::Kind::Insert) ++hunk_count_a;
      if (op.kind != DiffOp::Kind::Delete) ++hunk_count_b;
    }
    out << "@@ -" << hunk_line_a << "," << hunk_count_a << " +" << hunk_line_b << ","
        << hunk_count_b << " @@\n";
    for (const auto& op : hunk_ops) {
      char prefix = (op.kind == DiffOp::Kind::Equal) ? ' '
                                                      : (op.kind == DiffOp::Kind::Insert ? '+' : '-');
      out << prefix << op.line;
      if (op.line.empty() || op.line.back() != '\n') out << "\n";
    }
  }
  return out.str();
}

UnifiedPatchResult apply_unified_patch(const std::string& source, const std::string& patch) {
  UnifiedPatchResult result;
  const auto source_lines = split_keep_lines(source);
  const auto patch_lines = split_keep_lines(patch);
  std::vector<std::string> output(source_lines.begin(), source_lines.end());
  std::size_t pi = 0;
  while (pi < patch_lines.size()) {
    const std::string& line = patch_lines[pi];
    // Skip headers --- / +++ / preamble.
    if (starts_with(line, "--- ") || starts_with(line, "+++ ") || starts_with(line, "diff ")) {
      ++pi;
      continue;
    }
    if (!starts_with(line, "@@")) {
      ++pi;
      continue;
    }
    // Parse @@ -a,b +c,d @@
    int old_start = 0, old_count = 0;
    {
      auto minus = line.find('-');
      auto plus = line.find('+', minus);
      if (minus == std::string::npos || plus == std::string::npos) {
        result.conflicts.push_back("Malformed hunk header: " + line);
        ++pi;
        continue;
      }
      auto comma = line.find(',', minus);
      auto space = line.find(' ', minus);
      if (comma != std::string::npos && comma < plus) {
        try {
          old_start = std::stoi(line.substr(minus + 1, comma - minus - 1));
          old_count = std::stoi(line.substr(comma + 1, plus - comma - 2));
        } catch (...) {
          result.conflicts.push_back("Malformed hunk header: " + line);
          ++pi;
          continue;
        }
      } else if (space != std::string::npos && space < plus) {
        try {
          old_start = std::stoi(line.substr(minus + 1, space - minus - 1));
          old_count = 1;
        } catch (...) {
          result.conflicts.push_back("Malformed hunk header: " + line);
          ++pi;
          continue;
        }
      } else {
        result.conflicts.push_back("Malformed hunk header: " + line);
        ++pi;
        continue;
      }
    }
    ++pi;
    // Collect this hunk's body until next @@ or end.
    std::vector<std::string> hunk_body;
    while (pi < patch_lines.size() && !starts_with(patch_lines[pi], "@@") &&
           !starts_with(patch_lines[pi], "--- ") && !starts_with(patch_lines[pi], "+++ ")) {
      hunk_body.push_back(patch_lines[pi]);
      ++pi;
    }
    // Apply hunk starting at old_start (1-based).
    std::size_t apply_at = old_start > 0 ? static_cast<std::size_t>(old_start - 1) : 0;
    // Verify context + deletes against current output, build replacement.
    std::vector<std::string> new_segment;
    std::size_t check = apply_at;
    bool conflict = false;
    for (const auto& body_line : hunk_body) {
      if (body_line.empty()) continue;
      const char prefix = body_line[0];
      const std::string body = body_line.substr(1);
      if (prefix == ' ' || prefix == '-') {
        if (check >= output.size() || output[check] != body) {
          conflict = true;
          break;
        }
        if (prefix == ' ') new_segment.push_back(body);
        ++check;
      } else if (prefix == '+') {
        new_segment.push_back(body);
      } else if (prefix == '\\') {
        // "\ No newline at end of file" — ignore.
      } else {
        conflict = true;
        break;
      }
    }
    if (conflict) {
      result.conflicts.push_back("Hunk @@ -" + std::to_string(old_start) +
                                  "," + std::to_string(old_count) +
                                  " @@ did not match source.");
      result.applied_cleanly = false;
      continue;
    }
    output.erase(output.begin() + static_cast<std::ptrdiff_t>(apply_at),
                 output.begin() + static_cast<std::ptrdiff_t>(check));
    output.insert(output.begin() + static_cast<std::ptrdiff_t>(apply_at),
                  new_segment.begin(), new_segment.end());
  }
  std::string text;
  for (const auto& line : output) text += line;
  result.text = std::move(text);
  return result;
}

TruncatedOutput truncate_for_model(const std::string& output, std::size_t max_bytes,
                                   std::size_t head_lines, std::size_t tail_lines) {
  TruncatedOutput result;
  result.original_bytes = output.size();
  if (output.size() <= max_bytes) {
    result.text = output;
    result.truncated = false;
    result.kept_bytes = output.size();
    result.omitted_bytes = 0;
    result.total_lines = split_keep_lines(output).size();
    return result;
  }
  auto lines = split_keep_lines(output);
  result.total_lines = lines.size();
  if (lines.size() <= head_lines + tail_lines) {
    // Not enough lines to safely split head/tail; fall back to byte slicing.
    const std::size_t keep_each = max_bytes / 2;
    const std::size_t head_bytes = std::min<std::size_t>(keep_each, output.size());
    const std::size_t tail_bytes = std::min<std::size_t>(keep_each, output.size() - head_bytes);
    const std::size_t omitted_bytes = output.size() - head_bytes - tail_bytes;
    std::string text = output.substr(0, head_bytes);
    text += "\n... [";
    text += std::to_string(omitted_bytes) + " bytes omitted] ...\n";
    text += output.substr(output.size() - tail_bytes);
    result.text = std::move(text);
    result.truncated = true;
    result.kept_bytes = head_bytes + tail_bytes;
    result.omitted_bytes = omitted_bytes;
    return result;
  }

  std::string head_text;
  for (std::size_t i = 0; i < head_lines; ++i) head_text += lines[i];
  std::string tail_text;
  for (std::size_t i = lines.size() - tail_lines; i < lines.size(); ++i) tail_text += lines[i];
  const std::size_t omitted_line_count = lines.size() - head_lines - tail_lines;
  std::size_t omitted_bytes = 0;
  for (std::size_t i = head_lines; i < lines.size() - tail_lines; ++i) {
    omitted_bytes += lines[i].size();
  }
  std::string marker = "\n... [" + std::to_string(omitted_line_count) + " lines / " +
                       std::to_string(omitted_bytes) + " bytes omitted] ...\n";
  result.text = head_text + marker + tail_text;
  result.truncated = true;
  result.kept_bytes = head_text.size() + tail_text.size();
  result.omitted_bytes = omitted_bytes;
  return result;
}

Value truncated_output_to_value(const TruncatedOutput& output) {
  return Value::object({
      {"text", output.text},
      {"truncated", output.truncated},
      {"totalLines", static_cast<long long>(output.total_lines)},
      {"omittedBytes", static_cast<long long>(output.omitted_bytes)},
      {"kept_bytes", static_cast<long long>(output.kept_bytes)},
  });
}

}  // namespace agent
