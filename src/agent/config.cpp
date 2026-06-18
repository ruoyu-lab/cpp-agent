#include "agent/app_api.hpp"
#include "agent/knowledge_io.hpp"
#include "agent/memory_vector.hpp"
#include "agent/mcp_native.hpp"
#include "agent/providers/reasoning.hpp"
#include "agent/skills.hpp"
#include "detail/helpers.hpp"
#include "config_helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>

namespace agent {

namespace {

constexpr std::array<const char*, 5> kNativeAgentConfigFiles = {
    "node-agent.config.mjs",
    "node-agent.config.js",
    "node-agent.config.cjs",
    "node-agent.config.json",
    "node-agent.config.ts",
};

using config_detail::embedding_dimensions_from_config;
using config_detail::env_value;
using config_detail::reasoning_mode_from_model_config;
using config_detail::resolve_defaulted_env_config_value;
using config_detail::resolve_env_config_value;

std::string lowercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

bool is_native_json_config_file(const std::filesystem::path& path) {
  return lowercase_copy(path.extension().string()) == ".json";
}

bool is_native_module_config_file(const std::filesystem::path& path) {
  const auto extension = lowercase_copy(path.extension().string());
  return extension == ".mjs" || extension == ".js" || extension == ".cjs" || extension == ".ts";
}

std::filesystem::path current_config_cwd() {
  std::error_code error;
  auto cwd = std::filesystem::current_path(error);
  return error ? std::filesystem::path(".") : cwd;
}

void attach_config_location(Value& config,
                            const std::filesystem::path& cwd,
                            const std::filesystem::path& path = {}) {
  const auto effective_cwd = cwd.empty() ? current_config_cwd() : cwd;
  config["_cwd"] = effective_cwd.string();
  if (!path.empty()) {
    config["_path"] = path.string();
  }
}

void attach_config_cwd(Value& config, const std::filesystem::path& resolved_path) {
  attach_config_location(
      config,
      resolved_path.parent_path().empty() ? current_config_cwd() : resolved_path.parent_path(),
      resolved_path);
}

std::filesystem::path resolve_config_path(const Value& root, const std::string& raw) {
  std::filesystem::path path(raw);
  if (raw.empty() || path.is_absolute()) {
    return path;
  }
  const std::string cwd = root.at("_cwd").as_string();
  return cwd.empty() ? path : std::filesystem::path(cwd) / path;
}

std::vector<std::string> string_array_from_value(const Value& value) {
  std::vector<std::string> result;
  if (!value.is_array()) {
    return result;
  }
  for (const auto& item : value.as_array()) {
    const auto text = item.as_string();
    if (!text.empty()) {
      result.push_back(text);
    }
  }
  return result;
}

std::vector<std::string> string_list_from_value(const Value& value) {
  if (value.is_string()) {
    const auto text = value.as_string();
    return text.empty() ? std::vector<std::string>{} : std::vector<std::string>{text};
  }
  return string_array_from_value(value);
}

std::map<std::string, std::string> string_map_from_value(const Value& value) {
  std::map<std::string, std::string> result;
  if (!value.is_object()) {
    return result;
  }
  for (const auto& [key, item] : value.as_object()) {
    result[key] = item.as_string();
  }
  return result;
}

void add_env_ref(std::set<std::string>& keys, const Value& value) {
  const auto key = value.as_string();
  if (!key.empty()) {
    keys.insert(key);
  }
}

bool is_one_of(const std::string& value, std::initializer_list<const char*> allowed) {
  return std::any_of(allowed.begin(), allowed.end(), [&](const char* candidate) {
    return value == candidate;
  });
}

bool is_named_resource_ref(const Value& value) {
  return value.is_object() && value.at("use").is_string() && !value.at("use").as_string().empty();
}

std::string allowed_values_text(std::initializer_list<const char*> values) {
  std::string allowed;
  for (const auto* item : values) {
    if (!allowed.empty()) {
      allowed += ", ";
    }
    allowed += item;
  }
  return allowed;
}

std::string allowed_values_text(const std::vector<std::string>& values) {
  std::string allowed;
  for (const auto& item : values) {
    if (!allowed.empty()) {
      allowed += ", ";
    }
    allowed += item;
  }
  return allowed;
}

const Value& require_config_resource(const Value& config,
                                     const std::string& collection,
                                     const std::string& name,
                                     const std::string& type_name) {
  const auto& resources = config.at("resources").at(collection);
  if (!resources.is_object() || !resources.contains(name)) {
    throw ConfigurationError("Unknown " + type_name + " resource \"" + name + "\".");
  }
  return resources.at(name);
}

bool provider_supports_reasoning_field(const Value& model, const std::string& field) {
  const auto profile = resolve_native_chat_provider_profile(model);
  return provider_request_profile_supports_reasoning_field(profile, field);
}

void validate_reasoning_config(const Value& model, const std::string& path) {
  const auto& reasoning = model.at("reasoning");
  if (!model.contains("reasoning")) {
    return;
  }
  assert_reasoning_settings(reasoning, path + ".reasoning");
  const std::string provider = model.at("provider").as_string();
  if (reasoning.at("enabled").as_bool(false) && !provider_supports_reasoning_field(model, "enabled")) {
    throw ConfigurationError(path + ".reasoning.enabled requires the provider to support \"reasoning\".");
  }
  if (!reasoning.at("budget").is_null() && !provider_supports_reasoning_field(model, "budget")) {
    throw ConfigurationError(path + ".reasoning.budget is not supported by provider \"" + provider + "\".");
  }
  if (reasoning.at("includeThoughts").is_bool() &&
      !provider_supports_reasoning_field(model, "includeThoughts")) {
    throw ConfigurationError(path + ".reasoning.includeThoughts is not supported by provider \"" + provider + "\".");
  }
}

void validate_string_array_values(const Value& value, const std::string& path) {
  if (!value.is_array()) {
    return;
  }
  std::size_t index = 0;
  for (const auto& item : value.as_array()) {
    if (!item.is_string() || item.as_string().empty()) {
      throw ConfigurationError(path + "[" + std::to_string(index) + "] must be a non-empty string.");
    }
    ++index;
  }
}

std::set<std::string> default_model_capabilities(const std::string& provider) {
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (descriptor && !descriptor->default_capabilities.empty()) {
    return descriptor->default_capabilities;
  }
  return {"input.text"};
}

std::set<std::string> model_capabilities_from_config(const Value& model) {
  const auto provider = model.at("provider").as_string();
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  std::set<std::string> capabilities =
      descriptor && descriptor->resolve_capabilities
          ? descriptor->resolve_capabilities(model)
          : default_model_capabilities(provider);
  const auto profile = resolve_native_chat_provider_profile(model);
  if (provider_request_profile_supports_reasoning_field(profile, "budget")) {
    capabilities.insert("reasoning.budget");
  }
  for (const auto& item : model.at("capabilities").as_array()) {
    const auto capability = item.as_string();
    if (!capability.empty()) {
      capabilities.insert(capability);
    }
  }
  return capabilities;
}

std::vector<std::string> model_required_capabilities(const Value& model) {
  return string_array_from_value(model.at("requireCapabilities"));
}

bool model_satisfies_capabilities(const Value& model, const std::vector<std::string>& required) {
  if (required.empty()) {
    return true;
  }
  const auto capabilities = model_capabilities_from_config(model);
  return std::all_of(required.begin(), required.end(), [&](const auto& capability) {
    return capabilities.find(capability) != capabilities.end();
  });
}

void flatten_model_candidates(const Value& model, std::vector<Value>& candidates) {
  if (!model.is_object()) {
    return;
  }
  candidates.push_back(model);
  for (const auto& fallback : model.at("fallbacks").as_array()) {
    flatten_model_candidates(fallback, candidates);
  }
}

std::string model_candidate_key(const Value& model) {
  return model.at("provider").as_string() + ":" +
         model.at("model").as_string(
             model.at("modelPath").as_string(model.at("modelPathEnv").as_string("unknown")));
}

std::vector<Value> select_model_candidates(const Value& model) {
  std::vector<Value> flattened;
  flatten_model_candidates(model, flattened);
  const auto required = model_required_capabilities(model);
  std::set<std::string> seen;
  std::vector<Value> selected;
  for (const auto& candidate : flattened) {
    const auto key = model_candidate_key(candidate);
    if (seen.find(key) != seen.end()) {
      continue;
    }
    seen.insert(key);
    if (model_satisfies_capabilities(candidate, required)) {
      selected.push_back(candidate);
    }
  }
  if (selected.empty()) {
    std::string required_text;
    for (const auto& capability : required) {
      if (!required_text.empty()) {
        required_text += ", ";
      }
      required_text += capability;
    }
    throw ConfigurationError("No configured model candidate satisfies capabilities: " +
                             (required_text.empty() ? "(none)" : required_text) + ".");
  }
  return selected;
}

void validate_model_config(const Value& model, const std::string& path) {
  if (!model.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  const std::string provider = model.at("provider").as_string();
  if (provider.empty()) {
    throw ConfigurationError(path + ".provider is required.");
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (!descriptor) {
    throw ConfigurationError(path + ".provider \"" + provider +
                             "\" is not registered as a native chat provider.");
  }
  if (descriptor->validate_model) {
    descriptor->validate_model(model, path);
  }
  validate_string_array_values(model.at("capabilities"), path + ".capabilities");
  validate_string_array_values(model.at("requireCapabilities"), path + ".requireCapabilities");
  const auto reasoning_mode = reasoning_mode_from_model_config(model, path);
  if (reasoning_mode != ProviderReasoningMode::None) {
    const auto profile = resolve_native_chat_provider_profile(model);
    if (!provider_request_profile_supports_reasoning_field(profile, "enabled")) {
      throw ConfigurationError(path + ".reasoningMode \"" + model.at("reasoningMode").as_string() +
                               "\" is not valid for provider \"" +
                               model.at("provider").as_string() + "\".");
    }
  }
  validate_reasoning_config(model, path);
  if (model.at("fallbacks").is_array()) {
    std::size_t index = 0;
    for (const auto& fallback : model.at("fallbacks").as_array()) {
      validate_model_config(fallback, path + ".fallbacks[" + std::to_string(index) + "]");
      ++index;
    }
  }
  (void)select_model_candidates(model);
}

void validate_embedding_config(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  const std::string provider = value.at("provider").as_string();
  if (provider.empty()) {
    throw ConfigurationError(path + ".provider is required.");
  }
  if (!find_native_text_embedding_provider_descriptor(provider)) {
    throw ConfigurationError(path + ".provider must be one of: " +
                             allowed_values_text(native_text_embedding_provider_names()) + ".");
  }
}

void validate_image_embedding_config(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  const std::string provider = value.at("provider").as_string();
  if (provider.empty()) {
    throw ConfigurationError(path + ".provider is required.");
  }
  if (!find_native_image_embedding_provider_descriptor(provider)) {
    throw ConfigurationError(path + ".provider must be one of: " +
                             allowed_values_text(native_image_embedding_provider_names()) + ".");
  }
}

void validate_optional_string_value(const Value& value, const std::string& path) {
  if (!value.is_null() && !value.is_string()) {
    throw ConfigurationError(path + " must be a string when provided.");
  }
}

void validate_required_non_empty_string_value(const Value& value, const std::string& path) {
  if (!value.is_string() || value.as_string().empty()) {
    throw ConfigurationError(path + " must be a non-empty string.");
  }
}

void validate_optional_bool_field(const Value& value, const std::string& key, const std::string& path) {
  if (value.contains(key) && !value.at(key).is_null() && !value.at(key).is_bool()) {
    throw ConfigurationError(path + "." + key + " must be a boolean when provided.");
  }
}

void validate_optional_number_min_field(const Value& value,
                                        const std::string& key,
                                        double minimum,
                                        const std::string& minimum_text,
                                        const std::string& path) {
  if (!value.contains(key) || value.at(key).is_null()) {
    return;
  }
  const auto& item = value.at(key);
  if (!item.is_number()) {
    throw ConfigurationError(path + "." + key + " must be a number when provided.");
  }
  if (item.as_number() < minimum) {
    throw ConfigurationError(path + "." + key + " must be >= " + minimum_text + ".");
  }
}

std::string validate_config_kind(const Value& value,
                                 const std::string& path,
                                 std::initializer_list<const char*> allowed,
                                 const std::string& fallback = "memory") {
  const auto& kind_value = value.at("kind");
  if (!kind_value.is_null() && !kind_value.is_string()) {
    throw ConfigurationError(path + ".kind must be a string when provided.");
  }
  const std::string kind = kind_value.as_string(fallback);
  if (!is_one_of(kind, allowed)) {
    throw ConfigurationError(path + ".kind must be one of: " + allowed_values_text(allowed) + ".");
  }
  return kind;
}

void validate_knowledge_store_config(const Value& value, const std::string& path) {
  if (value.is_null()) {
    return;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  const auto kind = validate_config_kind(value, path, {"memory", "file"});
  if (kind == "file") {
    validate_required_non_empty_string_value(value.at("filePath"), path + ".filePath");
  }
}

void validate_knowledge_text_index_config(const Value& value, const std::string& path) {
  if (value.is_null()) {
    return;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  (void)validate_config_kind(value, path, {"memory", "minisearch"});
}

void validate_knowledge_vector_index_config(const Value& value, const std::string& path) {
  if (value.is_null()) {
    return;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  const auto kind = validate_config_kind(value, path, {"memory", "sqlite", "qdrant"});
  if (kind == "sqlite") {
    validate_required_non_empty_string_value(value.at("filePath"), path + ".filePath");
    if (!value.at("dimensions").is_number() || value.at("dimensions").as_integer() < 1) {
      throw ConfigurationError(path + ".dimensions must be a positive number.");
    }
    validate_optional_number_min_field(value, "batchSize", 1.0, "1", path);
    validate_optional_string_value(value.at("tableName"), path + ".tableName");
    validate_optional_string_value(value.at("namespace"), path + ".namespace");
  }
  if (kind == "qdrant") {
    validate_required_non_empty_string_value(value.at("baseUrl"), path + ".baseUrl");
    validate_required_non_empty_string_value(value.at("collection"), path + ".collection");
    validate_optional_number_min_field(value, "dimensions", 0.0, "0", path);
    validate_optional_number_min_field(value, "oversampleFactor", 1.0, "1", path);
    validate_optional_string_value(value.at("apiKey"), path + ".apiKey");
    validate_optional_string_value(value.at("vectorName"), path + ".vectorName");
    validate_optional_bool_field(value, "createCollection", path);
    validate_optional_bool_field(value, "wait", path);
    if (!value.at("distance").is_null()) {
      const auto distance = value.at("distance").as_string();
      if (!is_one_of(distance, {"Cosine", "Dot", "Euclid", "Manhattan"})) {
        throw ConfigurationError(path + ".distance must be one of: Cosine, Dot, Euclid, Manhattan.");
      }
    }
    if (!value.at("headers").is_null() && !value.at("headers").is_object()) {
      throw ConfigurationError(path + ".headers must be an object when provided.");
    }
  }
}

void validate_knowledge_chunking_config(const Value& value, const std::string& path) {
  validate_optional_number_min_field(value, "chunkSize", 1.0, "1", path);
  validate_optional_number_min_field(value, "chunkOverlap", 0.0, "0", path);
  validate_optional_number_min_field(value, "codeChunkLines", 1.0, "1", path);
  validate_optional_number_min_field(value, "codeChunkOverlapLines", 0.0, "0", path);
}

void validate_knowledge_ingest_options_config(const Value& value, const std::string& path) {
  if (value.is_null()) {
    return;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  validate_optional_number_min_field(value, "documentBatchSize", 1.0, "1", path);
  validate_optional_number_min_field(value, "embeddingBatchSize", 1.0, "1", path);
  validate_optional_number_min_field(value, "maxAttempts", 1.0, "1", path);
  validate_optional_number_min_field(value, "retryDelayMs", 0.0, "0", path);
  validate_optional_bool_field(value, "replaceExisting", path);
  validate_optional_bool_field(value, "skipIfUnchanged", path);
}

std::optional<int> known_text_embedding_dimensions_from_value(const Value& value);

void validate_knowledge_vector_index_dimensions(const Value& value,
                                                const Value& default_embedder,
                                                const std::string& path) {
  const auto& vector_index = value.at("vectorIndex");
  if (!vector_index.is_object() || vector_index.at("kind").as_string("memory") != "sqlite") {
    return;
  }
  const auto known_dimensions = value.at("embedder").is_object()
                                    ? known_text_embedding_dimensions_from_value(value.at("embedder"))
                                    : known_text_embedding_dimensions_from_value(default_embedder);
  if (known_dimensions && vector_index.at("dimensions").is_number() &&
      vector_index.at("dimensions").as_integer() != *known_dimensions) {
    throw ConfigurationError(path + ".vectorIndex.dimensions must match embedder dimensions (" +
                             std::to_string(*known_dimensions) + ").");
  }
}

void validate_knowledge_retrieval_options(const Value& value, const std::string& path);

void validate_knowledge_base_config(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    return;
  }
  validate_optional_string_value(value.at("id"), path + ".id");
  validate_optional_string_value(value.at("tenantId"), path + ".tenantId");
  validate_optional_string_value(value.at("title"), path + ".title");
  validate_optional_string_value(value.at("description"), path + ".description");
  validate_optional_string_value(value.at("contextTitle"), path + ".contextTitle");
  validate_optional_string_value(value.at("preset"), path + ".preset");
  validate_optional_bool_field(value, "persistent", path);
  const auto preset = value.at("preset").as_string();
  if (!preset.empty() && preset != "production") {
    throw ConfigurationError(path + ".preset must be \"production\" when provided.");
  }
  if (value.at("embedder").is_object()) {
    validate_embedding_config(value.at("embedder"), path + ".embedder");
  }
  if (value.at("imageEmbedder").is_object()) {
    validate_image_embedding_config(value.at("imageEmbedder"), path + ".imageEmbedder");
  }
  validate_knowledge_retrieval_options(value.at("searchDefaults"), path + ".searchDefaults");
  validate_knowledge_retrieval_options(value.at("retrievalConfig"), path + ".retrievalConfig");
  validate_knowledge_store_config(value.at("store"), path + ".store");
  validate_knowledge_text_index_config(value.at("textIndex"), path + ".textIndex");
  validate_knowledge_vector_index_config(value.at("vectorIndex"), path + ".vectorIndex");
  validate_knowledge_vector_index_dimensions(value, Value(), path);
  validate_knowledge_chunking_config(value, path);
  validate_knowledge_ingest_options_config(value.at("ingestOptions"), path + ".ingestOptions");
  if (!value.at("sync").is_null() && !value.at("sync").is_object()) {
    throw ConfigurationError(path + ".sync must be an object when provided.");
  }
  validate_optional_bool_field(value.at("sync"), "enabled", path + ".sync");
  validate_optional_bool_field(value.at("sync"), "deleteMissing", path + ".sync");
  if (!value.at("sync").at("stateFilePath").is_null() &&
      !value.at("sync").at("stateFilePath").is_string()) {
    throw ConfigurationError(path + ".sync.stateFilePath must be a string when provided.");
  }
}

void validate_knowledge_manager_config(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    return;
  }
  if (value.at("embedder").is_object()) {
    validate_embedding_config(value.at("embedder"), path + ".embedder");
  }
  if (value.at("imageEmbedder").is_object()) {
    validate_image_embedding_config(value.at("imageEmbedder"), path + ".imageEmbedder");
  }
  validate_optional_string_value(value.at("baseDir"), path + ".baseDir");
  validate_knowledge_text_index_config(value.at("textIndex"), path + ".textIndex");
  validate_knowledge_vector_index_config(value.at("vectorIndex"), path + ".vectorIndex");
  if (value.at("bases").is_array()) {
    std::size_t index = 0;
    for (const auto& base : value.at("bases").as_array()) {
      validate_knowledge_base_config(base, path + ".bases[" + std::to_string(index) + "]");
      validate_knowledge_vector_index_dimensions(base, value.at("embedder"),
                                                 path + ".bases[" + std::to_string(index) + "]");
      ++index;
    }
  }
}

void validate_knowledge_retrieval_options(const Value& value, const std::string& path) {
  if (value.is_null() || value.is_bool()) {
    return;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object or boolean.");
  }
  if (value.contains("topK") && value.at("topK").as_integer(1) < 0) {
    throw ConfigurationError(path + ".topK must be >= 0.");
  }
  if (value.contains("vectorTopK") && value.at("vectorTopK").as_integer(0) < 0) {
    throw ConfigurationError(path + ".vectorTopK must be >= 0.");
  }
  if (value.contains("rerankTopK") && value.at("rerankTopK").as_integer(0) < 0) {
    throw ConfigurationError(path + ".rerankTopK must be >= 0.");
  }
  if (value.contains("lexicalTopK") && value.at("lexicalTopK").as_integer(0) < 0) {
    throw ConfigurationError(path + ".lexicalTopK must be >= 0.");
  }
  if (value.contains("retrievalMode")) {
    const auto retrieval_mode = value.at("retrievalMode").as_string();
    if (retrieval_mode != "vector" && retrieval_mode != "hybrid") {
      throw ConfigurationError(path + ".retrievalMode must be \"vector\" or \"hybrid\".");
    }
  }
  if (value.contains("oversampleFactor") && value.at("oversampleFactor").as_number(1.0) < 1.0) {
    throw ConfigurationError(path + ".oversampleFactor must be >= 1.");
  }
  if (value.contains("fusion")) {
    const auto fusion = value.at("fusion").as_string();
    if (fusion != "rrf" && fusion != "weighted") {
      throw ConfigurationError(path + ".fusion must be \"rrf\" or \"weighted\".");
    }
  }
  validate_string_array_values(value.at("documentIds"), path + ".documentIds");
  validate_string_array_values(value.at("assetTypes"), path + ".assetTypes");
  validate_string_array_values(value.at("sourceTypes"), path + ".sourceTypes");
  validate_string_array_values(value.at("chunkIds"), path + ".chunkIds");
  validate_string_array_values(value.at("knowledgeBaseIds"), path + ".knowledgeBaseIds");
}

void validate_web_config(const Value& value, const std::string& path) {
  if (!value.is_object()) {
    return;
  }
  const auto& fetcher = value.at("fetcher");
  if (fetcher.is_bool() && !fetcher.as_bool()) {
    return;
  }
  const auto& browser_fallback = fetcher.at("browserFallback");
  if (!browser_fallback.is_object()) {
    return;
  }
  if (browser_fallback.contains("minimumTextLength") &&
      browser_fallback.at("minimumTextLength").as_integer(0) < 0) {
    throw ConfigurationError(path + ".fetcher.browserFallback.minimumTextLength must be >= 0.");
  }
}

void validate_planner_config(const Value& value, const std::string& path) {
  if (value.is_null() || value.is_bool()) {
    return;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object or boolean.");
  }
  const std::string kind = value.at("kind").as_string("model");
  if (!is_one_of(kind, {"model", "static"})) {
    throw ConfigurationError(path + ".kind must be one of: model, static.");
  }
  if (kind == "model" && value.at("model").is_object()) {
    validate_model_config(value.at("model"), path + ".model");
  }
  if (value.contains("maxSteps") && value.at("maxSteps").as_integer(1) < 1) {
    throw ConfigurationError(path + ".maxSteps must be >= 1.");
  }
  if (kind == "static") {
    const auto& plan = value.at("plan").is_object() ? value.at("plan") : value;
    if (!normalize_execution_plan(plan)) {
      throw ConfigurationError(path + ".plan must contain a non-empty execution plan.");
    }
  }
}

void validate_agent_resource_refs(const Value& config, const Value& definition, const std::string& agent_path) {
  const auto& memory_session = definition.at("memory").at("session");
  if (is_named_resource_ref(memory_session)) {
    (void)require_config_resource(config, "sessionStores", memory_session.at("use").as_string(), "sessionStore");
  }
  const auto& knowledge_base = definition.at("knowledge").at("base");
  if (is_named_resource_ref(knowledge_base)) {
    const auto& resource = require_config_resource(
        config, "knowledgeBases", knowledge_base.at("use").as_string(), "knowledgeBase");
    validate_knowledge_base_config(resource, "resources.knowledgeBases." + knowledge_base.at("use").as_string());
  } else if (knowledge_base.is_object()) {
    validate_knowledge_base_config(knowledge_base, agent_path + ".knowledge.base");
  }
  const auto& knowledge_manager = definition.at("knowledge").at("manager");
  if (is_named_resource_ref(knowledge_manager)) {
    const auto& resource = require_config_resource(
        config, "knowledgeManagers", knowledge_manager.at("use").as_string(), "knowledgeManager");
    validate_knowledge_manager_config(resource, "resources.knowledgeManagers." + knowledge_manager.at("use").as_string());
  } else if (knowledge_manager.is_object()) {
    validate_knowledge_manager_config(knowledge_manager, agent_path + ".knowledge.manager");
  }
  const auto& workflow_engine = definition.at("workflow").at("engine");
  if (is_named_resource_ref(workflow_engine)) {
    (void)require_config_resource(config, "workflowEngines", workflow_engine.at("use").as_string(), "workflowEngine");
  }
  const auto& permission_policy = definition.at("permissions").at("policy");
  if (is_named_resource_ref(permission_policy)) {
    (void)require_config_resource(config, "permissionPolicies", permission_policy.at("use").as_string(), "permissionPolicy");
  }
  const auto& approval_handler = definition.at("permissions").at("approval");
  if (is_named_resource_ref(approval_handler)) {
    (void)require_config_resource(config, "approvalHandlers", approval_handler.at("use").as_string(), "approvalHandler");
  }
}

ReasoningSettings reasoning_settings_from_value(const Value& value) {
  ReasoningSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  if (value.contains("enabled")) {
    settings.enabled = value.at("enabled").as_bool();
  }
  const auto& budget = value.at("budget");
  if (budget.is_string()) {
    settings.budget = budget.as_string();
  } else if (budget.is_number()) {
    settings.budget = budget.as_number();
  }
  if (value.contains("includeThoughts")) {
    settings.include_thoughts = value.at("includeThoughts").as_bool();
  }
  settings.tag_name = value.at("tagName").as_string();
  return settings;
}

ModelSettings model_settings_from_value(const Value& value) {
  ModelSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  settings.model = value.at("model").as_string();
  if (value.contains("temperature")) {
    settings.temperature = value.at("temperature").as_number();
  }
  if (value.contains("maxOutputTokens")) {
    settings.max_output_tokens = static_cast<int>(value.at("maxOutputTokens").as_integer());
  }
  if (value.at("reasoning").is_object()) {
    settings.reasoning = reasoning_settings_from_value(value.at("reasoning"));
  }
  if (value.at("extra").is_object()) {
    settings.extra = value.at("extra");
  }
  return settings;
}

ContextStatsBucketInput context_stats_bucket_input_from_value(const Value& value,
                                                              const std::string& path) {
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object.");
  }
  ContextStatsBucketInput bucket;
  bucket.id = value.at("id").as_string();
  if (bucket.id.empty()) {
    throw ConfigurationError(path + ".id is required.");
  }
  bucket.label = value.at("label").as_string(bucket.id);
  const std::string kind = value.at("kind").as_string("other");
  bucket.kind = context_stats_bucket_kind_from_string(kind, ContextStatsBucketKind::Other);
  if (to_string(bucket.kind) != kind) {
    throw ConfigurationError(path + ".kind must be one of: system_prompt, rules, tool_definitions, skills, "
                             "mcp, subagents, memory, knowledge, planning, conversation, context, other.");
  }
  bucket.text = value.at("text").as_string();
  if (bucket.text.empty()) {
    throw ConfigurationError(path + ".text is required.");
  }
  bucket.metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({});
  return bucket;
}

std::vector<ContextStatsBucketInput> context_stats_bucket_inputs_from_value(const Value& value,
                                                                            const std::string& path) {
  std::vector<ContextStatsBucketInput> buckets;
  if (value.is_null()) {
    return buckets;
  }
  if (!value.is_array()) {
    throw ConfigurationError(path + " must be an array when provided.");
  }
  buckets.reserve(value.as_array().size());
  for (std::size_t index = 0; index < value.as_array().size(); ++index) {
    buckets.push_back(context_stats_bucket_input_from_value(
        value.as_array()[index],
        path + "[" + std::to_string(index) + "]"));
  }
  return buckets;
}

RunnerRetrievalOptions retrieval_options_from_value(const Value& value) {
  RunnerRetrievalOptions options;
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("enabled")) {
    options.enabled = value.at("enabled").as_bool();
  }
  if (value.contains("topK")) {
    options.top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("topK").as_integer(4)));
  }
  if (value.contains("minScore")) {
    options.min_score = value.at("minScore").as_number(0.2);
  }
  options.namespace_id = value.at("namespace").as_string();
  return options;
}

RunnerWritebackOptions writeback_options_from_value(const Value& value) {
  RunnerWritebackOptions options;
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("enabled")) {
    options.enabled = value.at("enabled").as_bool();
  }
  options.namespace_id = value.at("namespace").as_string();
  if (value.at("metadata").is_object()) {
    options.metadata = value.at("metadata");
  }
  return options;
}

KnowledgeAssetType knowledge_asset_type_from_config(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value == "image" ? KnowledgeAssetType::Image : KnowledgeAssetType::Text;
}

std::vector<KnowledgeAssetType> knowledge_asset_types_from_value(const Value& value) {
  std::vector<KnowledgeAssetType> result;
  for (const auto& item : string_list_from_value(value)) {
    result.push_back(knowledge_asset_type_from_config(item));
  }
  return result;
}

std::map<KnowledgeAssetType, double> knowledge_modality_weights_from_value(const Value& value) {
  std::map<KnowledgeAssetType, double> result;
  if (!value.is_object()) {
    return result;
  }
  if (value.contains("text")) {
    result[KnowledgeAssetType::Text] = value.at("text").as_number(1.0);
  }
  if (value.contains("image")) {
    result[KnowledgeAssetType::Image] = value.at("image").as_number(1.0);
  }
  return result;
}

KnowledgeSearchOptions knowledge_search_options_from_value(const Value& value) {
  KnowledgeSearchOptions options;
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("topK")) {
    options.top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("topK").as_integer(0)));
  }
  if (value.contains("vectorTopK")) {
    options.vector_top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("vectorTopK").as_integer(0)));
  }
  if (value.contains("lexicalTopK")) {
    options.lexical_top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("lexicalTopK").as_integer(0)));
  }
  if (value.contains("minScore")) {
    options.min_score = value.at("minScore").as_number(0.0);
  }
  if (value.contains("hybridAlpha")) {
    options.hybrid_alpha = value.at("hybridAlpha").as_number(0.65);
  }
  if (value.contains("rerankTopK")) {
    options.rerank_top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("rerankTopK").as_integer(0)));
  }
  if (value.contains("retrievalMode")) {
    options.retrieval_mode = value.at("retrievalMode").as_string();
  }
  if (value.contains("oversampleFactor")) {
    options.oversample_factor = std::max(1.0, value.at("oversampleFactor").as_number(1.0));
  }
  if (value.contains("fusion")) {
    options.fusion = value.at("fusion").as_string();
  }
  options.modality_weights = knowledge_modality_weights_from_value(value.at("modalityWeights"));
  options.uri_prefix = value.at("uriPrefix").as_string();
  options.document_ids = string_list_from_value(value.at("documentIds"));
  options.asset_types = knowledge_asset_types_from_value(value.at("assetTypes"));
  options.space_id = value.at("spaceId").as_string();
  options.source_types = string_list_from_value(value.at("sourceTypes"));
  options.chunk_ids = string_list_from_value(value.at("chunkIds"));
  if (value.at("metadata").is_object()) {
    options.metadata = value.at("metadata");
  }
  return options;
}

KnowledgeSearchOptions knowledge_config_search_preset_options() {
  KnowledgeSearchOptions options;
  options.retrieval_mode = "hybrid";
  options.top_k = 8;
  options.vector_top_k = 24;
  options.lexical_top_k = 24;
  options.rerank_top_k = 8;
  options.hybrid_alpha = 0.5;
  options.oversample_factor = 3.0;
  options.fusion = "rrf";
  return options;
}

KnowledgeSearchOptions knowledge_base_search_defaults_from_config(const Value& value) {
  const auto preset = value.at("preset").as_string() == "production"
                          ? knowledge_config_search_preset_options()
                          : KnowledgeSearchOptions{};
  const auto search_defaults =
      merge_knowledge_search_options(preset, knowledge_search_options_from_value(value.at("searchDefaults")));
  const auto retrieval_config =
      merge_knowledge_search_options(preset, knowledge_search_options_from_value(value.at("retrievalConfig")));
  return merge_knowledge_search_options(retrieval_config, search_defaults);
}

RunnerKnowledgeRetrievalOptions knowledge_retrieval_options_from_value(const Value& value) {
  RunnerKnowledgeRetrievalOptions options;
  if (value.is_bool()) {
    options.enabled = value.as_bool();
    return options;
  }
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("enabled")) {
    options.enabled = value.at("enabled").as_bool();
  }
  if (value.contains("topK")) {
    options.top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("topK").as_integer(4)));
  }
  if (value.contains("vectorTopK")) {
    options.vector_top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("vectorTopK").as_integer(0)));
  }
  if (value.contains("lexicalTopK")) {
    options.lexical_top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("lexicalTopK").as_integer(0)));
  }
  if (value.contains("minScore")) {
    options.min_score = value.at("minScore").as_number(0.0);
  }
  if (value.contains("hybridAlpha")) {
    options.hybrid_alpha = value.at("hybridAlpha").as_number(0.65);
  }
  if (value.contains("rerankTopK")) {
    options.rerank_top_k = static_cast<std::size_t>(std::max<long long>(0, value.at("rerankTopK").as_integer(0)));
  }
  if (value.contains("retrievalMode")) {
    options.retrieval_mode = value.at("retrievalMode").as_string("hybrid");
  }
  if (value.contains("oversampleFactor")) {
    options.oversample_factor = std::max(1.0, value.at("oversampleFactor").as_number(1.0));
  }
  if (value.contains("fusion")) {
    options.fusion = value.at("fusion").as_string("weighted");
  }
  options.modality_weights = knowledge_modality_weights_from_value(value.at("modalityWeights"));
  options.uri_prefix = value.at("uriPrefix").as_string();
  options.document_ids = string_list_from_value(value.at("documentIds"));
  options.asset_types = knowledge_asset_types_from_value(value.at("assetTypes"));
  options.space_id = value.at("spaceId").as_string();
  options.source_types = string_list_from_value(value.at("sourceTypes"));
  options.chunk_ids = string_list_from_value(value.at("chunkIds"));
  options.tenant_id = value.at("tenantId").as_string();
  options.knowledge_base_ids = string_list_from_value(value.at("knowledgeBaseIds"));
  if (value.at("metadata").is_object()) {
    options.metadata = value.at("metadata");
  }
  return options;
}

SessionMemoryOptions session_memory_options_from_value(const Value& value) {
  SessionMemoryOptions options;
  if (!value.is_object()) {
    return options;
  }
  options.storage.session_id = value.at("sessionId").as_string(value.at("session_id").as_string(options.storage.session_id));
  if (value.contains("compactionBudget") && value.at("compactionBudget").is_object()) {
    const auto& bv = value.at("compactionBudget");
    if (bv.contains("maxMessages")) {
      options.compaction.compaction_budget.max_messages =
          static_cast<std::size_t>(std::max<long long>(0, bv.at("maxMessages").as_integer(0)));
    }
    if (bv.contains("maxTokens")) {
      options.compaction.compaction_budget.max_tokens =
          static_cast<std::size_t>(std::max<long long>(0, bv.at("maxTokens").as_integer(0)));
    }
  }
  options.storage.summary_label = value.at("summaryLabel").as_string(options.storage.summary_label);
  options.storage.summary = value.at("summary").as_string();
  if (value.at("messages").is_array()) {
    for (const auto& item : value.at("messages").as_array()) {
      options.storage.messages.push_back(agent_message_from_value(item));
    }
  }
  return options;
}

std::shared_ptr<SessionStore> session_store_from_value(
    const Value& root,
    const Value& session,
    const NativeSessionMemoryConfigurator& configure_session_memory = {}) {
  if (!session.is_object()) {
    SessionMemoryOptions session_options;
    if (configure_session_memory) {
      configure_session_memory(session_options, session);
    }
    return std::make_shared<InMemorySessionStore>(InMemorySessionStoreConfig{
        .session_options = std::move(session_options),
    });
  }

  const auto& resolved_session = [&]() -> const Value& {
    const std::string ref = session.at("use").as_string();
    if (ref.empty()) {
      return session;
    }
    const auto& stores = root.at("resources").at("sessionStores");
    if (!stores.is_object() || !stores.contains(ref)) {
      throw ConfigurationError("Unknown session store resource \"" + ref + "\".");
    }
    return stores.at(ref);
  }();

  const std::string kind = resolved_session.at("kind").as_string("memory");
  auto session_options = session_memory_options_from_value(resolved_session.at("sessionOptions"));
  if (configure_session_memory) {
    configure_session_memory(session_options, resolved_session);
  }
  if (kind == "memory") {
    return std::make_shared<InMemorySessionStore>(InMemorySessionStoreConfig{
        .session_options = std::move(session_options),
    });
  }
  if (kind == "file") {
    const std::string base_dir = resolved_session.at("baseDir").as_string();
    if (base_dir.empty()) {
      throw ConfigurationError("File session store requires baseDir.");
    }
    return std::make_shared<FileSessionStore>(FileSessionStoreConfig{
        .base_dir = resolve_config_path(root, base_dir),
        .file_extension = resolved_session.at("fileExtension").as_string(".json"),
        .session_options = std::move(session_options),
    });
  }
  throw ConfigurationError("Unsupported session store kind \"" + kind + "\".");
}

std::shared_ptr<LongTermMemory> long_term_memory_from_value(const Value& root, const Value& value) {
  if (!value.is_object()) {
    return nullptr;
  }
  const std::string kind = value.at("kind").as_string("memory");
  const std::string namespace_id = value.at("namespace").as_string("default");
  std::shared_ptr<VectorStore> store;
  if (kind == "memory") {
    store = std::make_shared<InMemoryVectorStore>(namespace_id);
  } else if (kind == "file") {
    const std::string file_path = value.at("filePath").as_string();
    if (file_path.empty()) {
      throw ConfigurationError("File longTermMemory requires filePath.");
    }
    store = std::make_shared<FileVectorStore>(resolve_config_path(root, file_path), namespace_id);
  } else {
    throw ConfigurationError("Unsupported longTermMemory kind \"" + kind + "\".");
  }
  const std::size_t top_k =
      value.contains("topK") ? static_cast<std::size_t>(std::max<long long>(0, value.at("topK").as_integer(4))) : 4;
  const double min_score = value.at("minScore").as_number(0.2);
  const bool auto_remember = value.contains("autoRemember") ? value.at("autoRemember").as_bool() : true;
  const std::string context_title = value.at("contextTitle").as_string("Long-term memory retrieval");
  return std::make_shared<LongTermMemory>(
      std::make_shared<HashEmbeddingAdapter>(), std::move(store), namespace_id, top_k, min_score,
      auto_remember, context_title);
}

std::filesystem::path skills_cwd_from_value(const Value& root, const Value& anthropic) {
  const std::string configured = anthropic.at("cwd").as_string();
  if (configured.empty() || configured == ".") {
    const std::string cwd = root.at("_cwd").as_string();
    return cwd.empty() ? std::filesystem::current_path() : std::filesystem::path(cwd);
  }
  return resolve_config_path(root, configured);
}

std::shared_ptr<SkillRegistry> skills_registry_from_value(const Value& root, const Value& value) {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto& anthropic = value.at("anthropic");
  if (!anthropic.is_object()) {
    return nullptr;
  }
  AnthropicSkillRegistryLoadOptions options;
  options.cwd = skills_cwd_from_value(root, anthropic);
  options.include_user = anthropic.contains("includeUser") ? anthropic.at("includeUser").as_bool() : true;
  return std::make_shared<SkillRegistry>(load_anthropic_skill_registry(std::move(options)));
}

std::filesystem::path mcp_cwd_from_value(const Value& root, const Value& anthropic) {
  const std::string configured = anthropic.at("cwd").as_string();
  if (configured.empty() || configured == ".") {
    const std::string cwd = root.at("_cwd").as_string();
    return cwd.empty() ? std::filesystem::current_path() : std::filesystem::path(cwd);
  }
  return resolve_config_path(root, configured);
}

void append_unique_tools(std::vector<ToolDefinition>& tools, std::vector<ToolDefinition> incoming) {
  std::set<std::string> names;
  for (const auto& tool : tools) {
    names.insert(tool.name);
  }
  for (auto& tool : incoming) {
    if (names.insert(tool.name).second) {
      tools.push_back(std::move(tool));
    }
  }
}

std::vector<ToolDefinition> mcp_tools_from_value(const Value& root,
                                                 const Value& value,
                                                 const NativeMCPTransportFactory& transport_factory,
                                                 std::vector<std::shared_ptr<MCPClient>>& clients) {
  if (!value.is_object()) {
    return {};
  }
  const auto& anthropic = value.at("anthropic");
  if (!anthropic.is_object()) {
    return {};
  }

  std::optional<std::filesystem::path> config_file;
  const std::string configured_file = anthropic.at("file").as_string();
  if (!configured_file.empty()) {
    config_file = resolve_config_path(root, configured_file);
  } else {
    config_file = find_anthropic_mcp_config_file(mcp_cwd_from_value(root, anthropic));
  }
  if (!config_file) {
    return {};
  }

  const auto loaded = load_anthropic_mcp_config_file(*config_file);
  const auto selected_names = string_array_from_value(anthropic.at("servers"));
  const std::set<std::string> selected(selected_names.begin(), selected_names.end());

  std::vector<ToolDefinition> tools;
  for (const auto& [name, server] : loaded.mcp_servers) {
    if (!selected.empty() && selected.find(name) == selected.end()) {
      continue;
    }
    auto transport = transport_factory ? transport_factory(server) : create_native_mcp_transport(server);
    if (!transport) {
      throw ConfigurationError("MCP transport factory returned null for server \"" + name + "\".");
    }
    auto client = std::make_shared<MCPClient>(server.name, std::move(transport));
    client->connect();
    auto server_tools = create_mcp_tool_definitions(client);
    for (auto& tool : server_tools) {
      tools.push_back(std::move(tool));
    }
    clients.push_back(std::move(client));
  }
  return tools;
}

const Value& resolve_config_resource_ref(const Value& root,
                                         const Value& value,
                                         const std::string& collection,
                                         const std::string& type_name) {
  const std::string ref = value.at("use").as_string();
  if (ref.empty()) {
    return value;
  }
  const auto& resources = root.at("resources").at(collection);
  if (!resources.is_object() || !resources.contains(ref)) {
    throw ConfigurationError("Unknown " + type_name + " resource \"" + ref + "\".");
  }
  return resources.at(ref);
}

std::vector<ToolRiskLevel> risk_levels_from_value(const Value& value) {
  std::vector<ToolRiskLevel> result;
  for (const auto& item : string_list_from_value(value)) {
    result.push_back(tool_risk_level_from_string(item));
  }
  return result;
}

ToolPermissionMatcher permission_matcher_from_value(const Value& value) {
  ToolPermissionMatcher matcher;
  if (!value.is_object()) {
    return matcher;
  }
  matcher.tool_names = string_list_from_value(value.at("toolName"));
  matcher.capabilities = string_list_from_value(value.at("capabilities"));
  matcher.tags = string_list_from_value(value.at("tags"));
  matcher.risk_levels = risk_levels_from_value(value.at("riskLevel"));
  if (value.at("builtin").is_bool()) {
    matcher.builtin = value.at("builtin").as_bool();
  }
  matcher.bundles = string_list_from_value(value.at("bundle"));
  return matcher;
}

std::vector<ToolPermissionRule> permission_rules_from_value(const Value& value) {
  std::vector<ToolPermissionRule> rules;
  if (!value.is_array()) {
    return rules;
  }
  for (const auto& item : value.as_array()) {
    if (!item.is_object()) {
      continue;
    }
    rules.push_back(ToolPermissionRule{
        .match = permission_matcher_from_value(item.at("match")),
        .decision = permission_decision_kind_from_string(item.at("decision").as_string()),
        .reason = item.at("reason").as_string(),
    });
  }
  return rules;
}

PermissionPolicy permission_policy_from_value(const Value& root, const Value& value) {
  if (!value.is_object()) {
    return {};
  }
  const auto& resolved = resolve_config_resource_ref(root, value, "permissionPolicies", "permissionPolicy");
  const std::string kind = resolved.at("kind").as_string("rule-based");
  if (kind == "capability") {
    CapabilityPermissionPolicyConfig config;
    config.allow = string_list_from_value(resolved.at("allow"));
    config.deny = string_list_from_value(resolved.at("deny"));
    config.ask = string_list_from_value(resolved.at("ask"));
    config.high_risk_mode = permission_decision_kind_from_string(
        resolved.at("highRiskMode").as_string(), PermissionDecisionKind::Ask);
    return create_capability_policy(std::move(config));
  }
  if (kind == "rule-based" || kind.empty()) {
    RuleBasedPermissionPolicyConfig config;
    config.rules = permission_rules_from_value(resolved.at("rules"));
    config.default_decision = permission_decision_kind_from_string(
        resolved.at("defaultDecision").as_string(), PermissionDecisionKind::Deny);
    config.default_reason = resolved.at("defaultReason").as_string();
    return create_rule_based_permission_policy(std::move(config));
  }
  throw ConfigurationError("Unsupported permission policy kind \"" + kind + "\".");
}

PermissionApprovalHandler approval_handler_from_value(const Value& root, const Value& value) {
  if (!value.is_object()) {
    return {};
  }
  const auto& resolved = resolve_config_resource_ref(root, value, "approvalHandlers", "approvalHandler");
  const std::string kind = resolved.at("kind").as_string("static");
  if (kind == "static" || kind.empty()) {
    return create_static_approval_handler(PermissionDecision{
        .decision = permission_decision_kind_from_string(resolved.at("decision").as_string(),
                                                         PermissionDecisionKind::Allow),
        .reason = resolved.at("reason").as_string(),
    });
  }
  if (kind == "cli") {
    CliApprovalHandlerConfig config;
    config.default_decision = permission_decision_kind_from_string(
        resolved.at("defaultDecision").as_string(), PermissionDecisionKind::Deny);
    return create_cli_approval_handler(std::move(config));
  }
  throw ConfigurationError("Unsupported approval handler kind \"" + kind + "\".");
}

std::shared_ptr<WorkflowStore> workflow_store_from_value(const Value& root, const Value& value) {
  if (!value.is_object()) {
    return std::make_shared<InMemoryWorkflowStore>();
  }
  const std::string kind = value.at("kind").as_string("memory");
  if (kind == "memory") {
    return std::make_shared<InMemoryWorkflowStore>();
  }
  if (kind == "file") {
    const std::string base_dir = value.at("baseDir").as_string();
    if (base_dir.empty()) {
      throw ConfigurationError("File workflow store requires baseDir.");
    }
    return std::make_shared<FileWorkflowStore>(FileWorkflowStoreConfig{
        .base_dir = resolve_config_path(root, base_dir),
    });
  }
  throw ConfigurationError("Unsupported workflow store kind \"" + kind + "\".");
}

Value crawler_profile_from_value(const Value& value) {
  if (!value.is_object()) {
    return Value::object({});
  }
  Value::Object profile;
  if (value.contains("maxDepth")) {
    profile["maxDepth"] = value.at("maxDepth");
  }
  if (value.contains("maxPages")) {
    profile["maxPages"] = value.at("maxPages");
  }
  if (value.contains("concurrency")) {
    profile["concurrency"] = value.at("concurrency");
  }
  if (value.contains("allowedDomains")) {
    profile["allowedDomains"] = value.at("allowedDomains");
  }
  if (value.contains("obeyRobots")) {
    profile["obeyRobots"] = value.at("obeyRobots");
  }
  if (value.contains("discoverSitemap")) {
    profile["discoverSitemap"] = value.at("discoverSitemap");
  }
  return Value(std::move(profile));
}

bool has_bundle(const std::vector<std::string>& bundles, const std::string& bundle) {
  return std::find(bundles.begin(), bundles.end(), bundle) != bundles.end();
}

std::shared_ptr<NativeWebRuntime> web_runtime_from_value(const Value& value,
                                                         const std::vector<std::string>& bundles,
                                                         NativeWebAdapters adapters,
                                                         const std::shared_ptr<NativeBrowserRuntime>& browser) {
  const bool wants_web_bundle = has_bundle(bundles, "web");
  const bool has_web_config = value.is_object();
  if (!wants_web_bundle && !has_web_config) {
    return nullptr;
  }

  auto runtime = std::make_shared<NativeWebRuntime>();
  runtime->search_registry = std::make_shared<WebSearchProviderRegistry>();
  if (!adapters.search_transport) {
    adapters.search_transport = create_native_web_search_transport();
  }
  if (!adapters.fetch_transport) {
    adapters.fetch_transport = create_native_web_fetch_transport();
  }

  if (value.at("search").is_array()) {
    for (const auto& provider : value.at("search").as_array()) {
      if (!provider.is_object()) {
        continue;
      }
      const std::string kind = provider.at("kind").as_string("brave");
      if (kind == "brave") {
        auto resolved = create_brave_web_search_provider(
            adapters.search_transport,
            resolve_defaulted_env_config_value(provider.at("apiKey"), provider.at("apiKeyEnv"),
                                               "BRAVE_SEARCH_API_KEY", {}),
            resolve_defaulted_env_config_value(provider.at("baseUrl"), provider.at("baseUrlEnv"),
                                               "BRAVE_SEARCH_BASE_URL",
                                               "https://api.search.brave.com/res/v1"));
        const auto name = resolved.name();
        runtime->search_registry->register_provider(std::move(resolved));
        if (runtime->default_search_provider.empty() || provider.at("default").as_bool()) {
          runtime->default_search_provider = name;
        }
        continue;
      }
      if (kind == "searxng") {
        auto resolved = create_searxng_web_search_provider(
            adapters.search_transport,
            resolve_env_config_value(provider.at("baseUrl"), provider.at("baseUrlEnv")));
        const auto name = resolved.name();
        runtime->search_registry->register_provider(std::move(resolved));
        if (runtime->default_search_provider.empty() || provider.at("default").as_bool()) {
          runtime->default_search_provider = name;
        }
        continue;
      }
      throw ConfigurationError("Unsupported web search provider kind \"" + kind + "\".");
    }
  }

  const auto& fetcher_config = value.at("fetcher");
  const bool fetcher_disabled = fetcher_config.is_bool() && !fetcher_config.as_bool();
  const auto& crawler_config = value.at("crawler");
  const bool crawler_disabled = crawler_config.is_bool() && !crawler_config.as_bool();
  const bool wants_fetcher = wants_web_bundle || fetcher_config.is_object() ||
                             (crawler_config.is_object() && !crawler_disabled);
  if (!fetcher_disabled && wants_fetcher) {
    runtime->fetcher = std::make_shared<NativeWebPageFetcher>(std::move(adapters.fetch_transport));
    const auto& browser_fallback = fetcher_config.at("browserFallback");
    const bool browser_fallback_disabled = browser_fallback.is_bool() && !browser_fallback.as_bool();
    const bool wants_browser_fallback = browser_fallback.is_object() ||
                                        (browser_fallback.is_bool() && browser_fallback.as_bool());
    if (!browser_fallback_disabled && wants_browser_fallback) {
      if (!browser || !browser->renderer) {
        throw ConfigurationError("web.fetcher.browserFallback requires a configured browser renderer.");
      }
      BrowserRenderRequest browser_request;
      browser_request.selector = browser_fallback.at("selector").as_string();
      const auto wait_until = browser_fallback.at("waitUntil").as_string();
      if (!wait_until.empty()) {
        browser_request.wait_until = wait_until;
      }
      browser_request.timeout_ms = static_cast<int>(browser_fallback.at("timeoutMs").as_integer(0));
      runtime->fetcher = std::make_shared<BrowserBackedWebPageFetcher>(BrowserBackedWebPageFetcherConfig{
          .fetcher = runtime->fetcher,
          .browser = browser->renderer,
          .minimum_text_length = static_cast<int>(std::max<long long>(
              0, browser_fallback.at("minimumTextLength").as_integer(160))),
          .browser_request = std::move(browser_request),
      });
    }
  }
  if (!crawler_disabled && crawler_config.is_object()) {
    runtime->default_crawler_profile = crawler_profile_from_value(crawler_config);
    runtime->crawler = std::make_shared<NativeWebCrawler>(runtime->fetcher.get());
  }
  return runtime;
}

std::shared_ptr<NativeBrowserRuntime> browser_runtime_from_value(const Value& value,
                                                                 const std::vector<std::string>& bundles,
                                                                 NativeBrowserAdapters adapters) {
  const bool wants_browser_bundle = has_bundle(bundles, "browser");
  const auto& renderer_config = value.at("renderer");
  if (renderer_config.is_bool() && !renderer_config.as_bool()) {
    return nullptr;
  }

  const bool has_renderer_config = renderer_config.is_object();
  const bool wants_renderer = wants_browser_bundle || has_renderer_config || adapters.renderer != nullptr;
  if (!wants_renderer) {
    return nullptr;
  }
  if (!adapters.renderer) {
    if (has_renderer_config) {
      throw ConfigurationError("browser.renderer requires a NativeBrowserAdapters.renderer.");
    }
    return nullptr;
  }

  auto runtime = std::make_shared<NativeBrowserRuntime>();
  runtime->renderer = std::move(adapters.renderer);
  return runtime;
}

std::vector<Value> value_array_copy(const Value& value) {
  if (!value.is_array()) {
    return {};
  }
  return value.as_array();
}

bool is_production_knowledge_preset(const Value& value) {
  return value.at("preset").as_string() == "production";
}

bool effective_knowledge_persistent(const Value& value, bool default_persistent) {
  return value.contains("persistent") ? value.at("persistent").as_bool() : default_persistent;
}

std::optional<int> known_text_embedding_dimensions_from_value(const Value& value) {
  if (!value.is_object()) {
    return std::nullopt;
  }
  if (value.at("dimensions").is_number()) {
    return static_cast<int>(std::max<long long>(0, value.at("dimensions").as_integer()));
  }
  return value.at("provider").as_string() == "hash" ? std::optional<int>(256) : std::nullopt;
}

std::optional<int> effective_text_embedding_dimensions(const Value& value, const Value& default_embedder) {
  if (value.at("embedder").is_object()) {
    return known_text_embedding_dimensions_from_value(value.at("embedder"));
  }
  if (default_embedder.is_object()) {
    return known_text_embedding_dimensions_from_value(default_embedder);
  }
  return 256;
}

Value production_sqlite_vector_index_config(const std::string& tenant_id,
                                            const std::string& id,
                                            int dimensions) {
  return Value::object({
      {"kind", "sqlite"},
      {"filePath", ".node-agent/knowledge/" + tenant_id + "/" + id + "/vectors.sqlite"},
      {"dimensions", dimensions},
  });
}

std::shared_ptr<KnowledgeStore> knowledge_store_from_value(const Value& root,
                                                           const Value& value,
                                                           const std::string& tenant_id,
                                                           const std::string& id,
                                                           bool persistent) {
  if (value.is_object()) {
    const std::string kind = value.at("kind").as_string("memory");
    if (kind == "memory") {
      return std::make_shared<InMemoryKnowledgeStore>();
    }
    if (kind == "file") {
      const std::string file_path = value.at("filePath").as_string();
      if (file_path.empty()) {
        throw ConfigurationError("File knowledge store requires filePath.");
      }
      return std::make_shared<FileKnowledgeStore>(resolve_config_path(root, file_path));
    }
    throw ConfigurationError("Unsupported knowledge store kind \"" + kind + "\".");
  }
  if (persistent) {
    const std::string cwd = root.at("_cwd").as_string();
    const auto base = cwd.empty() ? std::filesystem::current_path() : std::filesystem::path(cwd);
    return std::make_shared<FileKnowledgeStore>(base / ".node-agent" / "knowledge" / tenant_id / id / "store.json");
  }
  return std::make_shared<InMemoryKnowledgeStore>();
}

std::shared_ptr<KnowledgeTextIndex> knowledge_text_index_from_value(const Value& value, bool production_preset = false) {
  if (!value.is_object()) {
    if (production_preset) {
      return std::make_shared<MiniSearchKnowledgeTextIndex>();
    }
    return std::make_shared<InMemoryKnowledgeTextIndex>();
  }
  const std::string kind = value.at("kind").as_string("memory");
  if (kind == "memory") {
    return std::make_shared<InMemoryKnowledgeTextIndex>();
  }
  if (kind == "minisearch") {
    return std::make_shared<MiniSearchKnowledgeTextIndex>();
  }
  throw ConfigurationError("Unsupported knowledge text index kind \"" + kind + "\".");
}

std::shared_ptr<KnowledgeVectorIndex> knowledge_vector_index_from_value(const Value& root, const Value& value) {
  if (!value.is_object()) {
    return nullptr;
  }
  const std::string kind = value.at("kind").as_string("memory");
  if (kind == "memory") {
    return std::make_shared<InMemoryKnowledgeVectorIndex>();
  }
  if (kind == "sqlite") {
    const auto file_path = value.at("filePath").as_string();
    if (file_path.empty()) {
      throw ConfigurationError("File-backed knowledge vector index requires filePath.");
    }
    return std::make_shared<SqliteKnowledgeVectorIndex>(SqliteKnowledgeVectorIndexConfig{
        .file_path = resolve_config_path(root, file_path),
        .dimensions = static_cast<int>(std::max<long long>(0, value.at("dimensions").as_integer(0))),
        .table_name = value.at("tableName").as_string("knowledge_vectors"),
        .create_table = value.at("createTable").as_bool(true),
        .namespace_id = value.at("namespace").as_string(),
        .batch_size = static_cast<std::size_t>(std::max<long long>(1, value.at("batchSize").as_integer(200))),
    });
  }
  if (kind == "qdrant") {
    return std::make_shared<QdrantKnowledgeVectorIndex>(QdrantKnowledgeVectorIndexConfig{
        .base_url = value.at("baseUrl").as_string(),
        .collection = value.at("collection").as_string(),
        .api_key = value.at("apiKey").as_string(),
        .headers = string_map_from_value(value.at("headers")),
        .vector_name = value.at("vectorName").as_string(),
        .create_collection = value.at("createCollection").as_bool(false),
        .distance = value.at("distance").as_string("Cosine"),
        .dimensions = static_cast<int>(std::max<long long>(0, value.at("dimensions").as_integer(0))),
        .wait = value.at("wait").as_bool(true),
        .oversample_factor = static_cast<std::size_t>(std::max<long long>(1, value.at("oversampleFactor").as_integer(4))),
        .transport = create_native_http_transport(),
    });
  }
  throw ConfigurationError("Unsupported knowledge vector index kind \"" + kind + "\".");
}

std::shared_ptr<KnowledgeVectorIndex> knowledge_vector_index_from_config(const Value& root,
                                                                         const Value& value,
                                                                         const Value& default_embedder,
                                                                         const std::string& tenant_id,
                                                                         const std::string& id,
                                                                         bool persistent,
                                                                         bool production_preset,
                                                                         const std::string& path) {
  const auto& vector_index = value.at("vectorIndex");
  if (vector_index.is_object()) {
    const auto kind = vector_index.at("kind").as_string("memory");
    if (kind == "sqlite") {
      const auto known_dimensions = effective_text_embedding_dimensions(value, default_embedder);
      if (!vector_index.at("dimensions").is_number() || vector_index.at("dimensions").as_integer() < 1) {
        throw ConfigurationError(path + ".vectorIndex.dimensions must be a positive number.");
      }
      if (known_dimensions && vector_index.at("dimensions").is_number() &&
          vector_index.at("dimensions").as_integer() != *known_dimensions) {
        throw ConfigurationError(path + ".vectorIndex.dimensions must match embedder dimensions (" +
                                 std::to_string(*known_dimensions) + ").");
      }
    }
    return knowledge_vector_index_from_value(root, vector_index);
  }
  if (!production_preset) {
    return nullptr;
  }
  if (!persistent) {
    return std::make_shared<InMemoryKnowledgeVectorIndex>();
  }
  const auto dimensions = effective_text_embedding_dimensions(value, default_embedder);
  if (!dimensions) {
    const auto provider = value.at("embedder").is_object()
                              ? value.at("embedder").at("provider").as_string()
                              : default_embedder.at("provider").as_string();
    throw ConfigurationError(path + ": Knowledge preset \"production\" requires embedder.dimensions or vectorIndex.dimensions "
                             "when using persistent storage with embedder provider \"" + provider + "\".");
  }
  return knowledge_vector_index_from_value(
      root,
      production_sqlite_vector_index_config(tenant_id, id, *dimensions));
}

RecursiveTextChunker knowledge_chunker_from_value(const Value& value) {
  const bool production_preset = is_production_knowledge_preset(value);
  const auto default_chunk_size = production_preset ? 1000 : 1200;
  const auto default_chunk_overlap = production_preset ? 120 : 180;
  const auto chunk_size =
      static_cast<std::size_t>(std::max<long long>(1, value.at("chunkSize").as_integer(default_chunk_size)));
  const auto chunk_overlap =
      static_cast<std::size_t>(std::max<long long>(0, value.at("chunkOverlap").as_integer(default_chunk_overlap)));
  const auto code_chunk_lines =
      static_cast<std::size_t>(std::max<long long>(1, value.at("codeChunkLines").as_integer(80)));
  const auto code_chunk_overlap_lines =
      static_cast<std::size_t>(std::max<long long>(0, value.at("codeChunkOverlapLines").as_integer(12)));
  return RecursiveTextChunker(chunk_size, chunk_overlap, code_chunk_lines, code_chunk_overlap_lines);
}

KnowledgeIngestionPipelineOptions knowledge_ingest_options_from_value(const Value& value) {
  KnowledgeIngestionPipelineOptions options;
  if (!value.is_object()) {
    return options;
  }
  if (value.contains("documentBatchSize")) {
    options.document_batch_size =
        static_cast<std::size_t>(std::max<long long>(1, value.at("documentBatchSize").as_integer(16)));
  }
  if (value.contains("embeddingBatchSize")) {
    options.embedding_batch_size =
        static_cast<std::size_t>(std::max<long long>(1, value.at("embeddingBatchSize").as_integer(0)));
  }
  if (value.contains("maxAttempts")) {
    options.max_attempts =
        static_cast<std::size_t>(std::max<long long>(1, value.at("maxAttempts").as_integer(2)));
  }
  if (value.contains("retryDelayMs")) {
    options.retry_delay_ms = static_cast<int>(std::max<long long>(0, value.at("retryDelayMs").as_integer(0)));
  }
  if (value.contains("replaceExisting")) {
    options.replace_existing = value.at("replaceExisting").as_bool();
  }
  if (value.contains("skipIfUnchanged")) {
    options.skip_if_unchanged = value.at("skipIfUnchanged").as_bool();
  }
  return options;
}

std::filesystem::path default_knowledge_sync_state_path(const Value& root,
                                                        const std::string& tenant_id,
                                                        const std::string& id) {
  const std::string cwd = root.at("_cwd").as_string();
  const auto base = cwd.empty() ? std::filesystem::current_path() : std::filesystem::path(cwd);
  return base / ".node-agent" / "knowledge-sync" / tenant_id / (id + ".json");
}

void ingest_knowledge_sources_from_config(const Value& root,
                                          const Value& config,
                                          KnowledgeBase& base,
                                          const KnowledgeSourceLoader& loader,
                                          const std::string& tenant_id,
                                          const std::string& id) {
  const auto sources = value_array_copy(config.at("sources"));
  if (sources.empty()) {
    return;
  }

  const auto ingest_options = knowledge_ingest_options_from_value(config.at("ingestOptions"));
  const auto& sync = config.at("sync");
  const bool production_default_sync = is_production_knowledge_preset(config) && !sync.is_object();
  const bool sync_enabled = sync.is_object() ? sync.at("enabled").as_bool(false) : production_default_sync;
  if (!sync_enabled) {
    KnowledgeIngestionPipeline pipeline(base, loader);
    (void)pipeline.ingest(sources, ingest_options);
    return;
  }

  KnowledgeSyncOptions sync_options;
  static_cast<KnowledgeIngestionPipelineOptions&>(sync_options) = ingest_options;
  sync_options.delete_missing = sync.is_object() ? sync.at("deleteMissing").as_bool(false) : false;

  std::unique_ptr<KnowledgeSyncStateStore> state_store;
  const auto state_file_path = sync.at("stateFilePath").as_string();
  if (!state_file_path.empty()) {
    state_store = std::make_unique<FileKnowledgeSyncStateStore>(resolve_config_path(root, state_file_path));
  } else if (production_default_sync) {
    state_store = std::make_unique<FileKnowledgeSyncStateStore>(default_knowledge_sync_state_path(root, tenant_id, id));
  } else {
    state_store = std::make_unique<InMemoryKnowledgeSyncStateStore>();
  }

  KnowledgeSyncJob sync_job(base, loader, *state_store);
  (void)sync_job.sync(sources, sync_options);
}

std::shared_ptr<TextEmbeddingAdapter> text_embedding_adapter_from_value(const Value& value,
                                                                        NativeProviderTransport transport,
                                                                        NativeLlamaCppAdapters llama_adapters) {
  if (!value.is_object()) {
    return std::make_shared<HashEmbeddingAdapter>();
  }
  const std::string provider = value.at("provider").as_string();
  const auto descriptor = find_native_text_embedding_provider_descriptor(provider);
  if (!descriptor) {
    throw ConfigurationError("Unsupported embedding provider \"" + provider + "\".");
  }
  auto adapter = descriptor->create_adapter(value, NativeEmbeddingProviderBuildContext{
                                                       .transport = std::move(transport),
                                                       .llama_adapters = std::move(llama_adapters),
                                                   });
  if (!adapter) {
    throw ConfigurationError("Embedding provider factory returned null for \"" + provider + "\".");
  }
  return adapter;
}

std::shared_ptr<ImageEmbeddingAdapter> image_embedding_adapter_from_value(const Value& value) {
  if (!value.is_object()) {
    return std::make_shared<HashImageEmbeddingAdapter>();
  }
  const std::string provider = value.at("provider").as_string();
  const auto descriptor = find_native_image_embedding_provider_descriptor(provider);
  if (!descriptor) {
    throw ConfigurationError("Unsupported image embedding provider \"" + provider + "\".");
  }
  auto adapter = descriptor->create_adapter(value);
  if (!adapter) {
    throw ConfigurationError("Image embedding provider factory returned null for \"" + provider + "\".");
  }
  return adapter;
}

std::shared_ptr<KnowledgeSourceLoader> knowledge_loader_for_web(const std::shared_ptr<NativeWebRuntime>& web,
                                                                const std::shared_ptr<NativeBrowserRuntime>& browser) {
  return create_web_enabled_knowledge_loader(web && web->fetcher ? web->fetcher.get() : nullptr,
                                             web && web->crawler ? web->crawler.get() : nullptr,
                                             browser && browser->renderer ? browser->renderer.get() : nullptr);
}

std::shared_ptr<KnowledgeBase> knowledge_base_from_value(const Value& root,
                                                         const Value& value,
                                                         const std::shared_ptr<KnowledgeSourceLoader>& loader,
                                                         NativeProviderTransport provider_transport,
                                                         NativeLlamaCppAdapters llama_adapters) {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto& resolved = resolve_config_resource_ref(root, value, "knowledgeBases", "knowledgeBase");
  const std::string id = resolved.at("id").as_string();
  if (id.empty()) {
    throw ConfigurationError("Knowledge base config requires id.");
  }
  const std::string tenant_id = resolved.at("tenantId").as_string("default");
  const bool persistent = effective_knowledge_persistent(resolved, false);
  const bool production_preset = is_production_knowledge_preset(resolved);
  auto base = std::make_shared<KnowledgeBase>(
      id,
      tenant_id,
      resolved.at("title").as_string(id),
      text_embedding_adapter_from_value(resolved.at("embedder"), provider_transport, llama_adapters),
      knowledge_chunker_from_value(resolved),
      std::make_shared<HeuristicKnowledgeReranker>(),
      knowledge_store_from_value(root, resolved.at("store"), tenant_id, id, persistent),
      image_embedding_adapter_from_value(resolved.at("imageEmbedder")),
      knowledge_text_index_from_value(resolved.at("textIndex"), production_preset),
      knowledge_vector_index_from_config(root, resolved, Value(), tenant_id, id, persistent, production_preset,
                                         "knowledgeBases." + id),
      knowledge_base_search_defaults_from_config(resolved),
      resolved.at("description").as_string(),
      resolved.at("contextTitle").as_string());

  ingest_knowledge_sources_from_config(root, resolved, *base, *loader, tenant_id, id);
  return base;
}

std::shared_ptr<KnowledgeBaseManager> knowledge_manager_from_value(const Value& root,
                                                                   const Value& value,
                                                                   const std::shared_ptr<KnowledgeSourceLoader>& loader,
                                                                   NativeProviderTransport provider_transport,
                                                                   NativeLlamaCppAdapters llama_adapters) {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto& resolved = resolve_config_resource_ref(root, value, "knowledgeManagers", "knowledgeManager");
  const std::string base_dir = resolved.at("baseDir").as_string();
  auto manager = std::make_shared<KnowledgeBaseManager>(
      base_dir.empty() ? std::filesystem::path{} : resolve_config_path(root, base_dir),
      text_embedding_adapter_from_value(resolved.at("embedder"), provider_transport, llama_adapters),
      image_embedding_adapter_from_value(resolved.at("imageEmbedder")),
      loader,
      std::make_shared<HeuristicKnowledgeReranker>(),
      knowledge_text_index_from_value(resolved.at("textIndex")),
      knowledge_vector_index_from_config(root, resolved, Value(), "default", "manager", false, false,
                                         "knowledgeManager"));

  for (const auto& item : resolved.at("bases").as_array()) {
    if (!item.is_object()) {
      continue;
    }
    const std::string id = item.at("id").as_string();
    if (id.empty()) {
      throw ConfigurationError("Managed knowledge base config requires id.");
    }
    const std::string tenant_id = item.at("tenantId").as_string("default");
    const bool persistent = effective_knowledge_persistent(item, !base_dir.empty());
    const bool production_preset = is_production_knowledge_preset(item);
    auto base = manager->create_knowledge_base(ManagedKnowledgeBaseConfig{
        .id = id,
        .tenant_id = tenant_id,
        .title = item.at("title").as_string(id),
        .description = item.at("description").as_string(),
        .persistent = item.contains("persistent") ? std::optional<bool>(item.at("persistent").as_bool()) : std::nullopt,
        .store = item.at("store").is_object()
                     ? knowledge_store_from_value(root, item.at("store"), tenant_id, id, persistent)
                     : nullptr,
        .embedder = item.at("embedder").is_object()
                        ? text_embedding_adapter_from_value(item.at("embedder"), provider_transport, llama_adapters)
                        : nullptr,
        .image_embedder = item.at("imageEmbedder").is_object()
                              ? image_embedding_adapter_from_value(item.at("imageEmbedder"))
                              : nullptr,
        .chunker = knowledge_chunker_from_value(item),
        .reranker = std::make_shared<HeuristicKnowledgeReranker>(),
        .text_index = (item.at("textIndex").is_object() || production_preset)
                          ? knowledge_text_index_from_value(item.at("textIndex"), production_preset)
                          : nullptr,
        .vector_index = knowledge_vector_index_from_config(
            root, item, resolved.at("embedder"), tenant_id, id, persistent, production_preset,
            "knowledgeManagers." + id),
        .search_defaults = knowledge_base_search_defaults_from_config(item),
        .context_title = item.at("contextTitle").as_string(),
    });
    ingest_knowledge_sources_from_config(root, item, *base, *loader, tenant_id, id);
  }
  return manager;
}

std::shared_ptr<NativeKnowledgeRuntime> knowledge_runtime_from_value(const Value& root,
                                                                     const Value& value,
                                                                     const std::shared_ptr<NativeWebRuntime>& web,
                                                                     const std::shared_ptr<NativeBrowserRuntime>& browser,
                                                                     NativeProviderTransport provider_transport,
                                                                     NativeLlamaCppAdapters llama_adapters) {
  if (!value.is_object()) {
    return nullptr;
  }
  auto runtime = std::make_shared<NativeKnowledgeRuntime>();
  runtime->loader = knowledge_loader_for_web(web, browser);
  runtime->base = knowledge_base_from_value(root, value.at("base"), runtime->loader, provider_transport, llama_adapters);
  runtime->manager = knowledge_manager_from_value(root, value.at("manager"), runtime->loader, provider_transport,
                                                  std::move(llama_adapters));
  return (runtime->base || runtime->manager) ? runtime : nullptr;
}

std::vector<ToolDefinition> builtin_tools_for_bundles(const std::vector<std::string>& bundles,
                                                      const std::shared_ptr<NativeWorkflowRuntime>& workflow,
                                                      const std::shared_ptr<NativeWebRuntime>& web,
                                                      const std::shared_ptr<NativeBrowserRuntime>& browser,
                                                      const std::shared_ptr<NativeKnowledgeRuntime>& knowledge,
                                                      const NativeDeveloperAdapters& developer_adapters) {
  std::vector<ToolDefinition> tools;
  const auto append = [&](std::vector<ToolDefinition> incoming) {
    append_unique_tools(tools, std::move(incoming));
  };
  for (const auto& bundle : bundles) {
    if (bundle == "web" && web) {
      auto web_tools = create_web_builtin_tools(
          web->search_registry.get(),
          web->default_search_provider,
          web->fetcher.get());
      for (auto& tool : web_tools) {
        auto execute = std::move(tool.execute);
        tool.execute = [web, execute = std::move(execute)](const Value& input,
                                                           ToolExecutionContext& context) mutable -> ToolInvokeResult {
          return execute(input, context);
        };
      }
      append(std::move(web_tools));
      continue;
    }
    if (bundle == "browser" && browser && browser->renderer) {
      auto browser_tools = create_browser_builtin_tools(browser->renderer.get());
      for (auto& tool : browser_tools) {
        auto execute = std::move(tool.execute);
        tool.execute = [browser, execute = std::move(execute)](const Value& input,
                                                               ToolExecutionContext& context) mutable -> ToolInvokeResult {
          return execute(input, context);
        };
      }
      append(std::move(browser_tools));
      continue;
    }
    if (bundle == "workflow" && workflow && workflow->engine) {
      auto workflow_tools = create_workflow_builtin_tools(workflow->engine.get());
      for (auto& tool : workflow_tools) {
        auto execute = std::move(tool.execute);
        tool.execute = [workflow, execute = std::move(execute)](const Value& input,
                                                                ToolExecutionContext& context) mutable -> ToolInvokeResult {
          return execute(input, context);
        };
      }
      append(std::move(workflow_tools));
      continue;
    }
    if (bundle == "agent" && knowledge) {
      KnowledgeContextProvider* knowledge_provider =
          knowledge->manager ? static_cast<KnowledgeContextProvider*>(knowledge->manager.get())
                             : static_cast<KnowledgeContextProvider*>(knowledge->base.get());
      auto agent_tools = create_agent_builtin_tools(
          nullptr,
          knowledge_provider);
      for (auto& tool : agent_tools) {
        auto execute = std::move(tool.execute);
        tool.execute = [knowledge, execute = std::move(execute)](const Value& input,
                                                                 ToolExecutionContext& context) mutable -> ToolInvokeResult {
          return execute(input, context);
        };
      }
      append(std::move(agent_tools));
      continue;
    }
    if (bundle == "developer") {
      append(create_developer_builtin_tools(developer_adapters.process_executor));
      continue;
    }
    append(create_builtin_tools({bundle}));
  }
  return tools;
}

std::vector<ToolDefinition> builtin_tools_without_bundle(const std::vector<std::string>& bundles,
                                                         const std::string& excluded_bundle,
                                                         const std::shared_ptr<NativeWebRuntime>& web,
                                                         const std::shared_ptr<NativeBrowserRuntime>& browser,
                                                         const std::shared_ptr<NativeKnowledgeRuntime>& knowledge,
                                                         const NativeDeveloperAdapters& developer_adapters) {
  std::vector<std::string> filtered;
  for (const auto& bundle : bundles) {
    if (bundle != excluded_bundle) {
      filtered.push_back(bundle);
    }
  }
  return builtin_tools_for_bundles(filtered, nullptr, web, browser, knowledge, developer_adapters);
}

std::shared_ptr<NativeWorkflowRuntime> workflow_runtime_from_value(const Value& root,
                                                                   const Value& value,
                                                                   const std::vector<ToolDefinition>& tools,
                                                                   PermissionPolicy permission_policy,
                                                                   PermissionApprovalHandler approval_handler) {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto& resolved = resolve_config_resource_ref(root, value, "workflowEngines", "workflowEngine");
  auto runtime = std::make_shared<NativeWorkflowRuntime>();
  runtime->store = workflow_store_from_value(root, resolved.at("store"));
  runtime->tools = std::make_shared<ToolRegistry>(tools);
  runtime->tool_executor =
      std::make_shared<ToolExecutor>(*runtime->tools, std::move(permission_policy), std::move(approval_handler));
  runtime->engine =
      std::make_shared<WorkflowEngine>(runtime->tools.get(), runtime->tool_executor.get(), runtime->store.get());
  return runtime;
}

ToolExecutionServices tool_services_from_native_runtimes(
    const std::shared_ptr<NativeWorkflowRuntime>& workflow,
    const std::shared_ptr<NativeWebRuntime>& web,
    const std::shared_ptr<NativeBrowserRuntime>& browser,
    const std::shared_ptr<NativeKnowledgeRuntime>& knowledge) {
  ToolExecutionServices services;
  services.values = Value::object({});
  if (workflow && workflow->engine) {
    services.service_container.set(kToolServiceWorkflowEngine, workflow->engine.get());
    services.values["workflow"] = Value::object({{"engineAvailable", true}});
  }
  if (web) {
    services.service_container.set(kToolServiceWebSearchRegistry, web->search_registry.get());
    if (!web->default_search_provider.empty()) {
      services.service_container.set(kToolServiceDefaultSearchProvider, &web->default_search_provider);
    }
    services.service_container.set(kToolServiceWebFetcher, web->fetcher.get());
    services.service_container.set(kToolServiceWebCrawler, web->crawler.get());
    if (web->default_crawler_profile.is_object() &&
        !web->default_crawler_profile.as_object().empty()) {
      services.service_container.set(kToolServiceDefaultCrawlerProfile, &web->default_crawler_profile);
    }
    services.values["web"] = Value::object({
        {"searchRegistryAvailable", web->search_registry != nullptr},
        {"defaultSearchProvider", web->default_search_provider},
        {"fetcherAvailable", web->fetcher != nullptr},
        {"crawlerAvailable", web->crawler != nullptr},
        {"defaultCrawlerProfile", web->default_crawler_profile},
    });
  }
  if (browser && browser->renderer) {
    services.service_container.set(kToolServiceBrowserRenderer, browser->renderer.get());
    services.values["browser"] = Value::object({
        {"rendererAvailable", true},
        {"renderer", browser->renderer->metadata().name},
    });
  }
  if (knowledge) {
    KnowledgeContextProvider* knowledge_provider =
        knowledge->manager ? static_cast<KnowledgeContextProvider*>(knowledge->manager.get())
                           : static_cast<KnowledgeContextProvider*>(knowledge->base.get());
    services.service_container.set(kToolServiceKnowledgeProvider, knowledge_provider);
    Value::Object knowledge_values{
        {"baseAvailable", knowledge->base != nullptr},
        {"managerAvailable", knowledge->manager != nullptr},
    };
    if (knowledge->base) {
      knowledge_values["baseId"] = knowledge->base->id();
      knowledge_values["tenantId"] = knowledge->base->tenant_id();
    }
    services.values["knowledge"] = Value(std::move(knowledge_values));
  }
  return services;
}

NativeChatProviderBuildContext default_native_chat_provider_context(
    NativeProviderTransport transport,
    NativeProviderStreamTransport stream_transport,
    NativeProviderStreamingTransport streaming_transport,
    NativeLlamaCppAdapters llama_adapters) {
  const bool has_injected_transport = static_cast<bool>(transport);
  const bool has_injected_stream_transport = static_cast<bool>(stream_transport);
  if (!transport) {
    transport = create_native_provider_http_transport(create_native_http_transport());
  }
  if (!stream_transport && !has_injected_transport) {
    stream_transport = create_native_provider_http_stream_transport(create_native_http_transport());
  }
  if (!streaming_transport && !has_injected_transport && !has_injected_stream_transport) {
    streaming_transport = create_native_provider_http_streaming_transport(
        create_native_http_streaming_transport());
  }
  return NativeChatProviderBuildContext{
      .transport = std::move(transport),
      .stream_transport = std::move(stream_transport),
      .streaming_transport = std::move(streaming_transport),
      .llama_adapters = std::move(llama_adapters),
  };
}

std::shared_ptr<ChatModelAdapter> concrete_model_adapter_from_value(const Value& model,
                                                                    NativeProviderTransport transport,
                                                                    NativeProviderStreamTransport stream_transport,
                                                                    NativeLlamaCppAdapters llama_adapters,
                                                                    NativeProviderStreamingTransport streaming_transport) {
  if (!model.is_object()) {
    throw ConfigurationError("Agent model must be an object.");
  }
  const std::string provider = model.at("provider").as_string();
  if (provider.empty()) {
    throw ConfigurationError("Agent model requires provider.");
  }
  const auto descriptor = find_native_chat_provider_descriptor(provider);
  if (!descriptor) {
    throw ConfigurationError("Unsupported model provider \"" + provider + "\".");
  }
  auto adapter = descriptor->create_adapter(
      model,
      default_native_chat_provider_context(std::move(transport), std::move(stream_transport),
                                           std::move(streaming_transport), std::move(llama_adapters)));
  adapter->set_capabilities(model_capabilities_from_config(model));
  return adapter;
}

std::shared_ptr<ChatModelAdapter> model_adapter_from_value(const Value& model,
                                                           NativeProviderTransport transport,
                                                           NativeProviderStreamTransport stream_transport,
                                                           NativeLlamaCppAdapters llama_adapters,
                                                           NativeProviderStreamingTransport streaming_transport = {}) {
  const auto candidates = select_model_candidates(model);
  std::vector<std::shared_ptr<ChatModelAdapter>> adapters;
  adapters.reserve(candidates.size());
  for (const auto& candidate : candidates) {
    adapters.push_back(concrete_model_adapter_from_value(candidate, transport, stream_transport, llama_adapters,
                                                        streaming_transport));
  }
  return adapters.size() == 1 ? adapters.front()
                              : std::make_shared<FallbackChatModelAdapter>(std::move(adapters));
}

std::shared_ptr<Planner> planner_from_value(const Value& value,
                                            const Value& default_model_config,
                                            const std::shared_ptr<ChatModelAdapter>& default_model,
                                            NativeProviderTransport transport,
                                            NativeProviderStreamTransport stream_transport,
                                            NativeLlamaCppAdapters llama_adapters,
                                            NativeProviderStreamingTransport streaming_transport) {
  if (value.is_bool()) {
    if (!value.as_bool()) {
      return nullptr;
    }
    return std::make_shared<ModelPlanner>(ModelPlannerConfig{.model = default_model});
  }
  if (!value.is_object()) {
    return nullptr;
  }
  if (value.contains("enabled") && !value.at("enabled").as_bool()) {
    return nullptr;
  }

  const std::string kind = value.at("kind").as_string("model");
  if (kind == "static") {
    const auto& plan_value = value.at("plan").is_object() ? value.at("plan") : value;
    auto plan = normalize_execution_plan(plan_value);
    if (!plan) {
      throw ConfigurationError("Static planner requires a non-empty execution plan.");
    }
    return std::make_shared<StaticPlanner>(std::move(*plan));
  }

  if (kind == "model") {
    auto planner_model = value.at("model").is_object()
                             ? model_adapter_from_value(value.at("model"), std::move(transport),
                                                        std::move(stream_transport), std::move(llama_adapters),
                                                        std::move(streaming_transport))
                             : default_model;
    ModelPlannerConfig config;
    config.model = std::move(planner_model);
    if (value.contains("maxSteps")) {
      config.max_steps = static_cast<std::size_t>(std::max<long long>(1, value.at("maxSteps").as_integer(6)));
    }
    const auto system_prompt = value.at("systemPrompt").as_string();
    if (!system_prompt.empty()) {
      config.system_prompt = system_prompt;
    }
    config.model_settings = model_settings_from_value(value.at("modelSettings"));
    if (config.model_settings.model.empty() && config.model) {
      config.model_settings.model = config.model->model();
    }
    if (config.model_settings.model.empty()) {
      config.model_settings.model = default_model_config.at("model").as_string();
    }
    return std::make_shared<ModelPlanner>(std::move(config));
  }

  throw ConfigurationError("Unsupported planner kind \"" + kind + "\".");
}

ReActPromptMode react_prompt_mode_from_value(const Value& value, const std::string& path) {
  if (value.is_null()) {
    return ReActPromptMode::Managed;
  }
  const auto mode = value.as_string("managed");
  if (mode == "managed") {
    return ReActPromptMode::Managed;
  }
  if (mode == "custom") {
    return ReActPromptMode::Custom;
  }
  if (mode == "external") {
    return ReActPromptMode::External;
  }
  throw ConfigurationError(path + " must be one of: managed, custom, external.");
}

AgentToolCallingStrategy tool_calling_strategy_from_value(const Value& value, const std::string& path) {
  if (value.is_null()) {
    return AgentToolCallingStrategy::TextReAct;
  }
  const auto strategy = value.as_string("text-react");
  if (strategy == "text-react") {
    return AgentToolCallingStrategy::TextReAct;
  }
  if (strategy == "native-tool-calling") {
    return AgentToolCallingStrategy::NativeToolCalling;
  }
  throw ConfigurationError(path + " must be one of: text-react, native-tool-calling.");
}

ReActParserOptions react_parser_options_from_value(const Value& value, const std::string& path) {
  ReActParserOptions options;
  if (value.is_null()) {
    return options;
  }
  if (!value.is_object()) {
    throw ConfigurationError(path + " must be an object when provided.");
  }
  if (value.contains("maxActions")) {
    const auto& max_actions = value.at("maxActions");
    if (max_actions.is_null()) {
      return options;
    }
    if (!max_actions.is_number()) {
      throw ConfigurationError(path + ".maxActions must be a positive integer when provided.");
    }
    const auto count = max_actions.as_integer();
    if (count <= 0) {
      throw ConfigurationError(path + ".maxActions must be greater than zero.");
    }
    options.max_actions = static_cast<std::size_t>(count);
  }
  return options;
}

const Value* find_config_resource(const Value& config,
                                  const std::string& collection,
                                  const std::string& name) {
  const auto& resources = config.at("resources").at(collection);
  if (!resources.is_object() || !resources.contains(name)) {
    return nullptr;
  }
  return &resources.at(name);
}

void collect_model_env_refs(const Value& model, std::set<std::string>& keys) {
  if (!model.is_object()) {
    return;
  }
  const std::string provider = model.at("provider").as_string();
  if (const auto descriptor = find_native_chat_provider_descriptor(provider);
      descriptor && descriptor->collect_env_refs) {
    for (const auto& key : descriptor->collect_env_refs(model)) {
      if (!key.empty()) {
        keys.insert(key);
      }
    }
  }
  for (const auto& fallback : model.at("fallbacks").as_array()) {
    collect_model_env_refs(fallback, keys);
  }
}

void collect_embedding_env_refs(const Value& embedding, std::set<std::string>& keys) {
  if (!embedding.is_object()) {
    return;
  }
  const std::string provider = embedding.at("provider").as_string();
  if (const auto descriptor = find_native_text_embedding_provider_descriptor(provider);
      descriptor && descriptor->collect_env_refs) {
    for (const auto& key : descriptor->collect_env_refs(embedding)) {
      if (!key.empty()) {
        keys.insert(key);
      }
    }
  }
  if (const auto descriptor = find_native_image_embedding_provider_descriptor(provider);
      descriptor && descriptor->collect_env_refs) {
    for (const auto& key : descriptor->collect_env_refs(embedding)) {
      if (!key.empty()) {
        keys.insert(key);
      }
    }
  }
}

void collect_web_env_refs(const Value& web, std::set<std::string>& keys) {
  if (!web.is_object()) {
    return;
  }
  for (const auto& provider : web.at("search").as_array()) {
    if (!provider.is_object()) {
      continue;
    }
    const std::string kind = provider.at("kind").as_string("brave");
    if (kind == "searxng") {
      add_env_ref(keys, provider.at("baseUrlEnv"));
      continue;
    }
    add_env_ref(keys, provider.at("apiKeyEnv"));
    add_env_ref(keys, provider.at("baseUrlEnv"));
  }
}

void collect_knowledge_base_env_refs(const Value& value, std::set<std::string>& keys);

void collect_knowledge_manager_env_refs(const Value& value, std::set<std::string>& keys) {
  if (!value.is_object()) {
    return;
  }
  collect_embedding_env_refs(value.at("embedder"), keys);
  collect_embedding_env_refs(value.at("imageEmbedder"), keys);
  for (const auto& base : value.at("bases").as_array()) {
    collect_knowledge_base_env_refs(base, keys);
  }
}

void collect_knowledge_base_env_refs(const Value& value, std::set<std::string>& keys) {
  if (!value.is_object()) {
    return;
  }
  collect_embedding_env_refs(value.at("embedder"), keys);
  collect_embedding_env_refs(value.at("imageEmbedder"), keys);
}

void collect_agent_env_refs(const Value& config, const Value& definition, std::set<std::string>& keys) {
  if (!definition.is_object()) {
    return;
  }
  collect_model_env_refs(definition.at("model"), keys);
  const auto& runtime_planner = definition.at("runtime").at("planner");
  if (runtime_planner.is_object()) {
    collect_model_env_refs(runtime_planner.at("model"), keys);
  }
  collect_web_env_refs(definition.at("web"), keys);

  const auto& knowledge = definition.at("knowledge");
  const auto& base = knowledge.at("base");
  if (is_named_resource_ref(base)) {
    if (const auto* resource = find_config_resource(config, "knowledgeBases", base.at("use").as_string())) {
      collect_knowledge_base_env_refs(*resource, keys);
    }
  } else {
    collect_knowledge_base_env_refs(base, keys);
  }

  const auto& manager = knowledge.at("manager");
  if (is_named_resource_ref(manager)) {
    if (const auto* resource = find_config_resource(config, "knowledgeManagers", manager.at("use").as_string())) {
      collect_knowledge_manager_env_refs(*resource, keys);
    }
  } else {
    collect_knowledge_manager_env_refs(manager, keys);
  }
}

bool env_has_value(const std::string& key, const std::optional<std::map<std::string, std::string>>& env) {
  if (key.empty()) {
    return true;
  }
  if (env) {
    const auto found = env->find(key);
    return found != env->end() && !found->second.empty();
  }
  return !env_value(key).empty();
}

}  // namespace

void validate_native_agent_config(const Value& config) {
  const auto& agents = config.at("agents");
  if (!agents.is_object() || agents.as_object().empty()) {
    throw ConfigurationError("Agent config requires at least one agent.");
  }

  const std::string default_agent = config.at("defaultAgent").as_string();
  if (!default_agent.empty() && !agents.contains(default_agent)) {
    throw ConfigurationError("defaultAgent \"" + default_agent + "\" does not exist in agents.");
  }

  for (const auto& [agent_id, definition] : agents.as_object()) {
    const std::string agent_path = "agents." + agent_id;
    if (!definition.is_object()) {
      throw ConfigurationError(agent_path + " must be an object.");
    }
    validate_model_config(definition.at("model"), agent_path + ".model");
    if (definition.at("tools").at("bundles").is_array()) {
      for (const auto& bundle : definition.at("tools").at("bundles").as_array()) {
        const std::string name = bundle.as_string();
        if (name.empty() ||
            !is_one_of(name, {"core", "local", "http", "developer", "agent",
                              "web", "browser", "workflow", "state"})) {
          throw ConfigurationError(agent_path + ".tools.bundles contains unsupported bundle \"" + name + "\".");
        }
      }
    }
    validate_agent_resource_refs(config, definition, agent_path);
    validate_knowledge_retrieval_options(definition.at("knowledge").at("retrievalOptions"),
                                         agent_path + ".knowledge.retrievalOptions");
    validate_knowledge_retrieval_options(definition.at("runtime").at("knowledgeRetrievalOptions"),
                                         agent_path + ".runtime.knowledgeRetrievalOptions");
    (void)tool_calling_strategy_from_value(definition.at("runtime").at("toolCallingStrategy"),
                                           agent_path + ".runtime.toolCallingStrategy");
    (void)react_prompt_mode_from_value(definition.at("runtime").at("reactPromptMode"),
                                       agent_path + ".runtime.reactPromptMode");
    (void)react_parser_options_from_value(definition.at("runtime").at("reactParserOptions"),
                                          agent_path + ".runtime.reactParserOptions");
    (void)context_stats_bucket_inputs_from_value(definition.at("runtime").at("promptStatsBuckets"),
                                                 agent_path + ".runtime.promptStatsBuckets");
    validate_web_config(definition.at("web"), agent_path + ".web");
    validate_planner_config(definition.at("runtime").at("planner"), agent_path + ".runtime.planner");
  }
}

void validate_native_agent_config(const NativeLoadedAgentConfig& loaded_config) {
  auto normalized = define_native_loaded_agent_config(loaded_config.config,
                                                      loaded_config.cwd,
                                                      loaded_config.path);
  validate_native_agent_config(normalized.config);
}

std::vector<std::string> collect_referenced_env_keys(const Value& config) {
  std::set<std::string> keys;
  const auto& agents = config.at("agents");
  if (agents.is_object()) {
    for (const auto& [_, definition] : agents.as_object()) {
      collect_agent_env_refs(config, definition, keys);
    }
  }
  return std::vector<std::string>(keys.begin(), keys.end());
}

std::vector<std::string> collect_referenced_env_keys(const NativeLoadedAgentConfig& loaded_config) {
  auto normalized = define_native_loaded_agent_config(loaded_config.config,
                                                      loaded_config.cwd,
                                                      loaded_config.path);
  return collect_referenced_env_keys(normalized.config);
}

std::vector<std::string> assert_referenced_env_keys(
    const Value& config,
    std::optional<std::map<std::string, std::string>> env) {
  const auto keys = collect_referenced_env_keys(config);
  std::vector<std::string> missing;
  for (const auto& key : keys) {
    if (!env_has_value(key, env)) {
      missing.push_back(key);
    }
  }
  if (!missing.empty()) {
    std::string message = "Missing required environment variables: ";
    for (std::size_t index = 0; index < missing.size(); ++index) {
      if (index > 0) {
        message += ", ";
      }
      message += missing[index];
    }
    throw ConfigurationError(std::move(message));
  }
  return keys;
}

std::vector<std::string> assert_referenced_env_keys(
    const NativeLoadedAgentConfig& loaded_config,
    std::optional<std::map<std::string, std::string>> env) {
  auto normalized = define_native_loaded_agent_config(loaded_config.config,
                                                      loaded_config.cwd,
                                                      loaded_config.path);
  return assert_referenced_env_keys(normalized.config, std::move(env));
}

Value define_native_agent_config(Value config) {
  if (!config.is_object()) {
    throw ConfigurationError("Native agent config must be an object.");
  }
  return config;
}

NativeLoadedAgentConfig define_native_loaded_agent_config(Value config,
                                                          std::filesystem::path cwd,
                                                          std::filesystem::path path) {
  if (!config.is_object()) {
    throw ConfigurationError("Native loaded agent config must contain an object config.");
  }
  if (path.empty() && config.at("_path").is_string()) {
    path = config.at("_path").as_string();
  }
  if (cwd.empty() && config.at("_cwd").is_string()) {
    cwd = config.at("_cwd").as_string();
  }
  if (cwd.empty()) {
    cwd = path.empty() || path.parent_path().empty() ? current_config_cwd() : path.parent_path();
  }
  attach_config_location(config, cwd, path);
  return NativeLoadedAgentConfig{std::move(config), std::move(cwd), std::move(path)};
}

std::optional<std::filesystem::path> find_native_agent_config_file(std::filesystem::path cwd) {
  std::error_code error;
  if (cwd.empty()) {
    cwd = std::filesystem::current_path(error);
    if (error) {
      cwd = ".";
      error.clear();
    }
  }

  std::filesystem::path current = std::filesystem::absolute(cwd, error);
  if (error) {
    current = cwd;
    error.clear();
  }
  if (std::filesystem::is_regular_file(current, error)) {
    current = current.parent_path();
    error.clear();
  }

  while (!current.empty()) {
    for (const auto* file_name : kNativeAgentConfigFiles) {
      const auto candidate = current / file_name;
      if (std::filesystem::is_regular_file(candidate, error)) {
        return candidate;
      }
      error.clear();
    }
    const auto parent = current.parent_path();
    if (parent == current || parent.empty()) {
      break;
    }
    current = parent;
  }
  return std::nullopt;
}

std::string resolve_agent_id(const Value& config, std::string requested_agent_id) {
  const auto& agents = config.at("agents");
  if (!agents.is_object() || agents.as_object().empty()) {
    throw ConfigurationError("Agent config requires at least one agent.");
  }
  if (!requested_agent_id.empty()) {
    if (!agents.contains(requested_agent_id)) {
      throw ConfigurationError("Unknown agent \"" + requested_agent_id + "\".");
    }
    return requested_agent_id;
  }
  const std::string default_agent = config.at("defaultAgent").as_string();
  if (!default_agent.empty()) {
    if (!agents.contains(default_agent)) {
      throw ConfigurationError("defaultAgent \"" + default_agent + "\" is not configured.");
    }
    return default_agent;
  }
  if (agents.as_object().size() == 1) {
    return agents.as_object().begin()->first;
  }
  throw ConfigurationError("Multiple agents are configured. Pass an explicit agent id or define defaultAgent.");
}

std::string resolve_agent_id(const NativeLoadedAgentConfig& loaded_config,
                             std::string requested_agent_id) {
  auto normalized = define_native_loaded_agent_config(loaded_config.config,
                                                      loaded_config.cwd,
                                                      loaded_config.path);
  return resolve_agent_id(normalized.config, std::move(requested_agent_id));
}

Value resolve_agent_definition(const Value& config, std::string requested_agent_id) {
  const auto agent_id = resolve_agent_id(config, std::move(requested_agent_id));
  const auto& definition = config.at("agents").at(agent_id);
  if (!definition.is_object()) {
    throw ConfigurationError("Agent definition \"" + agent_id + "\" must be an object.");
  }
  return definition;
}

Value resolve_agent_definition(const NativeLoadedAgentConfig& loaded_config,
                               std::string requested_agent_id) {
  auto normalized = define_native_loaded_agent_config(loaded_config.config,
                                                      loaded_config.cwd,
                                                      loaded_config.path);
  return resolve_agent_definition(normalized.config, std::move(requested_agent_id));
}

NativeResolvedAgentApp resolve_native_agent_app(const Value& config, std::string requested_agent_id) {
  NativeAgentAppResolveOptions options;
  options.requested_agent_id = std::move(requested_agent_id);
  return resolve_native_agent_app(config, std::move(options));
}

NativeResolvedAgentApp resolve_native_agent_app(const Value& config, NativeAgentAppResolveOptions options) {
  validate_native_agent_config(config);
  const std::string agent_id = resolve_agent_id(config, std::move(options.requested_agent_id));
  const auto& definition = config.at("agents").at(agent_id);
  if (!definition.is_object()) {
    throw ConfigurationError("Agent definition \"" + agent_id + "\" must be an object.");
  }

  auto provider_transport = std::move(options.provider_transport);
  auto provider_stream_transport = std::move(options.provider_stream_transport);
  auto provider_streaming_transport = std::move(options.provider_streaming_transport);
  auto mcp_transport_factory = std::move(options.mcp_transport_factory);
  auto web_adapters = std::move(options.web_adapters);
  auto developer_adapters = std::move(options.developer_adapters);
  auto browser_adapters = std::move(options.browser_adapters);
  auto llama_adapters = std::move(options.llama_adapters);
  auto configure_session_memory = std::move(options.configure_session_memory);

  const auto& tools_config = definition.at("tools");
  std::vector<std::string> bundles = string_array_from_value(tools_config.at("bundles"));
  PermissionPolicy permission_policy = permission_policy_from_value(config, definition.at("permissions").at("policy"));
  PermissionApprovalHandler approval_handler =
      approval_handler_from_value(config, definition.at("permissions").at("approval"));
  auto browser = browser_runtime_from_value(definition.at("browser"), bundles, std::move(browser_adapters));
  auto web = web_runtime_from_value(definition.at("web"), bundles, std::move(web_adapters), browser);
  auto knowledge = knowledge_runtime_from_value(config, definition.at("knowledge"), web, browser,
                                                provider_transport, llama_adapters);
  auto workflow = workflow_runtime_from_value(
      config,
      definition.at("workflow").at("engine"),
      builtin_tools_without_bundle(bundles, "workflow", web, browser, knowledge, developer_adapters),
      permission_policy,
      approval_handler);
  std::vector<ToolDefinition> tools =
      bundles.empty() ? std::vector<ToolDefinition>{}
                      : builtin_tools_for_bundles(bundles, workflow, web, browser, knowledge, developer_adapters);
  std::vector<std::shared_ptr<MCPClient>> mcp_clients;
  append_unique_tools(tools, mcp_tools_from_value(config, definition.at("mcp"), mcp_transport_factory, mcp_clients));

  const auto& runtime = definition.at("runtime");
  AgentRunnerConfig runner_config;
  runner_config.model_runtime.adapter =
      model_adapter_from_value(definition.at("model"), provider_transport, provider_stream_transport,
                               llama_adapters, provider_streaming_transport);
  runner_config.tool_runtime.definitions = tools;
  runner_config.context_runtime.system_prompt = definition.at("systemPrompt").as_string();
  runner_config.context_runtime.max_iterations =
      static_cast<int>(definition.at("maxIterations").as_integer(runtime.at("maxIterations").as_integer(8)));
  runner_config.model_runtime.settings = model_settings_from_value(runtime.at("modelSettings"));
  if (runner_config.model_runtime.settings.model.empty() && runner_config.model_runtime.adapter) {
    runner_config.model_runtime.settings.model = runner_config.model_runtime.adapter->model();
  }
  runner_config.memory_runtime.session_store = session_store_from_value(
      config, definition.at("memory").at("session"), configure_session_memory);
  runner_config.memory_runtime.long_term_memory = long_term_memory_from_value(config, runtime.at("longTermMemory"));
  runner_config.knowledge_runtime.provider =
      knowledge && knowledge->manager ? static_cast<KnowledgeContextProvider*>(knowledge->manager.get())
      : knowledge && knowledge->base ? static_cast<KnowledgeContextProvider*>(knowledge->base.get())
                                     : nullptr;
  runner_config.knowledge_runtime.retrieval_options =
      knowledge_retrieval_options_from_value(definition.at("knowledge").at("retrievalOptions"));
  if (!runtime.at("knowledgeRetrievalOptions").is_null()) {
    runner_config.knowledge_runtime.retrieval_options =
        knowledge_retrieval_options_from_value(runtime.at("knowledgeRetrievalOptions"));
  }
  runner_config.memory_runtime.retrieval_options = retrieval_options_from_value(runtime.at("retrievalOptions"));
  runner_config.memory_runtime.writeback_options = writeback_options_from_value(runtime.at("writebackOptions"));
  runner_config.governance.planner = planner_from_value(runtime.at("planner"), definition.at("model"),
                                                        runner_config.model_runtime.adapter, std::move(provider_transport),
                                                        std::move(provider_stream_transport), std::move(llama_adapters),
                                                        std::move(provider_streaming_transport));
  if (runtime.contains("enablePlanning")) {
    runner_config.governance.enable_planning = runtime.at("enablePlanning").as_bool();
  }
  runner_config.tool_runtime.permission_policy = std::move(permission_policy);
  runner_config.tool_runtime.approval_handler = std::move(approval_handler);
  runner_config.tool_runtime.services = tool_services_from_native_runtimes(workflow, web, browser, knowledge);
  runner_config.context_runtime.skills = skills_registry_from_value(config, definition.at("skills"));
  runner_config.context_runtime.default_skills = string_array_from_value(definition.at("skills").at("defaultSkills"));
  if (definition.at("skills").contains("advertiseSkills")) {
    runner_config.context_runtime.advertise_skills = definition.at("skills").at("advertiseSkills").as_bool();
  }
  runner_config.react_runtime.max_parse_errors =
      static_cast<int>(definition.at("maxParseErrors").as_integer(runtime.at("maxParseErrors").as_integer(2)));
  runner_config.tool_runtime.calling_strategy =
      tool_calling_strategy_from_value(runtime.at("toolCallingStrategy"), "runtime.toolCallingStrategy");
  runner_config.react_runtime.prompt_mode =
      react_prompt_mode_from_value(runtime.at("reactPromptMode"), "runtime.reactPromptMode");
  runner_config.react_runtime.parser_options =
      react_parser_options_from_value(runtime.at("reactParserOptions"), "runtime.reactParserOptions");
  runner_config.observability.prompt_stats_buckets =
      context_stats_bucket_inputs_from_value(runtime.at("promptStatsBuckets"), "runtime.promptStatsBuckets");

  NativeResolvedAgentApp app;
  app.agent_id = agent_id;
  app.tools = tools;
  app.skills = runner_config.context_runtime.skills;
  app.mcp_clients = std::move(mcp_clients);
  app.workflow = std::move(workflow);
  app.web = std::move(web);
  app.browser = std::move(browser);
  app.knowledge = std::move(knowledge);
  app.runner = std::make_shared<AgentRunner>(std::move(runner_config));
  return app;
}

NativeResolvedAgentApp resolve_native_agent_app(NativeLoadedAgentConfig loaded_config,
                                                std::string requested_agent_id) {
  NativeAgentAppResolveOptions options;
  options.requested_agent_id = std::move(requested_agent_id);
  return resolve_native_agent_app(std::move(loaded_config), std::move(options));
}

NativeResolvedAgentApp resolve_native_agent_app(NativeLoadedAgentConfig loaded_config,
                                                NativeAgentAppResolveOptions options) {
  loaded_config = define_native_loaded_agent_config(std::move(loaded_config.config),
                                                    std::move(loaded_config.cwd),
                                                    std::move(loaded_config.path));
  return resolve_native_agent_app(loaded_config.config, std::move(options));
}

NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             NativeConfigModuleLoader config_module_loader) {
  return load_native_agent_app(config_path, NativeAgentAppResolveOptions{}, std::move(config_module_loader));
}

NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             std::string requested_agent_id,
                                             NativeConfigModuleLoader config_module_loader) {
  NativeAgentAppResolveOptions options;
  options.requested_agent_id = std::move(requested_agent_id);
  return load_native_agent_app(config_path, std::move(options), std::move(config_module_loader));
}

NativeResolvedAgentApp load_native_agent_app(const std::filesystem::path& config_path,
                                             NativeAgentAppResolveOptions options,
                                             NativeConfigModuleLoader config_module_loader) {
  auto loaded_config = load_native_loaded_agent_config(config_path, std::move(config_module_loader));
  return resolve_native_agent_app(std::move(loaded_config), std::move(options));
}

NativeLoadedAgentConfig load_native_loaded_agent_config(const std::filesystem::path& config_path,
                                                        NativeConfigModuleLoader config_module_loader) {
  std::filesystem::path resolved_path = config_path;
  std::error_code error;
  if (resolved_path.empty() || std::filesystem::is_directory(resolved_path, error)) {
    auto found = find_native_agent_config_file(resolved_path);
    if (!found) {
      throw ConfigurationError("Could not find node-agent config file.");
    }
    resolved_path = *found;
  }
  error.clear();
  if (!is_native_json_config_file(resolved_path)) {
    if (!is_native_module_config_file(resolved_path)) {
      throw ConfigurationError("Unsupported native agent config file extension: " + resolved_path.string());
    }
    if (!config_module_loader) {
      throw ConfigurationError(
          "Native C++ agent config loader requires a NativeConfigModuleLoader for JS/TS config modules: " +
          resolved_path.string());
    }
    Value config;
    try {
      config = config_module_loader(resolved_path);
    } catch (const ConfigurationError&) {
      throw;
    } catch (const std::exception& exception) {
      throw ConfigurationError("Failed to load agent config \"" + resolved_path.string() + "\". " +
                               exception.what());
    }
    if (!config.is_object()) {
      throw ConfigurationError("Agent config \"" + resolved_path.string() + "\" must export an object.");
    }
    attach_config_cwd(config, resolved_path);
    const auto cwd = std::filesystem::path(config.at("_cwd").as_string());
    return NativeLoadedAgentConfig{std::move(config), cwd, resolved_path};
  }

  Value config = read_json_file(resolved_path);
  if (!config.is_object()) {
    throw ConfigurationError("Agent config file must contain an object.");
  }
  attach_config_cwd(config, resolved_path);
  const auto cwd = std::filesystem::path(config.at("_cwd").as_string());
  return NativeLoadedAgentConfig{std::move(config), cwd, resolved_path};
}

Value load_native_agent_config(const std::filesystem::path& config_path,
                               NativeConfigModuleLoader config_module_loader) {
  return load_native_loaded_agent_config(config_path, std::move(config_module_loader)).config;
}
}  // namespace agent
