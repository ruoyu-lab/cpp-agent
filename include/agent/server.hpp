#pragma once

#include "agent/autonomous.hpp"
#include "agent/config.hpp"
#include "agent/http.hpp"
#include "agent/runtime.hpp"
#include "agent/tasks.hpp"
#include "agent/workflow.hpp"

#include <mutex>

namespace agent {

struct NativeResolvedAgentApp;

class HttpRequestError : public AgentFrameworkError {
 public:
  HttpRequestError(int status_code, std::string message, Value payload = Value::object({}));

  [[nodiscard]] int status_code() const noexcept;
  [[nodiscard]] const Value& payload() const noexcept;

 private:
  int status_code_;
  Value payload_;
};

struct ServerCorsConfig {
  std::vector<std::string> origins;
  std::vector<std::string> allowed_headers = {"authorization", "content-type"};
  std::vector<std::string> methods = {"GET", "POST", "DELETE", "OPTIONS"};
  std::optional<int> max_age_seconds;
};

struct AgentServerAuthConfig {
  std::vector<std::string> bearer_tokens;
  std::string header_name = "authorization";
  std::vector<std::string> exempt_paths = {"/health"};
};

struct AgentServerRateLimitConfig {
  int max_requests = 0;
  int window_ms = 60000;
  std::string key_by = "ip";
  std::vector<std::string> exempt_paths = {"/health"};
};

struct AgentServerApiKeyQuotaConfig {
  int max_requests = 0;
  int window_ms = 60000;
};

struct AgentServerApiKey {
  std::string id;
  std::string token;
  std::vector<std::string> agents;
  std::optional<AgentServerApiKeyQuotaConfig> quota;
  Value metadata = Value::object({});
};

struct AgentServerSessionPolicy {
  std::string mode = "allow";
  std::string id_prefix;
  bool allow_delete = true;
};

struct AgentServerAccessContext {
  std::string bearer_token;
  std::optional<AgentServerApiKey> api_key;
  std::string request_id;
  TraceContext trace_context;
  std::optional<int> quota_limit;
  std::optional<int> quota_remaining;
  std::optional<long long> quota_reset_at_ms;
};

struct AgentServerTracingConfig {
  std::string request_id_header = "x-request-id";
  std::string trace_id_header = "x-trace-id";
  bool response_headers = true;
};

struct AgentServerRequestTrace {
  std::string request_id;
  TraceContext trace_context;
};

struct AuditRecord {
  std::string at;
  std::string type;
  std::string method;
  std::string path;
  std::string agent_id;
  std::string session_id;
  std::string workflow_run_id;
  std::string approval_id;
  std::string api_key_id;
  std::string request_id;
  std::string trace_id;
  std::string task_id;
  std::optional<int> status_code;
  std::optional<long long> duration_ms;
  Value detail = Value::object({});
};

Value audit_record_to_value(const AuditRecord& record);
void append_audit_log(const std::filesystem::path& file_path, AuditRecord record);

enum class ApprovalRecordStatus {
  Pending,
  Resolved,
};

std::string to_string(ApprovalRecordStatus status);
ApprovalRecordStatus approval_record_status_from_string(
    const std::string& value,
    ApprovalRecordStatus fallback = ApprovalRecordStatus::Pending);

Value permission_decision_to_value(const PermissionDecision& decision);
PermissionDecision permission_decision_from_value(const Value& value);
Value permission_request_to_value(const PermissionRequest& request);
PermissionRequest permission_request_from_value(const Value& value);

struct ApprovalRecord {
  std::string id;
  std::string created_at;
  std::string resolved_at;
  ApprovalRecordStatus status = ApprovalRecordStatus::Pending;
  PermissionRequest request;
  PermissionDecision proposed_decision;
  std::optional<PermissionDecision> final_decision;
  Value metadata = Value::object({});
};

struct CreateApprovalRecordInput {
  std::string id;
  PermissionRequest request;
  PermissionDecision proposed_decision;
  Value metadata = Value::object({});
};

struct ApprovalRecordFilter {
  std::optional<ApprovalRecordStatus> status;
};

Value approval_record_to_value(const ApprovalRecord& record);
ApprovalRecord approval_record_from_value(const Value& value);

class ApprovalStore {
 public:
  virtual ~ApprovalStore() = default;
  virtual ApprovalRecord create(CreateApprovalRecordInput input) = 0;
  virtual std::optional<ApprovalRecord> get(const std::string& id) const = 0;
  virtual std::vector<ApprovalRecord> list(ApprovalRecordFilter filter = {}) const = 0;
  virtual std::optional<ApprovalRecord> resolve(const std::string& id, PermissionDecision final_decision) = 0;
};

struct ApprovalStoreState;

class InMemoryApprovalStore : public ApprovalStore {
 public:
  InMemoryApprovalStore();

  ApprovalRecord create(CreateApprovalRecordInput input) override;
  [[nodiscard]] std::optional<ApprovalRecord> get(const std::string& id) const override;
  [[nodiscard]] std::vector<ApprovalRecord> list(ApprovalRecordFilter filter = {}) const override;
  std::optional<ApprovalRecord> resolve(const std::string& id, PermissionDecision final_decision) override;

 protected:
  std::shared_ptr<ApprovalStoreState> state_;
};

class FileApprovalStore : public InMemoryApprovalStore {
 public:
  explicit FileApprovalStore(std::filesystem::path file_path);

  ApprovalRecord create(CreateApprovalRecordInput input) override;
  [[nodiscard]] std::optional<ApprovalRecord> get(const std::string& id) const override;
  [[nodiscard]] std::vector<ApprovalRecord> list(ApprovalRecordFilter filter = {}) const override;
  std::optional<ApprovalRecord> resolve(const std::string& id, PermissionDecision final_decision) override;
  [[nodiscard]] const std::filesystem::path& file_path() const noexcept;

 private:
  void ensure_loaded() const;
  void persist() const;

  std::filesystem::path file_path_;
  mutable bool loaded_ = false;
};

class PgApprovalClient {
 public:
  virtual ~PgApprovalClient() = default;
  virtual std::vector<Value> query(std::string sql, std::vector<Value> params = {}) = 0;
  virtual void close() {}
};

struct PgApprovalStoreConfig {
  std::shared_ptr<PgApprovalClient> client;
  std::string schema_name = "public";
  std::string table_name = "node_agent_approvals";
  bool create_table = false;
  bool close_client_on_close = false;
};

class PgApprovalStore : public ApprovalStore {
 public:
  explicit PgApprovalStore(PgApprovalStoreConfig config);
  ~PgApprovalStore() override;

  PgApprovalStore(const PgApprovalStore&) = delete;
  PgApprovalStore& operator=(const PgApprovalStore&) = delete;

  ApprovalRecord create(CreateApprovalRecordInput input) override;
  [[nodiscard]] std::optional<ApprovalRecord> get(const std::string& id) const override;
  [[nodiscard]] std::vector<ApprovalRecord> list(ApprovalRecordFilter filter = {}) const override;
  std::optional<ApprovalRecord> resolve(const std::string& id, PermissionDecision final_decision) override;
  void close();

 private:
  [[nodiscard]] std::string table() const;
  void ensure_ready() const;

  std::shared_ptr<PgApprovalClient> client_;
  std::string schema_name_;
  std::string table_name_;
  bool create_table_;
  bool close_client_on_close_;
  mutable bool ready_ = false;
  mutable std::mutex ready_mutex_;
};

struct ManualApprovalQueueConfig {
  ApprovalStore* store = nullptr;
};

struct ManualApprovalQueueState;

class ManualApprovalQueue {
 public:
  explicit ManualApprovalQueue(ManualApprovalQueueConfig config = {});

  PermissionDecision approve(const PermissionRequest& request, const PermissionDecision& decision);
  PermissionApprovalHandler handler();
  [[nodiscard]] std::vector<ApprovalRecord> list(ApprovalRecordFilter filter = {}) const;
  [[nodiscard]] std::optional<ApprovalRecord> get(const std::string& id) const;
  std::optional<ApprovalRecord> resolve(const std::string& id, PermissionDecision final_decision);
  [[nodiscard]] ApprovalStore& store() const;

 private:
  std::shared_ptr<ManualApprovalQueueState> state_;
};

PermissionApprovalHandler create_manual_approval_handler(ManualApprovalQueue& queue);

struct AgentServerMetricsBucket {
  int requests = 0;
  int succeeded = 0;
  int failed = 0;
  int rejected = 0;
  long long duration_ms = 0;
  std::map<std::string, int> status_codes;
  int input_tokens = 0;
  int output_tokens = 0;
  int total_tokens = 0;
  std::map<std::string, double> estimated_cost_by_currency;
};

Value agent_server_metrics_bucket_to_value(const AgentServerMetricsBucket& bucket);

class AgentServerMetricsCollector {
 public:
  explicit AgentServerMetricsCollector(bool enabled = true,
                                       std::map<std::string, UsagePricing> pricing = {});
  void record(const std::string& route, int status_code, long long duration_ms, std::string kind,
              std::string api_key_id = {}, std::string agent_id = {},
              Value model_response = {});
  [[nodiscard]] Value snapshot(const std::string& service_name) const;

 private:
  AgentServerMetricsBucket& bucket_for(std::map<std::string, AgentServerMetricsBucket>& buckets,
                                       const std::string& key);
  void apply(AgentServerMetricsBucket& bucket, int status_code, long long duration_ms,
             const std::string& kind, const Value& model_response);
  [[nodiscard]] std::optional<UsagePricing> pricing_for(const std::string& provider,
                                                        const std::string& model) const;

  bool enabled_ = true;
  mutable std::mutex mutex_;
  std::map<std::string, UsagePricing> pricing_;
  AgentServerMetricsBucket totals_;
  std::map<std::string, AgentServerMetricsBucket> routes_;
  std::map<std::string, AgentServerMetricsBucket> agents_;
  std::map<std::string, AgentServerMetricsBucket> api_keys_;
};

class RateLimitWindow {
 public:
  struct Result {
    bool allowed = true;
    int remaining = 0;
    long long reset_at_ms = 0;
  };

  Result consume(const std::string& key, int max_requests, int window_ms);

 private:
  std::mutex mutex_;
  struct Bucket {
    int count = 0;
    long long reset_at_ms = 0;
  };

  std::map<std::string, Bucket> buckets_;
};

struct AgentServerTaskAccessConfig {
  bool require_api_key = false;
  std::string tenant_metadata_key = "tenantId";
  std::string scope = "api-key";
};

struct AgentServerTaskIdempotencyConfig {
  bool require_key = false;
  std::string header_name = "Idempotency-Key";
};

struct AgentServerTasksConfig {
  InMemoryTaskStore* store = nullptr;
  InMemoryTaskQueue* queue = nullptr;
  AgentTaskWorker* worker = nullptr;
  std::string default_type = "agent.run";
  AgentServerTaskAccessConfig access;
  AgentServerTaskIdempotencyConfig idempotency;
};

struct AgentServerAutonomousAccessConfig {
  bool require_api_key = false;
  std::string tenant_metadata_key = "tenantId";
  std::string scope = "api-key";
};

struct AgentServerAutonomousConfig {
  AutonomousStore* store = nullptr;
  AutonomousRunManager* manager = nullptr;
  AgentServerAutonomousAccessConfig access;
};

struct AgentServerWorkflowConfig {
  WorkflowEngine* engine = nullptr;
  WorkflowStore* store = nullptr;
  std::optional<WorkflowDefinition> definition;
};

struct AgentServerApprovalsConfig {
  ApprovalStore* store = nullptr;
  ManualApprovalQueue* queue = nullptr;
};

struct AgentServerPiiGovernanceConfig {
  bool enabled = true;
  std::vector<std::string> detectors = {"email", "phone", "credit-card"};
  std::string replacement = "[REDACTED]";
};

struct AgentServerCitationGovernanceConfig {
  std::string mode = "require";
  std::optional<bool> append_to_text;
  int max_sources = 3;
  std::string header = "Sources";
};

struct AgentServerGovernanceConfig {
  std::optional<AgentServerPiiGovernanceConfig> pii;
  std::optional<AgentServerCitationGovernanceConfig> citations;
  std::optional<JsonSchema> output_schema;
};

struct AgentServerRequestGovernanceConfig {
  std::optional<AgentServerCitationGovernanceConfig> citations;
  std::optional<JsonSchema> output_schema;
};

struct AgentServerGovernanceSummary {
  bool redacted = false;
  bool schema_validated = false;
  int citation_count = 0;
  bool citations_appended = false;
};

Value agent_server_governance_summary_to_value(const AgentServerGovernanceSummary& summary);

using AgentServerRunnerFactory = std::function<std::shared_ptr<AgentRunner>(const Value& request_body)>;

struct AgentServerOptions {
  // Fail-closed: if neither auth nor api_keys is configured, AgentServerApp
  // construction throws a ConfigurationError. Set true to run a deliberately
  // public server (no authentication) — an explicit opt-in to that posture.
  // Declared first so it can be a leading designated initializer.
  bool allow_unauthenticated = false;
  AgentRunner* runner = nullptr;
  AgentServerRunnerFactory create_runner;
  Value config;
  std::filesystem::path config_path;
  NativeConfigModuleLoader config_module_loader;
  std::string server_name = "native-agent";
  std::string agent;
  std::string host = "127.0.0.1";
  int port = 3000;
  std::filesystem::path audit_log_path;
  std::filesystem::path replay_dir;
  std::string approval_mode = "inherit";
  std::optional<AgentServerAuthConfig> auth;
  std::vector<AgentServerApiKey> api_keys;
  std::optional<AgentServerRateLimitConfig> rate_limit;
  std::optional<ServerCorsConfig> cors;
  AgentServerTracingConfig tracing;
  std::optional<AgentServerGovernanceConfig> governance;
  AgentServerSessionPolicy session;
  SessionStore* session_store = nullptr;
  std::optional<AgentServerTasksConfig> tasks;
  std::optional<AgentServerAutonomousConfig> autonomous;
  std::optional<AgentServerWorkflowConfig> workflow;
  std::optional<AgentServerApprovalsConfig> approvals;
  bool metrics_enabled = true;
  std::map<std::string, UsagePricing> metrics_pricing;
};

struct RouteMatch {
  bool matched = false;
  std::map<std::string, std::string> params;
};

struct AgentServerRequestContext {
  HttpRequest request;
  std::string method;
  std::string path;
  std::map<std::string, std::string> query;
  std::map<std::string, std::string> params;
  AgentServerAccessContext access;
};

using AgentServerRouteHandler = std::function<HttpResponse(const AgentServerRequestContext&)>;

struct AgentServerRoute {
  std::string method;
  std::string pattern;
  AgentServerRouteHandler handler;
};

class AgentServerApp;

class AgentServerRouteModule {
 public:
  virtual ~AgentServerRouteModule() = default;
  virtual void register_routes(AgentServerApp& app) const = 0;
};

Value read_json_body(const HttpRequest& request, std::size_t max_bytes = 2 * 1024 * 1024);
HttpResponse send_json(int status_code, Value payload, std::map<std::string, std::string> headers = {});
HttpResponse send_sse(const std::vector<std::pair<std::string, Value>>& events,
                      std::map<std::string, std::string> headers = {});
RouteMatch match_route_pattern(const std::string& pattern, const std::string& path);

class AgentServerApp {
 public:
  explicit AgentServerApp(AgentServerOptions options = {});

  void add_route(AgentServerRoute route);
  void add_route_module(const AgentServerRouteModule& module);
  [[nodiscard]] std::vector<std::string> route_specs() const;
  HttpResponse handle(HttpRequest request);
  HttpResponse handle_error(const HttpRequest& request, const std::exception& error);
  [[nodiscard]] const AgentServerMetricsCollector& metrics() const noexcept;

 private:
  void register_builtin_routes();
  std::optional<HttpResponse> handle_cors_preflight(const HttpRequest& request, const std::string& path);
  void apply_cors_headers(const HttpRequest& request, HttpResponse& response) const;
  AgentServerAccessContext enforce_access(const HttpRequest& request, HttpResponse& response,
                                          const std::string& path,
                                          const AgentServerRequestTrace& trace);
  HttpResponse handle_chat_like(const AgentServerRequestContext& context, bool stream);
  HttpResponse handle_task_list(const AgentServerRequestContext& context);
  HttpResponse handle_task_create(const AgentServerRequestContext& context);
  HttpResponse handle_task_get(const AgentServerRequestContext& context);
  HttpResponse handle_task_events(const AgentServerRequestContext& context);
  HttpResponse handle_task_checkpoints(const AgentServerRequestContext& context);
  HttpResponse handle_task_state(const AgentServerRequestContext& context);
  HttpResponse handle_task_resume(const AgentServerRequestContext& context);
  HttpResponse handle_task_cancel(const AgentServerRequestContext& context);
  HttpResponse handle_autonomous_list(const AgentServerRequestContext& context);
  HttpResponse handle_autonomous_create(const AgentServerRequestContext& context);
  HttpResponse handle_autonomous_get(const AgentServerRequestContext& context);
  HttpResponse handle_autonomous_resume(const AgentServerRequestContext& context);
  HttpResponse handle_autonomous_cancel(const AgentServerRequestContext& context);
  HttpResponse handle_autonomous_complete_step(const AgentServerRequestContext& context);
  HttpResponse handle_workflow_run(const AgentServerRequestContext& context);
  HttpResponse handle_workflow_get(const AgentServerRequestContext& context);
  HttpResponse handle_workflow_checkpoints(const AgentServerRequestContext& context);
  HttpResponse handle_workflow_resume(const AgentServerRequestContext& context);
  HttpResponse handle_workflow_human_response(const AgentServerRequestContext& context);
  HttpResponse handle_workflow_webhook(const AgentServerRequestContext& context);
  HttpResponse handle_approval_list(const AgentServerRequestContext& context);
  HttpResponse handle_approval_resolve(const AgentServerRequestContext& context);
  HttpResponse handle_session_list(const AgentServerRequestContext& context);
  HttpResponse handle_session_get(const AgentServerRequestContext& context);
  HttpResponse handle_session_delete(const AgentServerRequestContext& context);
  void append_audit(AuditRecord record) const;
  struct ResolvedWorkflowRuntime {
    std::string agent_id;
    WorkflowEngine* engine = nullptr;
    WorkflowStore* store = nullptr;
    std::optional<WorkflowDefinition> definition;
    std::shared_ptr<NativeResolvedAgentApp> config_app;
  };
  [[nodiscard]] bool has_runner_source() const noexcept;
  [[nodiscard]] bool has_config_source() const noexcept;
  const Value& server_config() const;
  std::string resolve_default_agent_id() const;
  std::string resolve_authorized_agent_id(const AgentServerAccessContext& access,
                                          const std::string& requested_agent) const;
  std::shared_ptr<AgentRunner> create_request_runner(const Value& request_body) const;
  std::shared_ptr<NativeResolvedAgentApp> resolve_config_app(const std::string& agent_id) const;
  std::shared_ptr<AgentRunner> resolve_request_runner(const Value& request_body,
                                                      const std::string& agent_id) const;
  std::string maybe_write_replay(const AgentServerRequestContext& context, const Value& input,
                                 const Value& result, const std::vector<Value>& events,
                                 const std::string& agent_id = {}) const;
  ManualApprovalQueue* approval_queue() const;
  ResolvedWorkflowRuntime resolve_workflow_runtime(const AgentServerAccessContext& access) const;
  SessionStore* session_store(const std::string& agent_id = {}) const;
  std::string resolve_session_id(const Value& body) const;

  AgentServerOptions options_;
  mutable std::mutex config_load_mutex_;
  mutable std::optional<Value> loaded_config_;
  mutable std::mutex config_app_cache_mutex_;
  mutable std::map<std::string, std::shared_ptr<NativeResolvedAgentApp>> config_app_cache_;
  mutable std::mutex routes_mutex_;
  std::vector<AgentServerRoute> routes_;
  AgentServerMetricsCollector metrics_;
  RateLimitWindow rate_limit_window_;
  RateLimitWindow quota_window_;
  std::shared_ptr<ManualApprovalQueue> owned_approval_queue_;
  ManualApprovalQueue* approval_queue_ = nullptr;
};

AgentServerApp create_agent_server_app(AgentServerOptions options = {});

}  // namespace agent
