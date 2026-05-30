#pragma once

#include "agent/common.hpp"

namespace agent {

class AgentFrameworkError : public std::runtime_error {
 public:
  explicit AgentFrameworkError(std::string message, std::string details = {});
  [[nodiscard]] const std::string& details() const noexcept;

 private:
  std::string details_;
};

class ConfigurationError : public AgentFrameworkError {
 public:
  using AgentFrameworkError::AgentFrameworkError;
};

class AdapterError : public AgentFrameworkError {
 public:
  using AgentFrameworkError::AgentFrameworkError;
};

class TimeoutError : public AgentFrameworkError {
 public:
  TimeoutError(std::string message, std::string target, int timeout_ms);
  [[nodiscard]] const std::string& target() const noexcept;
  [[nodiscard]] int timeout_ms() const noexcept;

 private:
  std::string target_;
  int timeout_ms_;
};

class RetryExhaustedError : public AgentFrameworkError {
 public:
  RetryExhaustedError(std::string message, std::string target, int attempts);
  [[nodiscard]] const std::string& target() const noexcept;
  [[nodiscard]] int attempts() const noexcept;

 private:
  std::string target_;
  int attempts_;
};

struct SchemaValidationIssue {
  std::string path;
  std::string message;
};

class SchemaValidationError : public AgentFrameworkError {
 public:
  SchemaValidationError(std::string message, std::vector<SchemaValidationIssue> issues);
  [[nodiscard]] const std::vector<SchemaValidationIssue>& issues() const noexcept;

 private:
  std::vector<SchemaValidationIssue> issues_;
};

class ToolExecutionError : public AgentFrameworkError {
 public:
  ToolExecutionError(std::string message, std::string tool_name, std::string tool_call_id);
  [[nodiscard]] const std::string& tool_name() const noexcept;
  [[nodiscard]] const std::string& tool_call_id() const noexcept;

 private:
  std::string tool_name_;
  std::string tool_call_id_;
};

class PermissionDeniedError : public AgentFrameworkError {
 public:
  PermissionDeniedError(std::string message, std::string tool_name, std::string reason);
  [[nodiscard]] const std::string& tool_name() const noexcept;
  [[nodiscard]] const std::string& reason() const noexcept;

 private:
  std::string tool_name_;
  std::string reason_;
};

class Value {
 public:
  using Array = std::vector<Value>;
  using Object = std::map<std::string, Value>;

  Value();
  Value(std::nullptr_t);
  Value(bool value);
  Value(int value);
  Value(long long value);
  Value(std::size_t value);
  Value(double value);
  Value(const char* value);
  Value(std::string value);
  Value(Array value);
  Value(Object value);

  static Value array(std::initializer_list<Value> values);
  static Value object(std::initializer_list<std::pair<std::string, Value>> values);

  [[nodiscard]] bool is_null() const noexcept;
  [[nodiscard]] bool is_bool() const noexcept;
  [[nodiscard]] bool is_number() const noexcept;
  [[nodiscard]] bool is_string() const noexcept;
  [[nodiscard]] bool is_array() const noexcept;
  [[nodiscard]] bool is_object() const noexcept;

  [[nodiscard]] bool as_bool(bool fallback = false) const;
  [[nodiscard]] double as_number(double fallback = 0) const;
  [[nodiscard]] long long as_integer(long long fallback = 0) const;
  [[nodiscard]] std::string as_string(std::string fallback = {}) const;

  [[nodiscard]] const Array& as_array() const;
  [[nodiscard]] Array& as_array();
  [[nodiscard]] const Object& as_object() const;
  [[nodiscard]] Object& as_object();
  [[nodiscard]] bool contains(const std::string& key) const;
  [[nodiscard]] const Value& at(const std::string& key) const;
  Value& operator[](const std::string& key);

  [[nodiscard]] std::string stringify(int indent = 0) const;

  friend bool operator==(const Value& left, const Value& right);
  friend bool operator!=(const Value& left, const Value& right);

 private:
  using Storage = std::variant<std::nullptr_t, bool, double, std::string, Array, Object>;
  Storage storage_;
};

std::string safe_json_stringify(const Value& value);
Value parse_json(const std::string& text);
Value read_json_file(const std::filesystem::path& path);
void write_json_file(const std::filesystem::path& path, const Value& value, int indent = 2);
std::string now_iso8601();
std::string generate_uuid();
std::string base64_encode(const std::vector<std::uint8_t>& bytes);
std::string sha256_hex(const std::string& text);

enum class JsonSchemaType {
  String,
  Number,
  Integer,
  Boolean,
  Array,
  Object,
  Null,
};

struct JsonSchemaAdditionalProperties {
  bool value = true;
  bool specified = false;

  JsonSchemaAdditionalProperties() = default;
  JsonSchemaAdditionalProperties(bool initial_value) : value(initial_value), specified(true) {}

  JsonSchemaAdditionalProperties& operator=(bool next_value) noexcept {
    value = next_value;
    specified = true;
    return *this;
  }

  [[nodiscard]] bool is_set() const noexcept {
    return specified;
  }

  operator bool() const noexcept {
    return value;
  }
};

struct JsonSchema {
  std::optional<JsonSchemaType> type;
  std::string description;
  std::map<std::string, JsonSchema> properties;
  std::vector<std::string> required;
  JsonSchemaAdditionalProperties additional_properties;
  std::shared_ptr<JsonSchema> additional_properties_schema;
  std::shared_ptr<JsonSchema> items;
  std::vector<Value> enum_values;
  std::optional<std::size_t> min_length;
  std::optional<std::size_t> max_length;
  std::optional<double> minimum;
  std::optional<double> maximum;
  // ECMAScript regular expression applied to string values.
  std::optional<std::string> pattern;
  // `const` keyword — value must be deeply equal to this payload.
  std::optional<Value> const_value;
  // Composition: validate against all / at-least-one / exactly-one subschema.
  std::vector<JsonSchema> all_of;
  std::vector<JsonSchema> any_of;
  std::vector<JsonSchema> one_of;
};

JsonSchema normalize_json_schema(const JsonSchema& schema);
std::vector<SchemaValidationIssue> validate_json_schema(
    const JsonSchema& schema,
    const Value& value,
    const std::string& path = "$");
void assert_json_schema(const JsonSchema& schema, const Value& value);
Value json_schema_to_value(const JsonSchema& schema);
JsonSchema json_schema_from_value(const Value& value);

}  // namespace agent
