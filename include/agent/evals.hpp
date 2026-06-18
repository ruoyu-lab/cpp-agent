#pragma once

#include "agent/config.hpp"
#include "agent/model_providers.hpp"

namespace agent {

struct EvalAssertionResult {
  std::string name;
  bool passed = false;
  std::string message;
};

struct EvalUsage {
  int input_tokens = 0;
  int output_tokens = 0;
  int total_tokens = 0;
  std::optional<double> estimated_cost;
  std::string currency = "USD";
};

struct EvalPermissionEvent {
  std::string category;
  std::string tool_name;
  std::string reason;
  std::string decision;
  Value raw = Value::object({});
};

struct EvalPricing {
  std::optional<double> input_per_1k_tokens;
  std::optional<double> output_per_1k_tokens;
  std::string currency = "USD";
};

struct EvalAssertionContext;

using EvalAssertionFunction = std::function<EvalAssertionResult(const EvalAssertionContext&)>;

struct EvalAssertion {
  std::string name;
  EvalAssertionFunction evaluate;
  Value serialized = Value::object({});
};

struct EvalTextMatcher {
  std::string value;
  bool regex = false;
  std::string flags;
};

struct EvalCase {
  std::string id;
  std::string input;
  Value input_value;
  std::string session_id;
  ModelSettings model_settings;
  std::vector<std::string> expect_contains;
  std::vector<std::string> expect_not_contains;
  std::vector<EvalTextMatcher> expect_contains_matchers;
  std::vector<EvalTextMatcher> expect_not_contains_matchers;
  std::size_t min_output_length = 0;
  std::vector<EvalAssertion> expect;
  std::vector<std::string> tags;
  bool skip = false;
  bool only = false;
  Value metadata = Value::object({});
};

struct EvalCaseResult {
  std::string id;
  bool passed = false;
  std::string session_id;
  int duration_ms = 0;
  std::string output;
  std::vector<std::string> tool_calls;
  std::vector<std::string> status_stages;
  std::vector<EvalAssertionResult> assertions;
  EvalUsage usage;
  std::size_t knowledge_hit_count = 0;
  std::size_t citation_count = 0;
  std::vector<EvalPermissionEvent> permission_events;
  std::string replay_path;
  std::vector<std::string> tags;
  std::string error;
  Value metadata = Value::object({});
};

struct EvalSuite {
  std::string agent;
  std::optional<EvalPricing> pricing;
  std::filesystem::path replay_dir;
  std::vector<EvalCase> cases;
  Value metadata = Value::object({});
};

struct EvalReport {
  std::string started_at;
  std::string finished_at;
  int duration_ms = 0;
  std::size_t total_cases = 0;
  std::size_t passed_cases = 0;
  std::size_t failed_cases = 0;
  std::vector<EvalCaseResult> results;
  Value metadata = Value::object({});
};

struct RunEvalSuiteOptions {
  EvalSuite suite;
  AgentRunner* runner = nullptr;
  std::function<std::shared_ptr<AgentRunner>(const EvalCase&)> create_runner;
  Value config;
  std::optional<NativeLoadedAgentConfig> loaded_config;
  std::filesystem::path config_path;
  std::string agent;
  std::function<Value(const std::filesystem::path&)> config_module_loader;
  NativeProviderTransport provider_transport;
  NativeProviderStreamTransport provider_stream_transport;
  NativeProviderStreamingTransport provider_streaming_transport;
  NativeMCPTransportFactory mcp_transport_factory;
  NativeWebAdapters web_adapters;
  NativeDeveloperAdapters developer_adapters;
  NativeBrowserAdapters browser_adapters;
  NativeLlamaCppAdapters llama_adapters;
  std::vector<std::string> case_ids;
  std::vector<std::string> tags;
  bool stop_on_failure = false;
  std::filesystem::path report_path;
  std::filesystem::path markdown_report_path;
  std::filesystem::path replay_dir;
  std::filesystem::path baseline_path;
  bool update_baseline = false;
  CancellationToken* cancellation = nullptr;
};

struct EvalAssertionContext {
  const EvalCase* test_case = nullptr;
  const AgentRunnerRunResult* result = nullptr;
  std::vector<AgentRunnerStreamEvent> events;
  std::vector<std::string> tool_calls;
  std::vector<std::string> status_stages;
  int duration_ms = 0;
  EvalUsage usage;
  std::size_t knowledge_hit_count = 0;
  std::size_t citation_count = 0;
  std::vector<EvalPermissionEvent> permission_events;
  CancellationToken* cancellation = nullptr;
  // Runner that produced `result`; assertions that need access to the
  // configured adapters (e.g. LLM-as-judge) read it from here.
  AgentRunner* runner = nullptr;
};

struct EvalBaselineCaseDelta {
  std::string id;
  std::string status;
  std::optional<EvalCaseResult> before;
  std::optional<EvalCaseResult> after;
};

struct EvalBaselineComparison {
  bool changed = false;
  std::vector<EvalBaselineCaseDelta> deltas;
};

Value eval_assertion_result_to_value(const EvalAssertionResult& assertion);
EvalAssertionResult eval_assertion_result_from_value(const Value& value);
Value eval_usage_to_value(const EvalUsage& usage);
EvalUsage eval_usage_from_value(const Value& value);
Value eval_pricing_to_value(const EvalPricing& pricing);
EvalPricing eval_pricing_from_value(const Value& value);
Value eval_case_to_value(const EvalCase& test_case);
EvalCase eval_case_from_value(const Value& value);
Value eval_suite_to_value(const EvalSuite& suite);
EvalSuite eval_suite_from_value(const Value& value);
Value eval_case_result_to_value(const EvalCaseResult& result);
EvalCaseResult eval_case_result_from_value(const Value& value);
Value eval_report_to_value(const EvalReport& report);
EvalReport eval_report_from_value(const Value& value);
EvalSuite load_eval_suite(const std::filesystem::path& path);
EvalReport load_eval_report(const std::filesystem::path& path);
EvalSuite define_eval_suite(EvalSuite suite);
EvalReport load_eval_baseline(const std::filesystem::path& path);
void write_eval_report(const EvalReport& report, const std::filesystem::path& path);
void write_eval_baseline(const EvalReport& report, const std::filesystem::path& path);
EvalBaselineComparison compare_eval_baseline(const EvalReport& current, const EvalReport& baseline);
std::string render_eval_report_markdown(const EvalReport& report);
EvalReport run_eval_suite(EvalSuite suite, AgentRunner& runner, std::vector<std::string> case_ids = {},
                          std::vector<std::string> tags = {}, bool stop_on_failure = false,
                          CancellationToken* cancellation = nullptr);
EvalReport run_eval_suite(RunEvalSuiteOptions options);
EvalAssertion expect_output_contains(std::string needle);
EvalAssertion expect_output_not_contains(std::string needle);
EvalAssertion expect_output_matches(std::string pattern, std::string flags = {});
EvalAssertion expect_output_not_matches(std::string pattern, std::string flags = {});
EvalAssertion expect_tool_called(std::string tool_name);
EvalAssertion expect_tool_call_count(std::string tool_name, std::size_t count);
EvalAssertion expect_status_stage_seen(std::string stage);
EvalAssertion expect_latency_under(int limit_ms);
EvalAssertion expect_token_under(int limit);
EvalAssertion expect_cost_under(double limit);
EvalAssertion expect_retrieval_hit_count(std::size_t expected_count);
EvalAssertion expect_citation_count_at_least(std::size_t min_count);
EvalAssertion expect_approval_requested(std::string tool_name = {});
EvalAssertion expect_approval_denied(std::string tool_name = {});
EvalAssertion expect_json_schema(JsonSchema schema);
EvalAssertion expect_custom(std::string name, EvalAssertionFunction evaluate);

// LLM-as-judge assertion. Sends a prompt to the runner's critique adapter
// (falling back to the main adapter — with a `eval.self_grading_detected`
// warning event emitted — if no critique adapter is configured). The judge
// is asked to reply starting with `PASS` or `FAIL`; anything else fails.
EvalAssertion expect_llm_judge(std::string name, std::string rubric);

}  // namespace agent
