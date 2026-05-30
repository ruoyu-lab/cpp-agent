#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <regex>
#include <sstream>

namespace agent {

namespace {

std::optional<JsonSchemaType> json_schema_type_from_string(const std::string& type) {
  if (type == "string") return JsonSchemaType::String;
  if (type == "number") return JsonSchemaType::Number;
  if (type == "integer") return JsonSchemaType::Integer;
  if (type == "boolean") return JsonSchemaType::Boolean;
  if (type == "array") return JsonSchemaType::Array;
  if (type == "object") return JsonSchemaType::Object;
  if (type == "null") return JsonSchemaType::Null;
  return std::nullopt;
}

Value string_vector_to_value(const std::vector<std::string>& values) {
  Value::Array array;
  for (const auto& value : values) {
    array.push_back(value);
  }
  return Value(std::move(array));
}

}  // namespace

AgentFrameworkError::AgentFrameworkError(std::string message, std::string details)
    : std::runtime_error(std::move(message)), details_(std::move(details)) {}

const std::string& AgentFrameworkError::details() const noexcept {
  return details_;
}

TimeoutError::TimeoutError(std::string message, std::string target, int timeout_ms)
    : AgentFrameworkError(std::move(message)), target_(std::move(target)), timeout_ms_(timeout_ms) {}

const std::string& TimeoutError::target() const noexcept {
  return target_;
}

int TimeoutError::timeout_ms() const noexcept {
  return timeout_ms_;
}

RetryExhaustedError::RetryExhaustedError(std::string message, std::string target, int attempts)
    : AgentFrameworkError(std::move(message)), target_(std::move(target)), attempts_(attempts) {}

const std::string& RetryExhaustedError::target() const noexcept {
  return target_;
}

int RetryExhaustedError::attempts() const noexcept {
  return attempts_;
}

SchemaValidationError::SchemaValidationError(std::string message, std::vector<SchemaValidationIssue> issues)
    : AgentFrameworkError(std::move(message)), issues_(std::move(issues)) {}

const std::vector<SchemaValidationIssue>& SchemaValidationError::issues() const noexcept {
  return issues_;
}

ToolExecutionError::ToolExecutionError(std::string message, std::string tool_name, std::string tool_call_id)
    : AgentFrameworkError(std::move(message)),
      tool_name_(std::move(tool_name)),
      tool_call_id_(std::move(tool_call_id)) {}

const std::string& ToolExecutionError::tool_name() const noexcept {
  return tool_name_;
}

const std::string& ToolExecutionError::tool_call_id() const noexcept {
  return tool_call_id_;
}

PermissionDeniedError::PermissionDeniedError(std::string message, std::string tool_name, std::string reason)
    : AgentFrameworkError(std::move(message)), tool_name_(std::move(tool_name)), reason_(std::move(reason)) {}

const std::string& PermissionDeniedError::tool_name() const noexcept {
  return tool_name_;
}

const std::string& PermissionDeniedError::reason() const noexcept {
  return reason_;
}

Value::Value() : storage_(nullptr) {}
Value::Value(std::nullptr_t) : storage_(nullptr) {}
Value::Value(bool value) : storage_(value) {}
Value::Value(int value) : storage_(static_cast<double>(value)) {}
Value::Value(long long value) : storage_(static_cast<double>(value)) {}
Value::Value(std::size_t value) : storage_(static_cast<double>(value)) {}
Value::Value(double value) : storage_(value) {}
Value::Value(const char* value) : storage_(std::string(value ? value : "")) {}
Value::Value(std::string value) : storage_(std::move(value)) {}
Value::Value(Array value) : storage_(std::move(value)) {}
Value::Value(Object value) : storage_(std::move(value)) {}

Value Value::array(std::initializer_list<Value> values) {
  return Value(Array(values));
}

Value Value::object(std::initializer_list<std::pair<std::string, Value>> values) {
  Object object;
  for (const auto& [key, value] : values) {
    object[key] = value;
  }
  return Value(std::move(object));
}

bool Value::is_null() const noexcept {
  return std::holds_alternative<std::nullptr_t>(storage_);
}

bool Value::is_bool() const noexcept {
  return std::holds_alternative<bool>(storage_);
}

bool Value::is_number() const noexcept {
  return std::holds_alternative<double>(storage_);
}

bool Value::is_string() const noexcept {
  return std::holds_alternative<std::string>(storage_);
}

bool Value::is_array() const noexcept {
  return std::holds_alternative<Array>(storage_);
}

bool Value::is_object() const noexcept {
  return std::holds_alternative<Object>(storage_);
}

bool Value::as_bool(bool fallback) const {
  return is_bool() ? std::get<bool>(storage_) : fallback;
}

double Value::as_number(double fallback) const {
  return is_number() ? std::get<double>(storage_) : fallback;
}

long long Value::as_integer(long long fallback) const {
  return is_number() ? static_cast<long long>(std::get<double>(storage_)) : fallback;
}

std::string Value::as_string(std::string fallback) const {
  return is_string() ? std::get<std::string>(storage_) : std::move(fallback);
}

const Value::Array& Value::as_array() const {
  if (!is_array()) {
    static const Array empty;
    return empty;
  }
  return std::get<Array>(storage_);
}

Value::Array& Value::as_array() {
  if (!is_array()) {
    storage_ = Array{};
  }
  return std::get<Array>(storage_);
}

const Value::Object& Value::as_object() const {
  if (!is_object()) {
    static const Object empty;
    return empty;
  }
  return std::get<Object>(storage_);
}

Value::Object& Value::as_object() {
  if (!is_object()) {
    storage_ = Object{};
  }
  return std::get<Object>(storage_);
}

bool Value::contains(const std::string& key) const {
  const auto& object = as_object();
  return object.find(key) != object.end();
}

const Value& Value::at(const std::string& key) const {
  const auto& object = as_object();
  const auto found = object.find(key);
  return found == object.end() ? null_value() : found->second;
}

Value& Value::operator[](const std::string& key) {
  return as_object()[key];
}

std::string Value::stringify(int indent) const {
  return stringify_impl(*this, indent, 0);
}

bool operator==(const Value& left, const Value& right) {
  return left.storage_ == right.storage_;
}

bool operator!=(const Value& left, const Value& right) {
  return !(left == right);
}

std::string safe_json_stringify(const Value& value) {
  try {
    return value.stringify(2);
  } catch (const std::exception& error) {
    return Value::object({{"error", "Failed to serialize value"}, {"message", error.what()}}).stringify(2);
  }
}

Value parse_json(const std::string& text) {
  return parse_json_text(text);
}

Value read_json_file(const std::filesystem::path& path) {
  return parse_json(bytes_to_text(read_binary_file(path)));
}

void write_json_file(const std::filesystem::path& path, const Value& value, int indent) {
  write_text_file(path, value.stringify(indent));
}

std::string now_iso8601() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return out.str();
}

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
  static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  int value = 0;
  int bits = -6;
  for (const auto byte : bytes) {
    value = (value << 8) + byte;
    bits += 8;
    while (bits >= 0) {
      output.push_back(table[(value >> bits) & 0x3f]);
      bits -= 6;
    }
  }
  if (bits > -6) {
    output.push_back(table[((value << 8) >> (bits + 8)) & 0x3f]);
  }
  while (output.size() % 4 != 0) {
    output.push_back('=');
  }
  return output;
}

std::string sha256_hex(const std::string& text) {
  static constexpr std::array<std::uint32_t, 64> k{
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
      0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
      0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
      0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
      0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
      0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
      0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
      0xc67178f2,
  };
  auto rotr = [](std::uint32_t value, std::uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
  };

  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  const std::uint64_t bit_length = static_cast<std::uint64_t>(bytes.size()) * 8;
  bytes.push_back(0x80);
  while ((bytes.size() % 64) != 56) {
    bytes.push_back(0);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<std::uint8_t>((bit_length >> shift) & 0xff));
  }

  std::array<std::uint32_t, 8> h{
      0x6a09e667,
      0xbb67ae85,
      0x3c6ef372,
      0xa54ff53a,
      0x510e527f,
      0x9b05688c,
      0x1f83d9ab,
      0x5be0cd19,
  };

  for (std::size_t chunk = 0; chunk < bytes.size(); chunk += 64) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16; ++i) {
      const std::size_t offset = chunk + i * 4;
      w[i] = (static_cast<std::uint32_t>(bytes[offset]) << 24) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
             static_cast<std::uint32_t>(bytes[offset + 3]);
    }
    for (std::size_t i = 16; i < 64; ++i) {
      const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    auto a = h[0];
    auto b = h[1];
    auto c = h[2];
    auto d = h[3];
    auto e = h[4];
    auto f = h[5];
    auto g = h[6];
    auto hh = h[7];

    for (std::size_t i = 0; i < 64; ++i) {
      const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto ch = (e & f) ^ ((~e) & g);
      const auto temp1 = hh + s1 + ch + k[i] + w[i];
      const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto lane : h) {
    out << std::setw(8) << lane;
  }
  return out.str();
}

std::string generate_uuid() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<int> dist(0, 255);
  std::array<unsigned char, 16> bytes{};
  for (auto& byte : bytes) {
    byte = static_cast<unsigned char>(dist(rng));
  }
  bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out << "-";
    }
    out << std::setw(2) << static_cast<int>(bytes[i]);
  }
  return out.str();
}

JsonSchema normalize_json_schema(const JsonSchema& schema) {
  JsonSchema normalized = schema;
  if (normalized.type == JsonSchemaType::Object &&
      !normalized.additional_properties.is_set() &&
      !normalized.additional_properties_schema) {
    normalized.additional_properties = false;
  }
  for (auto& child : normalized.all_of) {
    child = normalize_json_schema(child);
  }
  for (auto& child : normalized.any_of) {
    child = normalize_json_schema(child);
  }
  for (auto& child : normalized.one_of) {
    child = normalize_json_schema(child);
  }
  return normalized;
}

namespace {

std::vector<SchemaValidationIssue> validate_json_schema_impl(const JsonSchema& schema, const Value& value,
                                                             const std::string& path) {
  std::vector<SchemaValidationIssue> issues;
  if (schema.type && !schema_type_matches(*schema.type, value)) {
    issues.push_back({path, "Expected " + schema_type_name(*schema.type) + "."});
    return issues;
  }

  if (!schema.enum_values.empty() &&
      std::find(schema.enum_values.begin(), schema.enum_values.end(), value) == schema.enum_values.end()) {
    issues.push_back({path, "Value is not included in enum."});
  }

  if (schema.const_value && !(value == *schema.const_value)) {
    issues.push_back({path, "Value does not match const."});
  }

  if (value.is_string()) {
    const auto length = value.as_string().size();
    if (schema.min_length && length < *schema.min_length) {
      issues.push_back({path, "String is shorter than minLength."});
    }
    if (schema.max_length && length > *schema.max_length) {
      issues.push_back({path, "String is longer than maxLength."});
    }
    if (schema.pattern) {
      try {
        std::regex re(*schema.pattern, std::regex::ECMAScript);
        if (!std::regex_search(value.as_string(), re)) {
          issues.push_back({path, "String does not match pattern."});
        }
      } catch (const std::regex_error&) {
        issues.push_back({path, "Invalid pattern in schema."});
      }
    }
  }

  if (value.is_number()) {
    const double number = value.as_number();
    if (schema.minimum && number < *schema.minimum) {
      issues.push_back({path, "Number is lower than minimum."});
    }
    if (schema.maximum && number > *schema.maximum) {
      issues.push_back({path, "Number is greater than maximum."});
    }
  }

  if (schema.type == JsonSchemaType::Object && value.is_object()) {
    const auto& object = value.as_object();
    for (const auto& key : schema.required) {
      if (object.find(key) == object.end()) {
        issues.push_back({path + "." + key, "Required property is missing."});
      }
    }
    for (const auto& [key, child_schema] : schema.properties) {
      const auto found = object.find(key);
      if (found != object.end()) {
        auto nested = validate_json_schema_impl(child_schema, found->second, path + "." + key);
        issues.insert(issues.end(), nested.begin(), nested.end());
      }
    }
    if (!schema.additional_properties) {
      for (const auto& [key, _] : object) {
        if (schema.properties.find(key) == schema.properties.end()) {
          issues.push_back({path + "." + key, "Additional property is not allowed."});
        }
      }
    } else if (schema.additional_properties_schema) {
      for (const auto& [key, property_value] : object) {
        if (schema.properties.find(key) != schema.properties.end()) {
          continue;
        }
        auto nested = validate_json_schema_impl(*schema.additional_properties_schema, property_value, path + "." + key);
        issues.insert(issues.end(), nested.begin(), nested.end());
      }
    }
  }

  if (schema.type == JsonSchemaType::Array && value.is_array() && schema.items) {
    const auto& array = value.as_array();
    for (std::size_t index = 0; index < array.size(); ++index) {
      auto nested = validate_json_schema_impl(*schema.items, array[index], path + "[" + std::to_string(index) + "]");
      issues.insert(issues.end(), nested.begin(), nested.end());
    }
  }

  for (std::size_t index = 0; index < schema.all_of.size(); ++index) {
    auto nested = validate_json_schema_impl(schema.all_of[index], value, path);
    if (!nested.empty()) {
      issues.push_back({path, "Value does not match allOf[" + std::to_string(index) + "]."});
      issues.insert(issues.end(), nested.begin(), nested.end());
    }
  }

  if (!schema.any_of.empty()) {
    bool matched = false;
    for (const auto& subschema : schema.any_of) {
      if (validate_json_schema_impl(subschema, value, path).empty()) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      issues.push_back({path, "Value does not match any of the anyOf subschemas."});
    }
  }

  if (!schema.one_of.empty()) {
    std::size_t matches = 0;
    for (const auto& subschema : schema.one_of) {
      if (validate_json_schema_impl(subschema, value, path).empty()) {
        ++matches;
      }
    }
    if (matches != 1) {
      issues.push_back({path, "Value must match exactly one oneOf subschema (matched " +
                                  std::to_string(matches) + ")."});
    }
  }

  return issues;
}

}  // namespace

std::vector<SchemaValidationIssue> validate_json_schema(const JsonSchema& schema, const Value& value,
                                                        const std::string& path) {
  const auto normalized = normalize_json_schema(schema);
  return validate_json_schema_impl(normalized, value, path);
}

void assert_json_schema(const JsonSchema& schema, const Value& value) {
  auto issues = validate_json_schema(schema, value);
  if (!issues.empty()) {
    throw SchemaValidationError("JSON schema validation failed.", std::move(issues));
  }
}

Value json_schema_to_value(const JsonSchema& schema) {
  Value value = Value::object({});
  if (schema.type) {
    value["type"] = schema_type_name(*schema.type);
  }
  if (!schema.description.empty()) {
    value["description"] = schema.description;
  }
  if (!schema.properties.empty() || schema.type == JsonSchemaType::Object) {
    Value::Object properties;
    for (const auto& [key, child] : schema.properties) {
      properties[key] = json_schema_to_value(child);
    }
    value["properties"] = Value(std::move(properties));
  }
  if (!schema.required.empty() || schema.type == JsonSchemaType::Object) {
    value["required"] = string_vector_to_value(schema.required);
  }
  if (schema.additional_properties_schema) {
    value["additionalProperties"] = json_schema_to_value(*schema.additional_properties_schema);
  } else if (schema.additional_properties.is_set()) {
    value["additionalProperties"] = static_cast<bool>(schema.additional_properties);
  }
  if (schema.items) {
    value["items"] = json_schema_to_value(*schema.items);
  }
  if (!schema.enum_values.empty()) {
    value["enum"] = Value(schema.enum_values);
  }
  if (schema.min_length) {
    value["minLength"] = *schema.min_length;
  }
  if (schema.max_length) {
    value["maxLength"] = *schema.max_length;
  }
  if (schema.minimum) {
    value["minimum"] = *schema.minimum;
  }
  if (schema.maximum) {
    value["maximum"] = *schema.maximum;
  }
  if (schema.pattern) {
    value["pattern"] = *schema.pattern;
  }
  if (schema.const_value) {
    value["const"] = *schema.const_value;
  }
  auto schema_list_to_value = [](const std::vector<JsonSchema>& list) {
    Value::Array array;
    array.reserve(list.size());
    for (const auto& child : list) {
      array.push_back(json_schema_to_value(child));
    }
    return Value(std::move(array));
  };
  if (!schema.all_of.empty()) {
    value["allOf"] = schema_list_to_value(schema.all_of);
  }
  if (!schema.any_of.empty()) {
    value["anyOf"] = schema_list_to_value(schema.any_of);
  }
  if (!schema.one_of.empty()) {
    value["oneOf"] = schema_list_to_value(schema.one_of);
  }
  return value;
}

JsonSchema json_schema_from_value(const Value& value) {
  JsonSchema schema;
  if (!value.is_object()) {
    return schema;
  }
  if (auto type = json_schema_type_from_string(value.at("type").as_string())) {
    schema.type = *type;
  }
  schema.description = value.at("description").as_string();
  for (const auto& [key, child] : value.at("properties").as_object()) {
    schema.properties[key] = json_schema_from_value(child);
  }
  for (const auto& item : value.at("required").as_array()) {
    const auto required = item.as_string();
    if (!required.empty()) {
      schema.required.push_back(required);
    }
  }
  if (value.at("additionalProperties").is_bool()) {
    schema.additional_properties = value.at("additionalProperties").as_bool();
  } else if (value.at("additional_properties").is_bool()) {
    schema.additional_properties = value.at("additional_properties").as_bool();
  } else if (value.at("additionalProperties").is_object()) {
    schema.additional_properties = true;
    schema.additional_properties_schema = std::make_shared<JsonSchema>(
        json_schema_from_value(value.at("additionalProperties")));
  } else if (value.at("additional_properties").is_object()) {
    schema.additional_properties = true;
    schema.additional_properties_schema = std::make_shared<JsonSchema>(
        json_schema_from_value(value.at("additional_properties")));
  }
  if (value.at("items").is_object()) {
    schema.items = std::make_shared<JsonSchema>(json_schema_from_value(value.at("items")));
  }
  for (const auto& item : value.at("enum").as_array()) {
    schema.enum_values.push_back(item);
  }
  if (value.at("minLength").is_number() || value.at("min_length").is_number()) {
    schema.min_length = static_cast<std::size_t>(
        std::max<long long>(0, value.at("minLength").as_integer(value.at("min_length").as_integer())));
  }
  if (value.at("maxLength").is_number() || value.at("max_length").is_number()) {
    schema.max_length = static_cast<std::size_t>(
        std::max<long long>(0, value.at("maxLength").as_integer(value.at("max_length").as_integer())));
  }
  if (value.at("minimum").is_number()) {
    schema.minimum = value.at("minimum").as_number();
  }
  if (value.at("maximum").is_number()) {
    schema.maximum = value.at("maximum").as_number();
  }
  if (value.at("pattern").is_string()) {
    const auto pattern = value.at("pattern").as_string();
    if (!pattern.empty()) {
      schema.pattern = pattern;
    }
  }
  {
    const auto& const_value = value.at("const");
    if (!const_value.is_null()) {
      schema.const_value = const_value;
    }
  }
  auto schema_list_from_value = [](const Value& source) {
    std::vector<JsonSchema> result;
    if (!source.is_array()) {
      return result;
    }
    const auto& array = source.as_array();
    result.reserve(array.size());
    for (const auto& item : array) {
      if (item.is_object()) {
        result.push_back(json_schema_from_value(item));
      }
    }
    return result;
  };
  schema.all_of = schema_list_from_value(value.at("allOf"));
  schema.any_of = schema_list_from_value(value.at("anyOf"));
  schema.one_of = schema_list_from_value(value.at("oneOf"));
  return schema;
}
}  // namespace agent
