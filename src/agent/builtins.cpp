#include "agent/agent.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <tuple>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace agent {

namespace {

std::string uppercase_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

std::map<std::string, std::string> string_map_from_value(const Value& value) {
  std::map<std::string, std::string> output;
  for (const auto& [key, child] : value.as_object()) {
    output[key] = child.is_string() ? child.as_string() : child.stringify(0);
  }
  return output;
}

void append_unique_string(std::vector<std::string>& values, const std::string& value) {
  if (value.empty()) {
    return;
  }
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

std::vector<std::string> merge_unique_strings(const std::vector<std::string>& first,
                                              const std::vector<std::string>& second) {
  std::vector<std::string> merged;
  merged.reserve(first.size() + second.size());
  for (const auto& value : first) {
    append_unique_string(merged, value);
  }
  for (const auto& value : second) {
    append_unique_string(merged, value);
  }
  return merged;
}

ToolDefinition apply_bundle_metadata(ToolDefinition tool, const ToolBundleMetadata& metadata) {
  if (tool.bundle.empty()) {
    tool.bundle = metadata.name;
  }
  std::vector<std::string> metadata_tags = metadata.tags;
  append_unique_string(metadata_tags, "bundle:" + metadata.name);
  tool.tags = merge_unique_strings(metadata_tags, tool.tags);
  if (!metadata.capabilities.empty()) {
    tool.capabilities = merge_unique_strings(metadata.capabilities, tool.capabilities);
  }
  return tool;
}

Value headers_to_value(const std::map<std::string, std::string>& headers) {
  Value output = Value::object({});
  for (const auto& [key, value] : headers) {
    output[key] = value;
  }
  return output;
}

Value web_results_to_value(const std::vector<WebSearchResult>& results) {
  Value::Array output;
  for (const auto& result : results) {
    output.push_back(web_search_result_to_value(result));
  }
  return Value(output);
}

Value knowledge_hits_to_value(const std::vector<KnowledgeSearchHit>& hits) {
  Value::Array output;
  for (const auto& hit : hits) {
    output.push_back(knowledge_search_hit_to_value(hit));
  }
  return Value(output);
}

Value workflow_checkpoints_to_value(const std::vector<WorkflowCheckpoint>& checkpoints) {
  Value::Array output;
  for (const auto& checkpoint : checkpoints) {
    output.push_back(workflow_checkpoint_to_value(checkpoint));
  }
  return Value(output);
}

std::vector<std::string> string_array_from_value(const Value& value) {
  std::vector<std::string> output;
  for (const auto& item : value.as_array()) {
    output.push_back(item.as_string());
  }
  return output;
}

std::string request_body_from_value(const Value& body) {
  if (body.is_null()) {
    return {};
  }
  if (body.is_string()) {
    return body.as_string();
  }
  return body.stringify(0);
}

HttpTransport require_http_transport(const HttpTransport& transport) {
  if (!transport) {
    return create_native_http_transport();
  }
  return transport;
}

StaticWebSearchProvider resolve_web_search_provider(WebSearchProviderRegistry* registry,
                                                    const std::string& default_provider,
                                                    const std::string& preferred_provider) {
  if (!registry) {
    throw ConfigurationError("Web search provider registry is not configured.");
  }
  const std::string provider_name = preferred_provider.empty() ? default_provider : preferred_provider;
  if (provider_name.empty()) {
    throw ConfigurationError("Web search provider is not configured.");
  }
  const auto provider = registry->find(provider_name);
  if (!provider) {
    throw ConfigurationError("Unknown web search provider: " + provider_name);
  }
  return *provider;
}

WebFetchedPage fetch_web_page_with_default(NativeWebPageFetcher* fetcher, const WebFetchRequest& request) {
  if (fetcher) {
    return fetcher->fetch(request);
  }
  NativeWebPageFetcher default_fetcher(create_native_web_fetch_transport());
  return default_fetcher.fetch(request);
}

// ---------------------------------------------------------------------------
// Scratchpad / todo policy constants. The actual storage lives behind the
// injected `ScratchStore` interface (see `agent/scratch.hpp`). Todo lists are
// kept under a reserved key prefix so they do not collide with user scratch
// keys; the internal-key filter is applied at the tool layer, not the backend.
// ---------------------------------------------------------------------------

constexpr const char* kTodoInternalPrefix = "__todo:";
constexpr const char* kTodoListKey = "__todo:list";

ScratchStore& require_scratch_store(ToolExecutionContext& context) {
  if (!context.service_refs.scratch_store) {
    throw ConfigurationError(
        "Scratch / todo tools require an injected ScratchStore. "
        "Set AgentRunnerConfig::scratch_store or ToolExecutionServices::scratch_store "
        "before invoking these tools.");
  }
  return *context.service_refs.scratch_store;
}

std::string resolve_session_id(ToolExecutionContext& context) {
  return context.service_refs.session ? context.service_refs.session->session_id() : std::string("_");
}

bool is_valid_todo_status(const std::string& status) {
  return status == "pending" || status == "in_progress" || status == "completed" || status == "cancelled";
}

Value::Array load_todo_list(ScratchStore& store, const std::string& session) {
  auto stored = store.get(session, kTodoListKey);
  if (!stored || !stored->is_array()) {
    return {};
  }
  return stored->as_array();
}

void save_todo_list(ScratchStore& store, const std::string& session, Value::Array list) {
  store.set(session, kTodoListKey, Value(std::move(list)));
}

Value::Array filter_todo_list(const Value::Array& list, const std::string& status_filter) {
  if (status_filter.empty()) {
    return list;
  }
  Value::Array filtered;
  for (const auto& item : list) {
    if (item.is_object() && item.contains("status") && item.at("status").as_string() == status_filter) {
      filtered.push_back(item);
    }
  }
  return filtered;
}

}  // namespace

ToolBundleRegistry::ToolBundleRegistry(std::vector<ToolBundleProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

ToolBundleRegistry::ToolBundleRegistry(const ToolBundleRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

ToolBundleRegistry& ToolBundleRegistry::operator=(const ToolBundleRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

ToolBundleRegistry::ToolBundleRegistry(ToolBundleRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

ToolBundleRegistry& ToolBundleRegistry::operator=(ToolBundleRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

ToolBundleProvider& ToolBundleRegistry::register_provider(ToolBundleProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Tool bundle provider requires metadata.name.");
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (providers_.find(name) == providers_.end()) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

const ToolBundleProvider* ToolBundleRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::optional<ToolBundleProvider> ToolBundleRegistry::find(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? std::nullopt : std::optional<ToolBundleProvider>(found->second);
}

std::vector<ToolBundleProvider> ToolBundleRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<ToolBundleProvider> providers;
  providers.reserve(providers_.size());
  for (const auto& name : provider_order_) {
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
  }
  return providers;
}

std::vector<ToolDefinition> ToolBundleRegistry::create_tools(std::vector<std::string> bundles) const {
  std::vector<ToolBundleProvider> selected_providers;
  if (bundles.empty()) {
    selected_providers = list();
  } else {
    selected_providers.reserve(bundles.size());
    for (const auto& bundle_name : bundles) {
      auto provider = find(bundle_name);
      if (provider) {
        selected_providers.push_back(std::move(*provider));
      }
    }
  }
  std::vector<ToolDefinition> tools;
  std::set<std::string> seen;
  for (const auto& provider : selected_providers) {
    if (!provider.create_tools) {
      continue;
    }
    for (auto& tool : provider.create_tools()) {
      if (!seen.insert(tool.name).second) {
        continue;
      }
      tools.push_back(apply_bundle_metadata(std::move(tool), provider.metadata));
    }
  }
  return tools;
}

std::vector<ToolDefinition> create_core_builtin_tools() {
  JsonSchema json_schema;
  json_schema.type = JsonSchemaType::Object;
  json_schema.required = {"value"};
  json_schema.properties["value"].type = JsonSchemaType::Object;
  json_schema.properties["path"].type = JsonSchemaType::String;

  return {
      define_tool(ToolDefinition{
          .name = "time.now",
          .description = "Return the current ISO timestamp.",
          .capabilities = {"time.read"},
          .risk_level = ToolRiskLevel::Low,
          .builtin = true,
          .execute = [](const Value&, ToolExecutionContext&) -> ToolInvokeResult {
            return Value::object({{"now", now_iso8601()}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "uuid.generate",
          .description = "Generate a UUID.",
          .capabilities = {"random.read"},
          .risk_level = ToolRiskLevel::Low,
          .builtin = true,
          .execute = [](const Value&, ToolExecutionContext&) -> ToolInvokeResult {
            return Value::object({{"id", generate_uuid()}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "json.query",
          .description = "Select a path from a JSON-like object.",
          .input_schema = json_schema,
          .capabilities = {"json.read"},
          .risk_level = ToolRiskLevel::Low,
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            return Value::object({{"value", select_json_path(input.at("value"), input.at("path").as_string())}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "math.eval",
          .description = "Evaluate a safe arithmetic expression (no I/O, no identifiers beyond pi/e/tau and "
                         "registered math functions).",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.required = {"expression"};
            schema.properties["expression"].type = JsonSchemaType::String;
            return schema;
          }(),
          .capabilities = {"math.eval"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"core", "math"},
          .bundle = "core",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            const std::string expression = input.at("expression").as_string();
            const double value = evaluate_math_expression(expression);
            return Value::object({{"value", value}, {"expression", expression}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "text.diff",
          .description = "Produce a unified diff (3 lines context) between two text blobs.",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.required = {"before", "after"};
            schema.properties["before"].type = JsonSchemaType::String;
            schema.properties["after"].type = JsonSchemaType::String;
            schema.properties["labelBefore"].type = JsonSchemaType::String;
            schema.properties["labelAfter"].type = JsonSchemaType::String;
            return schema;
          }(),
          .capabilities = {"text.transform"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"core", "text", "diff"},
          .bundle = "core",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            const std::string before = input.at("before").as_string();
            const std::string after = input.at("after").as_string();
            const std::string label_before = input.at("labelBefore").as_string("a");
            const std::string label_after = input.at("labelAfter").as_string("b");
            const std::string diff = compute_unified_diff(before, after, label_before, label_after);
            return Value::object({{"diff", diff}, {"hasChanges", !diff.empty()}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "tool.search",
          .description = "Search the entire tool registry (including tools hidden by lazy_mode) by a "
                         "case-insensitive substring against the tool name and description.",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.required = {"query"};
            schema.properties["query"].type = JsonSchemaType::String;
            schema.properties["limit"].type = JsonSchemaType::Integer;
            return schema;
          }(),
          .capabilities = {"tool.discover"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"core", "tool", "discover"},
          .bundle = "core",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            ToolRegistry* registry = context.service_refs.registry;
            if (!registry) {
              return Value::object({{"tools", Value::array({})}});
            }
            std::string query = input.at("query").as_string();
            std::transform(query.begin(), query.end(), query.begin(),
                           [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
            const long long limit = input.contains("limit") && input.at("limit").is_number()
                                        ? input.at("limit").as_integer(0)
                                        : 0;
            Value::Array out;
            for (const auto& tool : registry->list_all()) {
              std::string lname = tool.name;
              std::transform(lname.begin(), lname.end(), lname.begin(),
                             [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
              std::string ldesc = tool.description;
              std::transform(ldesc.begin(), ldesc.end(), ldesc.begin(),
                             [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
              if (lname.find(query) == std::string::npos &&
                  ldesc.find(query) == std::string::npos) {
                continue;
              }
              Value::Array caps;
              for (const auto& cap : tool.capabilities) caps.emplace_back(cap);
              out.emplace_back(Value::object({
                  {"name", tool.name},
                  {"description", tool.description},
                  {"capabilities", Value(caps)},
                  {"bundle", tool.bundle},
              }));
              if (limit > 0 && static_cast<long long>(out.size()) >= limit) break;
            }
            return Value::object({{"tools", Value(std::move(out))}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "tool.describe",
          .description = "Return the full definition of a tool by name, including the input schema, "
                         "capabilities, bundle, and tags. Honors hidden tools when lazy_mode is on.",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.required = {"name"};
            schema.properties["name"].type = JsonSchemaType::String;
            return schema;
          }(),
          .capabilities = {"tool.discover"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"core", "tool", "discover"},
          .bundle = "core",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            ToolRegistry* registry = context.service_refs.registry;
            if (!registry) {
              return Value::object({{"found", false}});
            }
            const std::string name = input.at("name").as_string();
            auto found = registry->find(name);
            if (!found) {
              return Value::object({{"found", false}});
            }
            const auto& tool = *found;
            Value::Array caps;
            for (const auto& cap : tool.capabilities) caps.emplace_back(cap);
            Value::Array tags;
            for (const auto& tag : tool.tags) tags.emplace_back(tag);
            return Value::object({
                {"name", tool.name},
                {"description", tool.description},
                {"inputSchema", json_schema_to_value(tool.input_schema)},
                {"capabilities", Value(caps)},
                {"bundle", tool.bundle},
                {"tags", Value(tags)},
            });
          },
      }),
      define_tool(ToolDefinition{
          .name = "text.patch",
          .description = "Apply a unified diff patch to a source text. Returns the patched text, any conflict "
                         "messages, and whether all hunks applied cleanly.",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.required = {"source", "patch"};
            schema.properties["source"].type = JsonSchemaType::String;
            schema.properties["patch"].type = JsonSchemaType::String;
            return schema;
          }(),
          .capabilities = {"text.transform"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"core", "text", "patch"},
          .bundle = "core",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            const auto result = apply_unified_patch(input.at("source").as_string(),
                                                    input.at("patch").as_string());
            Value::Array conflicts;
            conflicts.reserve(result.conflicts.size());
            for (const auto& message : result.conflicts) {
              conflicts.emplace_back(message);
            }
            return Value::object({
                {"text", result.text},
                {"conflicts", Value(conflicts)},
                {"appliedCleanly", result.applied_cleanly},
            });
          },
      }),
  };
}

// Resolve `target` and ensure it stays within one of `allowed_roots`. The path
// is normalized (collapsing `..`) before the prefix check, defeating traversal.
// Symlinks are not followed (the target may not exist yet, e.g. fs.writeText) —
// for symlink-tight confinement the host should additionally restrict via OS
// sandboxing or the before_fs_write hook. An empty `allowed_roots` means
// confinement was explicitly disabled by the caller (the only path that reaches
// here empty), so the original path passes through untouched.
static std::string confine_fs_path(const std::string& target,
                                   const std::vector<std::string>& allowed_roots) {
  if (allowed_roots.empty()) {
    return target;
  }
  std::error_code ec;
  std::filesystem::path resolved = std::filesystem::weakly_canonical(target, ec);
  if (ec || resolved.empty()) {
    resolved = std::filesystem::absolute(target, ec);
  }
  const std::string resolved_str = resolved.lexically_normal().string();
  for (const auto& root : allowed_roots) {
    std::filesystem::path base_path = std::filesystem::weakly_canonical(root, ec);
    if (ec || base_path.empty()) {
      base_path = std::filesystem::absolute(root, ec);
    }
    std::string base = base_path.lexically_normal().string();
    if (base.empty()) {
      continue;
    }
    if (resolved_str == base) {
      return resolved_str;
    }
    if (base.back() != static_cast<char>(std::filesystem::path::preferred_separator)) {
      base += static_cast<char>(std::filesystem::path::preferred_separator);
    }
    if (resolved_str.rfind(base, 0) == 0) {
      return resolved_str;
    }
  }
  throw ConfigurationError("Path '" + target + "' is outside the allowed filesystem roots.");
}

std::vector<ToolDefinition> create_local_builtin_tools(std::vector<std::string> allowed_roots,
                                                       bool allow_unconfined) {
  // Secure by default: with confinement enabled and no explicit roots, the
  // fs.* tools are confined to the process working directory. An empty
  // `effective_roots` (only when allow_unconfined) disables confinement.
  std::vector<std::string> effective_roots;
  if (!allow_unconfined) {
    effective_roots = allowed_roots.empty()
                          ? std::vector<std::string>{std::filesystem::current_path().string()}
                          : std::move(allowed_roots);
  }

  JsonSchema path_schema;
  path_schema.type = JsonSchemaType::Object;
  path_schema.required = {"path"};
  path_schema.properties["path"].type = JsonSchemaType::String;

  JsonSchema write_schema = path_schema;
  write_schema.required = {"path", "content"};
  write_schema.properties["content"].type = JsonSchemaType::String;
  // Optional: caller-supplied sha256 of the file as last seen. When provided,
  // the write is refused with `{ ok:false, reason:"stale_read", actualSha256 }`
  // if the file's current contents (or absence) don't match. Prevents agents
  // from overwriting a file that changed since they last read it.
  write_schema.properties["expectedSha256"].type = JsonSchemaType::String;

  return {
      define_tool(ToolDefinition{
          .name = "fs.readText",
          .description = "Read a text file from disk.",
          .input_schema = path_schema,
          .capabilities = {"fs.read"},
          .risk_level = ToolRiskLevel::Medium,
          .builtin = true,
          .execute = [effective_roots](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            const auto text = bytes_to_text(
                read_binary_file(confine_fs_path(input.at("path").as_string(), effective_roots)));
            return Value::object({{"content", truncated_output_to_value(truncate_for_model(text))}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "fs.writeText",
          .description = "Write a text file to disk. Supports optional `expectedSha256` "
                         "for stale-read detection (refuses to overwrite if the on-disk "
                         "contents have changed).",
          .input_schema = write_schema,
          .capabilities = {"fs.write"},
          .risk_level = ToolRiskLevel::High,
          .builtin = true,
          .execute = [effective_roots](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            const std::string path = confine_fs_path(input.at("path").as_string(), effective_roots);
            const std::string content = input.at("content").as_string();
            const bool has_expected = input.contains("expectedSha256") &&
                                      !input.at("expectedSha256").is_null() &&
                                      !input.at("expectedSha256").as_string().empty();
            if (context.service_refs.hooks && context.service_refs.hooks->before_fs_write) {
              FsWriteHookContext hook_context;
              hook_context.target = ExecutionTarget::Tool;
              hook_context.trace_id = context.trace_context.trace_id;
              hook_context.run_id = context.trace_context.run_id;
              hook_context.workflow_run_id = context.trace_context.workflow_run_id;
              hook_context.path = path;
              hook_context.content = content;
              hook_context.tool_name = "fs.writeText";
              context.service_refs.hooks->before_fs_write(hook_context);
            }
            if (has_expected) {
              const std::string expected = input.at("expectedSha256").as_string();
              std::string actual;
              const bool exists = std::filesystem::exists(path);
              if (exists) {
                actual = sha256_hex(bytes_to_text(read_binary_file(path)));
              }
              if (!exists || actual != expected) {
                return Value::object({
                    {"ok", false},
                    {"reason", std::string("stale_read")},
                    {"actualSha256", exists ? actual : std::string()},
                });
              }
            }
            // Capture pre-write state so we can produce a unified diff for UI
            // rendering. Failures reading the old file (binary / permission)
            // are tolerated — we fall through to treating the old content as
            // empty so the new file appears as all-additions.
            const bool was_existing = std::filesystem::exists(path);
            std::string old_content;
            if (was_existing) {
              try {
                old_content = bytes_to_text(read_binary_file(path));
              } catch (...) {
                // leave old_content empty; whole-file diff will follow
              }
            }
            const bool binary = old_content.find('\0') != std::string::npos ||
                                content.find('\0') != std::string::npos;
            constexpr std::size_t kInlineDiffByteCap = 1024 * 1024;  // 1MB per side
            constexpr std::size_t kDiffTextByteCap   = 16 * 1024;    // 16KB before truncation
            const bool too_large = old_content.size() > kInlineDiffByteCap ||
                                   content.size() > kInlineDiffByteCap;
            auto count_lines = [](const std::string& text) -> int {
              if (text.empty()) return 0;
              int n = static_cast<int>(std::count(text.begin(), text.end(), '\n'));
              if (text.back() != '\n') ++n;
              return n;
            };
            std::string diff_text;
            int lines_added = 0;
            int lines_removed = 0;
            bool truncated = false;
            if (binary) {
              diff_text = "(binary file)";
              const int new_lines = count_lines(content);
              const int old_lines = count_lines(old_content);
              lines_added   = std::max(0, new_lines - old_lines);
              lines_removed = std::max(0, old_lines - new_lines);
            } else if (too_large) {
              diff_text = "(file too large for inline diff)";
              const int new_lines = count_lines(content);
              const int old_lines = count_lines(old_content);
              lines_added   = std::max(0, new_lines - old_lines);
              lines_removed = std::max(0, old_lines - new_lines);
            } else {
              diff_text = compute_unified_diff(old_content, content,
                                                std::string("a/") + path,
                                                std::string("b/") + path);
              // Count +/- lines (excluding the +++/--- headers).
              std::istringstream stream(diff_text);
              std::string line;
              while (std::getline(stream, line)) {
                if (line.size() >= 2 && line[0] == '+' && line[1] != '+') ++lines_added;
                else if (line.size() >= 2 && line[0] == '-' && line[1] != '-') ++lines_removed;
              }
              // Large diffs blow the agent's next-turn context; truncate text
              // but keep accurate counts (already computed above).
              if (diff_text.size() > kDiffTextByteCap) {
                auto trimmed = truncate_for_model(diff_text, kDiffTextByteCap);
                diff_text = std::move(trimmed.text);
                truncated = trimmed.truncated;
              }
            }
            write_text_file(path, content);
            Value result = Value::object({
                {"ok", true},
                {"newFile", !was_existing},
                {"diff", diff_text},
                {"linesAdded", lines_added},
                {"linesRemoved", lines_removed},
            });
            if (binary)    result["binary"] = true;
            if (truncated) result["truncated"] = true;
            return result;
          },
      }),
      define_tool(ToolDefinition{
          .name = "fs.listDirectory",
          .description = "List files in a directory.",
          .input_schema = path_schema,
          .capabilities = {"fs.read"},
          .risk_level = ToolRiskLevel::Medium,
          .builtin = true,
          .execute = [effective_roots](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            Value::Array entries;
            for (const auto& entry : std::filesystem::directory_iterator(
                     confine_fs_path(input.at("path").as_string(), effective_roots))) {
              entries.emplace_back(entry.path().filename().string());
            }
            std::sort(entries.begin(), entries.end(), [](const Value& left, const Value& right) {
              return left.as_string() < right.as_string();
            });
            return Value::object({{"entries", Value(entries)}});
          },
      }),
  };
}

std::vector<ToolDefinition> create_http_builtin_tools(HttpTransport transport) {
  JsonSchema request_schema;
  request_schema.type = JsonSchemaType::Object;
  request_schema.required = {"url"};
  request_schema.properties["url"].type = JsonSchemaType::String;
  request_schema.properties["method"].type = JsonSchemaType::String;
  request_schema.properties["headers"].type = JsonSchemaType::Object;
  request_schema.properties["body"] = JsonSchema{};

  JsonSchema inspect_schema;
  inspect_schema.type = JsonSchemaType::Object;
  inspect_schema.required = {"url"};
  inspect_schema.properties["url"].type = JsonSchemaType::String;

  return {
      define_tool(ToolDefinition{
          .name = "http.request",
          .description = "Perform an HTTP request and return status, headers, and (possibly truncated) body.",
          .input_schema = request_schema,
          .capabilities = {"network.http.read", "network.http.write"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"http", "network"},
          .bundle = "http",
          .builtin = true,
          .execute = [transport](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto resolved_transport = require_http_transport(transport);
            HttpRequest request;
            request.url = input.at("url").as_string();
            request.method = uppercase_copy(input.at("method").as_string("GET"));
            request.headers = string_map_from_value(input.at("headers"));
            request.body = request_body_from_value(input.at("body"));
            request.cancellation = context.cancellation;
            auto response = resolved_transport(request);
            return Value::object({{"status", response.status},
                                  {"headers", headers_to_value(response.headers)},
                                  {"body", truncated_output_to_value(truncate_for_model(response.body))}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "http.inspectJson",
          .description = "Fetch a JSON endpoint and return parsed data.",
          .input_schema = inspect_schema,
          .capabilities = {"network.http.read"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"http", "json"},
          .bundle = "http",
          .builtin = true,
          .execute = [transport](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto resolved_transport = require_http_transport(transport);
            HttpRequest request;
            request.url = input.at("url").as_string();
            request.method = "GET";
            request.headers = {{"content-type", "application/json"}};
            request.cancellation = context.cancellation;
            auto response = resolved_transport(request);
            return Value::object({{"status", response.status},
                                  {"data", parse_json(response.body)}});
          },
      }),
  };
}

std::vector<ToolDefinition> create_developer_builtin_tools(DeveloperProcessExecutor executor) {
  JsonSchema exec_schema;
  exec_schema.type = JsonSchemaType::Object;
  exec_schema.required = {"command"};
  exec_schema.properties["command"].type = JsonSchemaType::String;
  exec_schema.properties["args"].type = JsonSchemaType::Array;
  exec_schema.properties["args"].items = std::make_shared<JsonSchema>();
  exec_schema.properties["args"].items->type = JsonSchemaType::String;
  exec_schema.properties["cwd"].type = JsonSchemaType::String;

  // Helper for git.snapshot/undo.last. Runs `git <args>` via popen and returns
  // {ok, stdout, stderr_via_2>&1, exit_code}. Quotes args minimally; not safe
  // for arbitrary user input, only static arguments composed by these tools.
  auto run_git = [](const std::string& cwd,
                    const std::vector<std::string>& args) -> std::tuple<bool, std::string, int> {
    std::string cmd = "git";
    if (!cwd.empty()) {
      cmd += " -C '";
      for (char c : cwd) {
        if (c == '\'') cmd += "'\\''";
        else cmd += c;
      }
      cmd += "'";
    }
    for (const auto& arg : args) {
      cmd += " '";
      for (char c : arg) {
        if (c == '\'') cmd += "'\\''";
        else cmd += c;
      }
      cmd += "'";
    }
    cmd += " 2>&1";
#ifdef _WIN32
    return {false, "git tools unsupported on Windows", -1};
#else
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe) return {false, "popen failed", -1};
    std::string out;
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    const int rc = ::pclose(pipe);
    const int exit_code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
    return {exit_code == 0, out, exit_code};
#endif
  };

  return {
      define_tool(ToolDefinition{
          .name = "shell.exec",
          .description = "Execute a local process.",
          .input_schema = exec_schema,
          .capabilities = {"process.exec"},
          .risk_level = ToolRiskLevel::High,
          .tags = {"developer", "process"},
          .bundle = "developer",
          .builtin = true,
          // Shell execution needs filesystem + child-process capabilities.
          // The executor enforces this contract before dispatch; the active
          // request is forwarded to the business process executor below so it
          // can pick the matching platform mechanism.
          .sandbox_policy = ToolSandboxPolicy{ .required = sandbox_presets::shell_safe() },
          .execute = [executor = std::move(executor)](const Value& input,
                                                       ToolExecutionContext& context) -> ToolInvokeResult {
            if (!executor) {
              throw ConfigurationError("shell.exec requires a DeveloperProcessExecutor.");
            }
            DeveloperProcessRequest request;
            request.command = input.at("command").as_string();
            request.args = string_array_from_value(input.at("args"));
            request.cwd = input.at("cwd").as_string();
            request.cancellation = context.cancellation;
            // Propagate the effective sandbox request so the business
            // executor picks the matching platform isolation mechanism.
            // Contract has already been enforced by the framework; the active
            // scope (if any) carries the request the executor entered with.
            request.sandbox_request = context.service_refs.sandbox_scope
                                          ? context.service_refs.sandbox_scope->request()
                                          : SandboxRequest{};
            auto result = executor(request);
            return Value::object({
                {"stdout", truncated_output_to_value(truncate_for_model(result.stdout_text))},
                {"stderr", truncated_output_to_value(truncate_for_model(result.stderr_text))},
                {"exitCode", result.exit_code},
            });
          },
      }),
      define_tool(ToolDefinition{
          .name = "git.snapshot",
          .description = "Snapshot the working tree into the agent's private snapshot ref "
                         "(refs/agent/snapshots) without polluting branch history. Requires git in PATH.",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.properties["message"].type = JsonSchemaType::String;
            schema.properties["cwd"].type = JsonSchemaType::String;
            schema.properties["paths"].type = JsonSchemaType::Array;
            schema.properties["paths"].items = std::make_shared<JsonSchema>();
            schema.properties["paths"].items->type = JsonSchemaType::String;
            return schema;
          }(),
          .capabilities = {"git.write"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"developer", "git"},
          .bundle = "developer",
          .builtin = true,
          .execute = [run_git](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            const std::string cwd = input.at("cwd").as_string();
            const std::string message = input.at("message").as_string("agent snapshot");
            std::vector<std::string> paths;
            if (input.contains("paths") && input.at("paths").is_array()) {
              for (const auto& p : input.at("paths").as_array()) {
                paths.push_back(p.as_string());
              }
            }
            // Probe git availability.
            auto [probe_ok, probe_out, probe_rc] = run_git(cwd, {"rev-parse", "--git-dir"});
            if (!probe_ok) {
              return Value::object({{"ok", false}, {"reason", std::string("git_not_available")}});
            }
            // Stash original HEAD ref so we can reset back to it.
            auto [head_ok, head_out, head_rc] = run_git(cwd, {"rev-parse", "HEAD"});
            std::string original_head;
            if (head_ok) {
              original_head = head_out;
              if (!original_head.empty() && original_head.back() == '\n') original_head.pop_back();
            }
            std::vector<std::string> add_args = {"add"};
            if (paths.empty()) {
              add_args.emplace_back(".");
            } else {
              for (const auto& p : paths) add_args.push_back(p);
            }
            run_git(cwd, add_args);
            auto [commit_ok, commit_out, commit_rc] = run_git(
                cwd, {"commit", "-m", message, "--allow-empty", "--no-verify"});
            if (!commit_ok) {
              return Value::object({{"ok", false}, {"reason", std::string("commit_failed")},
                                    {"detail", commit_out}});
            }
            auto [snap_ok, snap_out, snap_rc] = run_git(cwd, {"rev-parse", "HEAD"});
            std::string snapshot_sha = snap_out;
            if (!snapshot_sha.empty() && snapshot_sha.back() == '\n') snapshot_sha.pop_back();
            // Move snapshot ref to this commit.
            run_git(cwd, {"update-ref", "refs/agent/snapshots", snapshot_sha});
            // Restore HEAD to where it was before, leaving working tree intact.
            if (!original_head.empty()) {
              run_git(cwd, {"reset", "--soft", original_head});
            } else {
              run_git(cwd, {"reset", "--soft", "HEAD~1"});
            }
            // Count files in the snapshot vs its parent.
            std::size_t file_count = 0;
            auto [list_ok, list_out, list_rc] =
                run_git(cwd, {"diff-tree", "--no-commit-id", "--name-only", "-r", snapshot_sha});
            if (list_ok) {
              std::stringstream ss(list_out);
              std::string line;
              while (std::getline(ss, line)) {
                if (!line.empty()) ++file_count;
              }
            }
            return Value::object({
                {"ok", true},
                {"commit", snapshot_sha},
                {"files", static_cast<long long>(file_count)},
            });
          },
      }),
      define_tool(ToolDefinition{
          .name = "undo.last",
          .description = "Restore the working tree from the latest snapshot in refs/agent/snapshots and "
                         "rewind the snapshot tip by one commit. Requires git in PATH.",
          .input_schema = [] {
            JsonSchema schema;
            schema.type = JsonSchemaType::Object;
            schema.properties["cwd"].type = JsonSchemaType::String;
            return schema;
          }(),
          .capabilities = {"git.write"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"developer", "git", "undo"},
          .bundle = "developer",
          .builtin = true,
          .execute = [run_git](const Value& input, ToolExecutionContext&) -> ToolInvokeResult {
            const std::string cwd = input.at("cwd").as_string();
            auto [probe_ok, probe_out, probe_rc] = run_git(cwd, {"rev-parse", "--git-dir"});
            if (!probe_ok) {
              return Value::object({{"ok", false}, {"reason", std::string("git_not_available")}});
            }
            auto [tip_ok, tip_out, tip_rc] = run_git(cwd, {"rev-parse", "refs/agent/snapshots"});
            if (!tip_ok) {
              return Value::object({{"restored", false}});
            }
            std::string tip = tip_out;
            if (!tip.empty() && tip.back() == '\n') tip.pop_back();
            auto [checkout_ok, checkout_out, checkout_rc] =
                run_git(cwd, {"checkout", tip, "--", "."});
            if (!checkout_ok) {
              return Value::object({{"restored", false}, {"detail", checkout_out}});
            }
            // Rewind snapshot ref by one (may fail if there is no parent — that's ok).
            auto [parent_ok, parent_out, parent_rc] = run_git(cwd, {"rev-parse", tip + "~1"});
            if (parent_ok) {
              std::string parent = parent_out;
              if (!parent.empty() && parent.back() == '\n') parent.pop_back();
              run_git(cwd, {"update-ref", "refs/agent/snapshots", parent});
            } else {
              run_git(cwd, {"update-ref", "-d", "refs/agent/snapshots"});
            }
            return Value::object({{"restored", true}, {"commit", tip}});
          },
      }),
  };
}

std::vector<ToolDefinition> create_browser_builtin_tools(BrowserRenderer* renderer) {
  JsonSchema render_schema;
  render_schema.type = JsonSchemaType::Object;
  render_schema.required = {"url"};
  render_schema.properties["url"].type = JsonSchemaType::String;
  render_schema.properties["selector"].type = JsonSchemaType::String;
  render_schema.properties["waitUntil"].type = JsonSchemaType::String;
  render_schema.properties["waitUntil"].enum_values = {"load", "domcontentloaded", "networkidle"};
  render_schema.properties["timeoutMs"].type = JsonSchemaType::Integer;

  const auto make_request = [](const Value& input, bool screenshot, CancellationToken* cancellation) {
    BrowserRenderRequest request;
    request.url = input.at("url").as_string();
    request.selector = input.at("selector").as_string();
    request.wait_until = input.at("waitUntil").as_string("networkidle");
    request.timeout_ms = static_cast<int>(input.at("timeoutMs").as_integer(0));
    request.screenshot = screenshot;
    request.cancellation = cancellation;
    return request;
  };

  const auto require_renderer = [](BrowserRenderer* value) -> BrowserRenderer& {
    if (!value) {
      throw ConfigurationError("Browser renderer is not configured.");
    }
    return *value;
  };

  return {
      define_tool(ToolDefinition{
          .name = "browser.render",
          .description = "Render a page through a configured headless browser.",
          .input_schema = render_schema,
          .capabilities = {"browser.read"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"browser", "render"},
          .bundle = "browser",
          .builtin = true,
          .execute = [renderer, make_request, require_renderer](const Value& input,
                                                                 ToolExecutionContext& context) -> ToolInvokeResult {
            BrowserRenderer* active_renderer = renderer ? renderer : context.service_refs.browser_renderer;
            return browser_render_result_to_value(
                require_renderer(active_renderer).render(make_request(input, false, context.cancellation)));
          },
      }),
      define_tool(ToolDefinition{
          .name = "browser.extract",
          .description = "Extract rendered text from a page or selector.",
          .input_schema = render_schema,
          .capabilities = {"browser.read"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"browser", "extract"},
          .bundle = "browser",
          .builtin = true,
          .execute = [renderer, make_request, require_renderer](const Value& input,
                                                                 ToolExecutionContext& context) -> ToolInvokeResult {
            BrowserRenderer* active_renderer = renderer ? renderer : context.service_refs.browser_renderer;
            return browser_render_result_to_value(
                require_renderer(active_renderer).render(make_request(input, false, context.cancellation)));
          },
      }),
      define_tool(ToolDefinition{
          .name = "browser.screenshot",
          .description = "Capture a screenshot of a page through a configured headless browser.",
          .input_schema = render_schema,
          .capabilities = {"browser.screenshot"},
          .risk_level = ToolRiskLevel::High,
          .tags = {"browser", "screenshot"},
          .bundle = "browser",
          .builtin = true,
          .execute = [renderer, make_request, require_renderer](const Value& input,
                                                                 ToolExecutionContext& context) -> ToolInvokeResult {
            BrowserRenderer* active_renderer = renderer ? renderer : context.service_refs.browser_renderer;
            return browser_render_result_to_value(
                require_renderer(active_renderer).render(make_request(input, true, context.cancellation)));
          },
      }),
  };
}

std::vector<ToolDefinition> create_web_builtin_tools(WebSearchProviderRegistry* search_registry,
                                                     std::string default_search_provider,
                                                     NativeWebPageFetcher* fetcher) {
  JsonSchema search_schema;
  search_schema.type = JsonSchemaType::Object;
  search_schema.required = {"query"};
  search_schema.properties["query"].type = JsonSchemaType::String;
  search_schema.properties["provider"].type = JsonSchemaType::String;
  search_schema.properties["topK"].type = JsonSchemaType::Integer;
  search_schema.properties["locale"].type = JsonSchemaType::String;
  search_schema.properties["recencyDays"].type = JsonSchemaType::Integer;
  search_schema.properties["safeSearch"].type = JsonSchemaType::String;
  search_schema.properties["safeSearch"].enum_values = {"off", "moderate", "strict"};
  search_schema.properties["domains"].type = JsonSchemaType::Array;
  search_schema.properties["domains"].items = std::make_shared<JsonSchema>();
  search_schema.properties["domains"].items->type = JsonSchemaType::String;

  JsonSchema fetch_schema;
  fetch_schema.type = JsonSchemaType::Object;
  fetch_schema.required = {"url"};
  fetch_schema.properties["url"].type = JsonSchemaType::String;
  fetch_schema.properties["timeoutMs"].type = JsonSchemaType::Integer;
  fetch_schema.properties["extract"].type = JsonSchemaType::String;
  fetch_schema.properties["extract"].enum_values = {"auto", "html", "text", "markdown"};

  return {
      define_tool(ToolDefinition{
          .name = "web.search",
          .description = "Search the web using a configured search provider.",
          .input_schema = search_schema,
          .capabilities = {"network.search"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"web", "search"},
          .bundle = "web",
          .builtin = true,
          .execute = [search_registry, default_search_provider = std::move(default_search_provider)](
                         const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            WebSearchProviderRegistry* active_registry =
                search_registry ? search_registry : context.service_refs.web_search_registry;
            const std::string active_default_provider = default_search_provider.empty()
                                                            ? context.service_refs.default_search_provider
                                                            : default_search_provider;
            const auto& provider = resolve_web_search_provider(active_registry, active_default_provider,
                                                               input.at("provider").as_string());
            WebSearchQuery query;
            query.query = input.at("query").as_string();
            query.top_k = static_cast<std::size_t>(std::max<long long>(0, input.at("topK").as_integer(8)));
            query.locale = input.at("locale").as_string();
            query.recency_days = static_cast<int>(input.at("recencyDays").as_integer(0));
            query.domains = string_array_from_value(input.at("domains"));
            query.safe_search = input.at("safeSearch").as_string("moderate");
            query.cancellation = context.cancellation;
            return Value::object({{"provider", provider.name()},
                                  {"results", web_results_to_value(provider.search(query))}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "web.fetch",
          .description = "Fetch a web page and extract normalized content.",
          .input_schema = fetch_schema,
          .capabilities = {"network.http.read"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"web", "fetch"},
          .bundle = "web",
          .builtin = true,
          .execute = [fetcher](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            WebFetchRequest request;
            request.url = input.at("url").as_string();
            if (input.at("timeoutMs").is_number()) {
              request.timeout_ms = static_cast<int>(input.at("timeoutMs").as_integer());
            }
            request.extract = input.at("extract").as_string("auto");
            request.cancellation = context.cancellation;
            NativeWebPageFetcher* active_fetcher = fetcher ? fetcher : context.service_refs.web_fetcher;
            Value page = web_fetched_page_to_value(fetch_web_page_with_default(active_fetcher, request));
            if (page.is_object() && page.contains("text") && page.at("text").is_string()) {
              page["text"] = truncated_output_to_value(truncate_for_model(page.at("text").as_string()));
            }
            return page;
          },
      }),
  };
}

std::vector<ToolDefinition> create_agent_builtin_tools(SessionMemory* session,
                                                       KnowledgeBase* knowledge_base,
                                                       KnowledgeBaseManager* knowledge_manager) {
  JsonSchema search_schema;
  search_schema.type = JsonSchemaType::Object;
  search_schema.required = {"query"};
  search_schema.properties["query"].type = JsonSchemaType::String;
  search_schema.properties["topK"].type = JsonSchemaType::Integer;

  return {
      define_tool(ToolDefinition{
          .name = "session.snapshot",
          .description = "Read the current session snapshot.",
          .capabilities = {"memory.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"agent", "session", "memory"},
          .bundle = "agent",
          .builtin = true,
          .execute = [session](const Value&, ToolExecutionContext& context) -> ToolInvokeResult {
            SessionMemory* active_session = session ? session : context.service_refs.session;
            if (!active_session) {
              return Value::object({{"session", Value()}});
            }
            return session_memory_snapshot_to_value(active_session->snapshot());
          },
      }),
      define_tool(ToolDefinition{
          .name = "knowledge.search",
          .description = "Search the configured knowledge base.",
          .input_schema = search_schema,
          .capabilities = {"knowledge.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"agent", "knowledge"},
          .bundle = "agent",
          .builtin = true,
          .execute = [knowledge_base, knowledge_manager](const Value& input,
                                                         ToolExecutionContext& context) -> ToolInvokeResult {
            KnowledgeSearchOptions options;
            const auto requested_top_k = input.at("topK").as_integer(4);
            if (requested_top_k <= 0) {
              return Value::object({{"hits", Value::array({})}});
            }
            options.top_k = static_cast<std::size_t>(requested_top_k);
            std::vector<KnowledgeSearchHit> hits;
            KnowledgeBase* active_knowledge_base =
                knowledge_base ? knowledge_base : context.service_refs.knowledge_base;
            KnowledgeBaseManager* active_knowledge_manager =
                knowledge_manager ? knowledge_manager : context.service_refs.knowledge_base_manager;
            if (active_knowledge_base) {
              hits = active_knowledge_base->search(input.at("query").as_string(), options);
            } else if (active_knowledge_manager) {
              ManagerSearchOptions manager_options;
              manager_options.top_k = options.top_k;
              hits = active_knowledge_manager->search(input.at("query").as_string(), manager_options);
            } else {
              return Value::object({{"hits", Value::array({})}});
            }
            return Value::object({{"hits", knowledge_hits_to_value(hits)}});
          },
      }),
  };
}

std::vector<ToolDefinition> create_workflow_builtin_tools(WorkflowEngine* engine) {
  JsonSchema run_schema;
  run_schema.type = JsonSchemaType::Object;
  run_schema.required = {"workflowRunId"};
  run_schema.properties["workflowRunId"].type = JsonSchemaType::String;

  return {
      define_tool(ToolDefinition{
          .name = "workflow.getRun",
          .description = "Load a workflow run state by id.",
          .input_schema = run_schema,
          .capabilities = {"workflow.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"workflow", "state"},
          .bundle = "workflow",
          .builtin = true,
          .execute = [engine](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            WorkflowEngine* active_engine = engine ? engine : context.service_refs.workflow_engine;
            if (!active_engine || !active_engine->store()) {
              return Value();
            }
            auto state = active_engine->store()->get_run(input.at("workflowRunId").as_string());
            return state ? workflow_run_state_to_value(*state) : Value();
          },
      }),
      define_tool(ToolDefinition{
          .name = "workflow.resume",
          .description = "Resume a workflow run from persisted state.",
          .input_schema = run_schema,
          .capabilities = {"workflow.resume"},
          .risk_level = ToolRiskLevel::Medium,
          .tags = {"workflow", "resume"},
          .bundle = "workflow",
          .builtin = true,
          .execute = [engine](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            WorkflowEngine* active_engine = engine ? engine : context.service_refs.workflow_engine;
            if (!active_engine) {
              return Value();
            }
            return workflow_execution_result_to_value(active_engine->resume(input.at("workflowRunId").as_string()));
          },
      }),
      define_tool(ToolDefinition{
          .name = "workflow.listCheckpoints",
          .description = "List checkpoints for a persisted workflow run.",
          .input_schema = run_schema,
          .capabilities = {"workflow.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"workflow", "checkpoint"},
          .bundle = "workflow",
          .builtin = true,
          .execute = [engine](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            WorkflowEngine* active_engine = engine ? engine : context.service_refs.workflow_engine;
            if (!active_engine || !active_engine->store()) {
              return Value::array({});
            }
            auto state = active_engine->store()->get_run(input.at("workflowRunId").as_string());
            return state ? workflow_checkpoints_to_value(state->checkpoints) : Value::array({});
          },
      }),
  };
}

std::vector<ToolDefinition> create_state_builtin_tools() {
  JsonSchema scratch_set_schema;
  scratch_set_schema.type = JsonSchemaType::Object;
  scratch_set_schema.required = {"key", "value"};
  scratch_set_schema.properties["key"].type = JsonSchemaType::String;
  scratch_set_schema.properties["value"] = JsonSchema{};  // accept any type

  JsonSchema scratch_key_schema;
  scratch_key_schema.type = JsonSchemaType::Object;
  scratch_key_schema.required = {"key"};
  scratch_key_schema.properties["key"].type = JsonSchemaType::String;

  JsonSchema scratch_list_schema;
  scratch_list_schema.type = JsonSchemaType::Object;
  scratch_list_schema.properties["prefix"].type = JsonSchemaType::String;

  JsonSchema todo_create_schema;
  todo_create_schema.type = JsonSchemaType::Object;
  todo_create_schema.required = {"items"};
  todo_create_schema.properties["items"].type = JsonSchemaType::Array;
  todo_create_schema.properties["items"].items = std::make_shared<JsonSchema>();
  todo_create_schema.properties["items"].items->type = JsonSchemaType::Object;
  todo_create_schema.properties["items"].items->required = {"text"};
  todo_create_schema.properties["items"].items->properties["text"].type = JsonSchemaType::String;
  todo_create_schema.properties["items"].items->properties["status"].type = JsonSchemaType::String;

  JsonSchema todo_update_schema;
  todo_update_schema.type = JsonSchemaType::Object;
  todo_update_schema.required = {"id"};
  todo_update_schema.properties["id"].type = JsonSchemaType::String;
  todo_update_schema.properties["status"].type = JsonSchemaType::String;
  todo_update_schema.properties["text"].type = JsonSchemaType::String;

  JsonSchema todo_list_schema;
  todo_list_schema.type = JsonSchemaType::Object;
  todo_list_schema.properties["status"].type = JsonSchemaType::String;

  JsonSchema todo_id_schema;
  todo_id_schema.type = JsonSchemaType::Object;
  todo_id_schema.required = {"id"};
  todo_id_schema.properties["id"].type = JsonSchemaType::String;

  return {
      define_tool(ToolDefinition{
          .name = "scratch.set",
          .description = "Store a value in the per-session scratchpad under the given key.",
          .input_schema = scratch_set_schema,
          .capabilities = {"scratch.write"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "scratch"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const std::string key = input.at("key").as_string();
            if (key.empty()) {
              throw ConfigurationError("scratch.set requires a non-empty key.");
            }
            if (key.rfind(kTodoInternalPrefix, 0) == 0) {
              throw ConfigurationError("scratch.set cannot write to reserved internal keys.");
            }
            store.set(session, key, input.at("value"));
            return Value::object({{"ok", true}, {"key", key}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "scratch.get",
          .description = "Read a value from the per-session scratchpad.",
          .input_schema = scratch_key_schema,
          .capabilities = {"scratch.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "scratch"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const std::string key = input.at("key").as_string();
            const auto stored = store.get(session, key);
            if (!stored) {
              return Value::object({{"key", key}, {"value", Value()}, {"found", false}});
            }
            return Value::object({{"key", key}, {"value", *stored}, {"found", true}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "scratch.list",
          .description = "List per-session scratchpad entries, optionally filtered by key prefix.",
          .input_schema = scratch_list_schema,
          .capabilities = {"scratch.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "scratch"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const std::string prefix = input.contains("prefix") ? input.at("prefix").as_string() : std::string();
            auto entries = store.entries(session, prefix);
            Value::Array array;
            array.reserve(entries.size());
            for (auto& [key, value] : entries) {
              // Internal-key filtering is policy at the tool layer; backends
              // return entries verbatim. Skip `__todo:*` entries from the
              // user-facing scratchpad listing.
              if (key.rfind(kTodoInternalPrefix, 0) == 0) {
                continue;
              }
              array.push_back(Value::object({{"key", key}, {"value", value}}));
            }
            return Value::object({{"entries", Value(array)}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "scratch.delete",
          .description = "Delete a key from the per-session scratchpad.",
          .input_schema = scratch_key_schema,
          .capabilities = {"scratch.write"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "scratch"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const std::string key = input.at("key").as_string();
            if (key.rfind(kTodoInternalPrefix, 0) == 0) {
              throw ConfigurationError("scratch.delete cannot remove reserved internal keys.");
            }
            const bool deleted = store.remove(session, key);
            return Value::object({{"deleted", deleted}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "todo.create",
          .description = "Append todo items to the per-session todo list. Each item gets a generated id and "
                         "createdAt/updatedAt timestamps.",
          .input_schema = todo_create_schema,
          .capabilities = {"scratch.write"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "todo"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            auto list = load_todo_list(store, session);
            Value::Array created;
            const std::string timestamp = now_iso8601();
            for (const auto& item : input.at("items").as_array()) {
              const std::string text = item.at("text").as_string();
              std::string status = "pending";
              if (item.contains("status") && !item.at("status").is_null()) {
                status = item.at("status").as_string("pending");
                if (!is_valid_todo_status(status)) {
                  throw ConfigurationError("todo.create received invalid status: " + status);
                }
              }
              Value entry = Value::object({
                  {"id", generate_uuid()},
                  {"text", text},
                  {"status", status},
                  {"createdAt", timestamp},
                  {"updatedAt", timestamp},
              });
              created.push_back(entry);
              list.push_back(entry);
            }
            save_todo_list(store, session, list);
            return Value::object({{"items", Value(created)}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "todo.update",
          .description = "Update the status and/or text of a todo item by id.",
          .input_schema = todo_update_schema,
          .capabilities = {"scratch.write"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "todo"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const std::string id = input.at("id").as_string();
            auto list = load_todo_list(store, session);
            for (auto& item : list) {
              if (!item.is_object() || !item.contains("id") || item.at("id").as_string() != id) {
                continue;
              }
              if (input.contains("status") && !input.at("status").is_null()) {
                const std::string status = input.at("status").as_string();
                if (!is_valid_todo_status(status)) {
                  throw ConfigurationError("todo.update received invalid status: " + status);
                }
                item["status"] = status;
              }
              if (input.contains("text") && !input.at("text").is_null()) {
                item["text"] = input.at("text").as_string();
              }
              item["updatedAt"] = now_iso8601();
              save_todo_list(store, session, list);
              return Value::object({{"item", item}});
            }
            throw ConfigurationError("todo.update could not find id: " + id);
          },
      }),
      define_tool(ToolDefinition{
          .name = "todo.list",
          .description = "List per-session todo items, optionally filtered by status.",
          .input_schema = todo_list_schema,
          .capabilities = {"scratch.read"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "todo"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const auto list = load_todo_list(store, session);
            const std::string status_filter =
                input.contains("status") && !input.at("status").is_null() ? input.at("status").as_string() : std::string();
            return Value::object({{"items", Value(filter_todo_list(list, status_filter))}});
          },
      }),
      define_tool(ToolDefinition{
          .name = "todo.complete",
          .description = "Mark a todo item as completed. Convenience wrapper over todo.update.",
          .input_schema = todo_id_schema,
          .capabilities = {"scratch.write"},
          .risk_level = ToolRiskLevel::Low,
          .tags = {"state", "todo"},
          .bundle = "state",
          .builtin = true,
          .execute = [](const Value& input, ToolExecutionContext& context) -> ToolInvokeResult {
            auto& store = require_scratch_store(context);
            const std::string session = resolve_session_id(context);
            const std::string id = input.at("id").as_string();
            auto list = load_todo_list(store, session);
            for (auto& item : list) {
              if (!item.is_object() || !item.contains("id") || item.at("id").as_string() != id) {
                continue;
              }
              item["status"] = std::string("completed");
              item["updatedAt"] = now_iso8601();
              save_todo_list(store, session, list);
              return Value::object({{"item", item}});
            }
            throw ConfigurationError("todo.complete could not find id: " + id);
          },
      }),
  };
}

std::vector<ToolBundleProvider> create_builtin_tool_bundle_providers() {
  return {
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "core",
              .tier = "core-safe",
              .title = "Core Builtin Tools",
              .description = "General-purpose low-risk utility tools.",
              .tags = {"builtin", "core"},
              .default_risk_profile = "mixed",
          },
          .create_tools = [] { return create_core_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "local",
              .tier = "core-safe",
              .title = "Local Builtin Tools",
              .description = "File system oriented tools.",
              .tags = {"builtin", "local"},
              .default_risk_profile = "mixed",
          },
          .create_tools = [] { return create_local_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "http",
              .tier = "portable",
              .title = "HTTP Builtin Tools",
              .description = "Network request and endpoint inspection helpers.",
              .tags = {"builtin", "http"},
              .capabilities = {"network.http.read", "network.http.write"},
              .default_risk_profile = "medium",
          },
          .create_tools = [] { return create_http_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "developer",
              .tier = "core-safe",
              .title = "Developer Builtin Tools",
              .description = "Environment and process execution tools.",
              .tags = {"builtin", "developer"},
              .default_risk_profile = "high",
          },
          .create_tools = [] { return create_developer_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "browser",
              .tier = "host-sensitive",
              .title = "Browser Builtin Tools",
              .description = "Controlled browser rendering, extraction, and screenshot tools.",
              .tags = {"builtin", "browser", "render", "extract", "screenshot"},
              .capabilities = {"browser.read", "browser.screenshot"},
              .default_risk_profile = "medium",
          },
          .create_tools = [] { return create_browser_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "web",
              .tier = "portable",
              .title = "Web Builtin Tools",
              .description = "Search and fetch helpers built on pluggable web providers.",
              .tags = {"builtin", "web", "search", "fetch"},
              .capabilities = {"network.search", "network.http.read"},
              .default_risk_profile = "medium",
          },
          .create_tools = [] { return create_web_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "agent",
              .tier = "core-safe",
              .title = "Agent Builtin Tools",
              .description = "Session and knowledge access tools.",
              .tags = {"builtin", "agent"},
              .default_risk_profile = "low",
          },
          .create_tools = [] { return create_agent_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "workflow",
              .tier = "portable",
              .title = "Workflow Builtin Tools",
              .description = "Workflow inspection and resume helpers.",
              .tags = {"builtin", "workflow"},
              .capabilities = {"workflow.read", "workflow.resume"},
              .default_risk_profile = "medium",
          },
          .create_tools = [] { return create_workflow_builtin_tools(); },
      },
      ToolBundleProvider{
          .metadata = ToolBundleMetadata{
              .name = "state",
              .tier = "core-safe",
              .title = "State Builtin Tools",
              .description = "Per-session scratchpad and todo list tools for externalizing agent working memory.",
              .tags = {"builtin", "state", "scratch", "todo"},
              .default_risk_profile = "low",
          },
          .create_tools = [] { return create_state_builtin_tools(); },
      },
  };
}

ToolBundleRegistry create_builtin_tool_bundle_registry() {
  return ToolBundleRegistry(create_builtin_tool_bundle_providers());
}

std::vector<ToolBundleMetadata> list_builtin_tool_bundle_metadata() {
  std::vector<ToolBundleMetadata> metadata;
  auto registry = create_builtin_tool_bundle_registry();
  for (const auto& provider : registry.list()) {
    metadata.push_back(provider.metadata);
  }
  return metadata;
}

std::vector<ToolDefinition> create_builtin_tools(std::vector<std::string> bundles) {
  return create_builtin_tool_bundle_registry().create_tools(std::move(bundles));
}
}  // namespace agent
