AgentServerApp::AgentServerApp(AgentServerOptions options)
    : options_(std::move(options)), metrics_(options_.metrics_enabled, options_.metrics_pricing) {
  if (!options_.approval_mode.empty() && options_.approval_mode != "inherit" &&
      options_.approval_mode != "manual") {
    throw ConfigurationError("AgentServerOptions approval_mode must be \"inherit\" or \"manual\".");
  }
  if (!options_.config.is_null() && !options_.config.is_object()) {
    throw ConfigurationError("AgentServerOptions config must be a JSON object.");
  }
  // Fail-closed: a server with no authentication configured is a
  // misconfiguration, not a default. Refuse to construct unless the operator
  // explicitly opts into a public server via allow_unauthenticated.
  const bool auth_configured = options_.auth || !options_.api_keys.empty();
  if (!auth_configured && !options_.allow_unauthenticated) {
    throw ConfigurationError(
        "Agent server has no authentication configured. Set auth.bearer_tokens or api_keys, "
        "or set allow_unauthenticated=true to run a deliberately public server.");
  }

  if (options_.approvals && options_.approvals->queue) {
    approval_queue_ = options_.approvals->queue;
  } else {
    owned_approval_queue_ = std::make_shared<ManualApprovalQueue>(ManualApprovalQueueConfig{
        .store = options_.approvals ? options_.approvals->store : nullptr,
    });
    approval_queue_ = owned_approval_queue_.get();
  }
  if (options_.approval_mode == "manual" && options_.runner && approval_queue_) {
    options_.runner->set_approval_handler(approval_queue_->handler());
  }
  register_builtin_routes();
}

void AgentServerApp::add_route(AgentServerRoute route) {
  route.method = upper_copy(std::move(route.method));
  if (route.method.empty() || route.pattern.empty() || !route.handler) {
    throw ConfigurationError("AgentServerRoute requires method, pattern, and handler.");
  }
  std::lock_guard<std::mutex> lock(routes_mutex_);
  routes_.push_back(std::move(route));
}

void AgentServerApp::add_route_module(const AgentServerRouteModule& module) {
  module.register_routes(*this);
}

std::vector<std::string> AgentServerApp::route_specs() const {
  std::vector<std::string> specs;
  std::lock_guard<std::mutex> lock(routes_mutex_);
  specs.reserve(routes_.size());
  for (const auto& route : routes_) {
    specs.push_back(route.method + " " + route.pattern);
  }
  return specs;
}

HttpResponse AgentServerApp::handle(HttpRequest request) {
  const auto started = now_ms();
  const auto method = upper_copy(request.method.empty() ? "GET" : request.method);
  const auto path = path_from_url(request.url);
  const auto query = query_from_url(request.url);
  std::string route_spec = "unknown";
  std::string api_key_id;
  HttpResponse carried_headers;
  const auto request_trace = ensure_request_trace(request, options_.tracing);
  apply_trace_headers(carried_headers, request_trace, options_.tracing);
  try {
    std::vector<AgentServerRoute> routes;
    {
      std::lock_guard<std::mutex> lock(routes_mutex_);
      routes = routes_;
    }

    if (auto preflight = handle_cors_preflight(request, path)) {
      merge_headers(carried_headers, *preflight);
      return *preflight;
    }

    auto access = enforce_access(request, carried_headers, path, request_trace);
    if (access.api_key) {
      api_key_id = access.api_key->id;
    }

    for (const auto& route : routes) {
      if (route.method != method) {
        continue;
      }
      auto match = match_route_pattern(route.pattern, path);
      if (!match.matched) {
        continue;
      }
      route_spec = route.method + " " + route.pattern;
      AgentServerRequestContext context{
          .request = request,
          .method = method,
          .path = path,
          .query = query,
          .params = std::move(match.params),
          .access = std::move(access),
      };
      auto response = route.handler(context);
      merge_headers(carried_headers, response);
      apply_cors_headers(request, response);
      const auto metrics_agent_id = response.metadata.at("agentId").as_string();
      const auto metrics_model_response = response.metadata.at("modelResponse");
      metrics_.record(route_spec, response.status, now_ms() - started,
                      response.status >= 200 && response.status < 400 ? "succeeded" : "failed",
                      api_key_id, metrics_agent_id, metrics_model_response);
      return response;
    }

    auto response = send_json(404, Value::object({{"error", "Route not found."}, {"path", path}}));
    merge_headers(carried_headers, response);
    apply_cors_headers(request, response);
    metrics_.record(route_spec, 404, now_ms() - started, "failed", api_key_id);
    return response;
  } catch (const std::exception& error) {
    auto response = handle_error(request, error);
    merge_headers(carried_headers, response);
    const std::string kind = response.status == 401 || response.status == 429
                                 ? "rejected"
                                 : "failed";
    const auto duration = now_ms() - started;
    if (kind != "rejected") {
      try {
        append_audit(AuditRecord{
            .type = "http.request.failed",
            .method = method,
            .path = path,
            .api_key_id = api_key_id,
            .request_id = request_trace.request_id,
            .trace_id = request_trace.trace_context.trace_id,
            .status_code = response.status,
            .duration_ms = duration,
            .detail = Value::object({{"error", error.what()}}),
        });
      } catch (const std::exception&) {
        // Preserve the original request failure over audit write failures.
      }
    }
    metrics_.record(route_spec, response.status, duration, kind, api_key_id);
    return response;
  }
}

HttpResponse AgentServerApp::handle_error(const HttpRequest& request, const std::exception& error) {
  const auto* request_error = dynamic_cast<const HttpRequestError*>(&error);
  auto response = request_error
                      ? send_json(request_error->status_code(),
                                  request_error->payload().as_object().empty()
                                      ? Value::object({{"error", error.what()}})
                                      : request_error->payload())
                      : send_json(500, Value::object({{"error", error.what()}}));
  const auto request_trace = ensure_request_trace(request, options_.tracing);
  apply_trace_headers(response, request_trace, options_.tracing);
  apply_cors_headers(request, response);
  return response;
}

const AgentServerMetricsCollector& AgentServerApp::metrics() const noexcept {
  return metrics_;
}

void AgentServerApp::append_audit(AuditRecord record) const {
  append_audit_log(options_.audit_log_path, std::move(record));
}

bool AgentServerApp::has_runner_source() const noexcept {
  return options_.runner != nullptr || static_cast<bool>(options_.create_runner) || has_config_source();
}

bool AgentServerApp::has_config_source() const noexcept {
  return options_.config.is_object() || !options_.config_path.empty();
}

const Value& AgentServerApp::server_config() const {
  if (options_.config.is_object()) {
    return options_.config;
  }
  std::lock_guard<std::mutex> lock(config_load_mutex_);
  if (!loaded_config_) {
    loaded_config_ = load_native_agent_config(options_.config_path, options_.config_module_loader);
  }
  return *loaded_config_;
}

std::string AgentServerApp::resolve_default_agent_id() const {
  if (has_config_source()) {
    return resolve_agent_id(server_config(), options_.agent);
  }
  return options_.agent;
}

std::string AgentServerApp::resolve_authorized_agent_id(const AgentServerAccessContext& access,
                                                        const std::string& requested_agent) const {
  std::string agent_id;
  if (has_config_source()) {
    agent_id = resolve_agent_id(server_config(), requested_agent.empty() ? options_.agent : requested_agent);
  } else {
    agent_id = requested_agent.empty() ? options_.agent : requested_agent;
  }
  if (access.api_key && !access.api_key->agents.empty() && !agent_id.empty() &&
      !contains_string(access.api_key->agents, agent_id)) {
    throw HttpRequestError(403, "API key is not allowed to access agent \"" + agent_id + "\".",
                           Value::object({{"error", "Forbidden."}, {"agentId", agent_id}}));
  }
  return agent_id;
}

std::shared_ptr<AgentRunner> AgentServerApp::create_request_runner(const Value& request_body) const {
  if (!options_.create_runner) {
    return {};
  }
  auto runner = options_.create_runner(request_body);
  if (runner && options_.approval_mode == "manual" && approval_queue_) {
    runner->set_approval_handler(approval_queue_->handler());
  }
  return runner;
}

std::shared_ptr<NativeResolvedAgentApp> AgentServerApp::resolve_config_app(const std::string& agent_id) const {
  if (!has_config_source()) {
    return {};
  }
  const auto& config = server_config();
  const auto resolved_agent_id = resolve_agent_id(config, agent_id.empty() ? options_.agent : agent_id);
  std::lock_guard<std::mutex> lock(config_app_cache_mutex_);
  auto found = config_app_cache_.find(resolved_agent_id);
  if (found != config_app_cache_.end()) {
    return found->second;
  }
  auto app = std::make_shared<NativeResolvedAgentApp>(
      resolve_native_agent_app(config, resolved_agent_id));
  if (app->runner && options_.approval_mode == "manual" && approval_queue_) {
    app->runner->set_approval_handler(approval_queue_->handler());
  }
  config_app_cache_[resolved_agent_id] = app;
  return app;
}

std::shared_ptr<AgentRunner> AgentServerApp::resolve_request_runner(const Value& request_body,
                                                                    const std::string& agent_id) const {
  if (options_.runner) {
    return std::shared_ptr<AgentRunner>(options_.runner, [](AgentRunner*) {});
  }
  if (options_.create_runner) {
    return create_request_runner(request_body);
  }
  auto app = resolve_config_app(agent_id);
  if (!app || !app->runner) {
    return {};
  }
  if (options_.approval_mode == "manual" && approval_queue_) {
    app->runner->set_approval_handler(approval_queue_->handler());
  }
  return app->runner;
}

std::string AgentServerApp::maybe_write_replay(const AgentServerRequestContext& context, const Value& input,
                                               const Value& result, const std::vector<Value>& events,
                                               const std::string& agent_id) const {
  if (options_.replay_dir.empty()) {
    return {};
  }
  const auto replay_agent_id = agent_id.empty() ? options_.server_name : agent_id;
  auto manifest = write_run_replay(options_.replay_dir,
                                   result.at("sessionId").as_string("server"),
                                   input,
                                   result,
                                   events,
                                   {},
                                   replay_agent_id,
                                   {},
                                   Value::object({{"source", "server"}, {"path", context.path}}));
  const auto html_path = options_.replay_dir / sanitize_segment(manifest.session_id) / manifest.run_id /
                         manifest.html_file;
  (void)context;
  return html_path.string();
}

void AgentServerApp::register_builtin_routes() {
  register_core_routes();
  register_session_routes();
  register_approval_routes();
  register_task_routes();
  register_async_run_routes();
  register_autonomous_routes();
  register_workflow_routes();
  register_chat_routes();
}

std::optional<HttpResponse> AgentServerApp::handle_cors_preflight(const HttpRequest& request,
                                                                  const std::string& path) {
  if (upper_copy(request.method) != "OPTIONS") {
    return std::nullopt;
  }
  auto response = HttpResponse{.status = 204};
  const auto origin = header_value(request, "origin");
  if (!origin.empty() && (!options_.cors || !contains_string(options_.cors->origins, origin))) {
    return send_json(403, Value::object({{"error", "CORS origin is not allowed."}}));
  }
  (void)path;
  apply_cors_headers(request, response);
  return response;
}

void AgentServerApp::apply_cors_headers(const HttpRequest& request, HttpResponse& response) const {
  if (!options_.cors) {
    return;
  }
  const auto origin = header_value(request, "origin");
  if (origin.empty() || !contains_string(options_.cors->origins, origin)) {
    return;
  }
  response.headers["access-control-allow-origin"] = origin;
  response.headers["vary"] = "Origin";
  response.headers["access-control-allow-headers"] = join_values(options_.cors->allowed_headers, ",");
  response.headers["access-control-allow-methods"] = join_values(options_.cors->methods, ",");
  if (options_.cors->max_age_seconds) {
    response.headers["access-control-max-age"] = std::to_string(*options_.cors->max_age_seconds);
  }
}

AgentServerAccessContext AgentServerApp::enforce_access(const HttpRequest& request, HttpResponse& response,
                                                        const std::string& path,
                                                        const AgentServerRequestTrace& trace) {
  AgentServerAccessContext access;
  access.request_id = trace.request_id;
  access.trace_context = trace.trace_context;
  if (upper_copy(request.method) == "OPTIONS") {
    return access;
  }

  const std::string auth_header = options_.auth ? options_.auth->header_name : "authorization";
  access.bearer_token = bearer_token(request, auth_header);
  // A missing bearer (empty) never matches a configured secret, so an empty
  // configured token can't authenticate an unauthenticated request.
  if (!access.bearer_token.empty()) {
    for (const auto& key : options_.api_keys) {
      if (constant_time_equals(key.token, access.bearer_token)) {
        access.api_key = key;
        break;
      }
    }
  }

  const bool auth_configured = options_.auth || !options_.api_keys.empty();
  const auto exempt_paths = options_.auth ? options_.auth->exempt_paths : std::vector<std::string>{"/health"};
  if (auth_configured && !path_exempt(path, exempt_paths)) {
    const bool valid_bearer = options_.auth && !access.bearer_token.empty() &&
                              contains_secret(options_.auth->bearer_tokens, access.bearer_token);
    const bool valid_api_key = access.api_key.has_value();
    if (!valid_bearer && !valid_api_key) {
      append_audit(AuditRecord{
          .type = "http.request.rejected",
          .method = upper_copy(request.method.empty() ? "GET" : request.method),
          .path = path,
          .request_id = access.request_id,
          .trace_id = access.trace_context.trace_id,
          .status_code = 401,
          .detail = Value::object({{"reason", "unauthorized"}}),
      });
      throw HttpRequestError(401, "Unauthorized.", Value::object({{"error", "Unauthorized."}}));
    }
  }

  if (access.api_key && access.api_key->quota && !path_exempt(path, exempt_paths)) {
    const auto quota = access.api_key->quota.value();
    auto limit = quota_window_.consume("api-key:" + access.api_key->id, quota.max_requests,
                                       quota.window_ms <= 0 ? 60000 : quota.window_ms);
    access.quota_limit = quota.max_requests;
    access.quota_remaining = limit.remaining;
    access.quota_reset_at_ms = limit.reset_at_ms;
    response.headers["x-quota-limit"] = std::to_string(quota.max_requests);
    response.headers["x-quota-remaining"] = std::to_string(limit.remaining);
    response.headers["x-quota-reset"] = std::to_string(limit.reset_at_ms);
    response.headers["x-api-key-id"] = access.api_key->id;
    if (!limit.allowed) {
      append_audit(AuditRecord{
          .type = "http.request.rejected",
          .method = upper_copy(request.method.empty() ? "GET" : request.method),
          .path = path,
          .api_key_id = access.api_key->id,
          .request_id = access.request_id,
          .trace_id = access.trace_context.trace_id,
          .status_code = 429,
          .detail = Value::object({{"reason", "quota"}}),
      });
      throw HttpRequestError(429, "API key quota exceeded.",
                             Value::object({{"error", "API key quota exceeded."}}));
    }
  }

  if (options_.rate_limit && !path_exempt(path, options_.rate_limit->exempt_paths)) {
    const auto key = options_.rate_limit->key_by == "authorization"
                         ? (access.bearer_token.empty() ? "anonymous" : access.bearer_token)
                         : header_value(request, "x-forwarded-for").empty() ? "anonymous"
                                                                            : header_value(request, "x-forwarded-for");
    auto limit = rate_limit_window_.consume(key,
                                            options_.rate_limit->max_requests,
                                            options_.rate_limit->window_ms <= 0
                                                ? 60000
                                                : options_.rate_limit->window_ms);
    response.headers["x-ratelimit-limit"] = std::to_string(options_.rate_limit->max_requests);
    response.headers["x-ratelimit-remaining"] = std::to_string(limit.remaining);
    response.headers["x-ratelimit-reset"] = std::to_string(limit.reset_at_ms);
    if (!limit.allowed) {
      append_audit(AuditRecord{
          .type = "http.request.rejected",
          .method = upper_copy(request.method.empty() ? "GET" : request.method),
          .path = path,
          .api_key_id = access.api_key ? access.api_key->id : std::string{},
          .request_id = access.request_id,
          .trace_id = access.trace_context.trace_id,
          .status_code = 429,
          .detail = Value::object({{"reason", "rate-limit"},
                                   {"keyBy", options_.rate_limit->key_by.empty()
                                                 ? Value("ip")
                                                 : Value(options_.rate_limit->key_by)}}),
      });
      throw HttpRequestError(429, "Rate limit exceeded.", Value::object({{"error", "Rate limit exceeded."}}));
    }
  }

  if (access.api_key) {
    response.headers["x-api-key-id"] = access.api_key->id;
  }
  return access;
}
