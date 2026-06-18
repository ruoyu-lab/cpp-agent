
SessionStore* AgentServerApp::session_store(const std::string& agent_id) const {
  if (options_.session_store) {
    return options_.session_store;
  }
  if (options_.runner) {
    return options_.runner->sessions().store();
  }
  auto app = resolve_config_app(agent_id);
  return app && app->runner ? app->runner->sessions().store() : nullptr;
}

std::string AgentServerApp::resolve_session_id(const Value& body) const {
  const auto requested = body.at("sessionId").as_string();
  if (options_.session.mode == "require" && requested.empty()) {
    throw HttpRequestError(400, "Session policy requires sessionId.",
                           Value::object({{"error", "Session policy requires sessionId."}}));
  }
  std::string session_id = requested;
  if (options_.session.mode == "ephemeral" || session_id.empty()) {
    session_id = "server:" + std::to_string(now_ms()) + "-" + generate_uuid();
  }
  if (!options_.session.id_prefix.empty() && session_id.rfind(options_.session.id_prefix, 0) != 0) {
    session_id = options_.session.id_prefix + session_id;
  }
  return session_id;
}

AgentServerApp create_agent_server_app(AgentServerOptions options) {
  return AgentServerApp(std::move(options));
}

AgentServerApp create_server_app(AgentServerOptions options) {
  return create_agent_server_app(std::move(options));
}
