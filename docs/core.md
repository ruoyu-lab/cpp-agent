# Core API

The native core module contains the framework's common error types, dynamic
`Value` payload, JSON helpers, identifiers, hashing, base64 helpers, and JSON
Schema validation. Higher-level modules use these types instead of introducing
third-party JSON or schema dependencies.

## Errors

All framework-specific exceptions derive from `AgentFrameworkError`:

```cpp
try {
  throw agent::ConfigurationError("Missing model provider.");
} catch (const agent::AgentFrameworkError& error) {
  auto message = std::string(error.what());
  auto details = error.details();
}
```

Specialized error types preserve context:

- `ConfigurationError`: invalid app/configuration state.
- `AdapterError`: injected adapter or provider failure.
- `TimeoutError`: target plus timeout milliseconds.
- `RetryExhaustedError`: target plus attempted count.
- `SchemaValidationError`: structured schema validation issues.
- `ToolExecutionError`: tool name and tool-call id.
- `PermissionDeniedError`: tool name and denial reason.

## Value

`Value` is the native JSON-like dynamic payload used across the public API:

```cpp
agent::Value payload = agent::Value::object({
    {"ok", true},
    {"items", agent::Value::array({"a", "b"})},
});

payload["count"] = 2;
auto text = payload.at("items").as_array().front().as_string();
```

Supported kinds are null, bool, number, string, array, and object.

Access helpers are intentionally conservative:

- `as_bool`, `as_number`, `as_integer`, and `as_string` return fallbacks for
  mismatched types.
- `as_array` and `as_object` require the matching type.
- `at(key)` returns a null value when an object key is missing.
- `operator[](key)` promotes a non-object value to an object before assigning.

Use `stringify(indent)` to serialize values for logs, fixtures, and persisted
artifacts.

## JSON And Files

Core JSON helpers:

```cpp
auto parsed = agent::parse_json(R"({"name":"native"})");
agent::write_json_file("state.json", parsed);
auto loaded = agent::read_json_file("state.json");
```

`safe_json_stringify` returns a JSON error object if serialization throws, which
is useful for audit and diagnostic paths that should preserve the original
failure.

Other core helpers:

- `now_iso8601()`
- `generate_uuid()`
- `base64_encode`
- `base64_encode_text`
- `sha256_hex`

## JSON Schema

`JsonSchema` represents the subset used by tools, structured output, config,
server governance, MCP adapters, and eval assertions:

```cpp
agent::JsonSchema schema;
schema.type = agent::JsonSchemaType::Object;
schema.required = {"text"};
schema.properties["text"].type = agent::JsonSchemaType::String;
schema.additional_properties = false;

agent::assert_json_schema(schema, agent::Value::object({{"text", "ok"}}));
```

Supported fields include:

- `type`
- `description`
- `properties`
- `required`
- `additional_properties`
- `additional_properties_schema`
- `items`
- `enum_values`
- `min_length` / `max_length`
- `minimum` / `maximum`
- `pattern` — ECMAScript regex applied to string values (`std::regex` /
  `std::regex::ECMAScript`).
- `const_value` — deep equality against a fixed payload (JSON Schema `const`).
- `all_of` — every subschema must validate.
- `any_of` — at least one subschema must validate.
- `one_of` — exactly one subschema must validate.

`normalize_json_schema` applies Node-style object defaults and recurses into
each composition subschema. Object schemas that do not explicitly set
`additionalProperties` default to closed objects.

Composition subschemas are evaluated against the same value at the same path
regardless of the parent `type`. They can be combined freely with `type`,
`properties`, `enum_values`, and the other fields above.

Validation functions:

- `validate_json_schema` returns all issues.
- `assert_json_schema` throws `SchemaValidationError` when invalid.
- `json_schema_to_value` serializes schemas, including `pattern`, `const`,
  `allOf`, `anyOf`, and `oneOf`.
- `json_schema_from_value` parses serialized schemas, including
  `additionalProperties` as either bool or typed schema, plus `pattern`,
  `const`, `allOf`, `anyOf`, and `oneOf`.

Example:

```cpp
agent::JsonSchema schema;
schema.type = agent::JsonSchemaType::Object;
schema.properties["api_key"].type = agent::JsonSchemaType::String;
schema.properties["api_key"].pattern = "^sk-[A-Za-z0-9]{20,}$";
schema.properties["kind"].const_value = agent::Value("envelope");

agent::JsonSchema text_payload;
text_payload.type = agent::JsonSchemaType::String;
text_payload.min_length = 1;
agent::JsonSchema image_payload;
image_payload.type = agent::JsonSchemaType::Object;
image_payload.properties["url"].type = agent::JsonSchemaType::String;
image_payload.required = {"url"};
schema.properties["payload"].one_of = {text_payload, image_payload};
```

## Zero-Dependency Boundary

Core uses only the C++ standard library and local framework helpers. It does not
link a JSON library, schema library, crypto library, database, network stack, or
JavaScript runtime.
