#include "agent/server.hpp"

namespace agent {

HttpResponse AgentServerApp::handle_session_list(const AgentServerRequestContext& context) {
  const auto agent_id = resolve_authorized_agent_id(context.access, {});
  auto* store = session_store(agent_id);
  if (!store) {
    throw HttpRequestError(404, "Session runtime is not configured.",
                           Value::object({{"error", "Session runtime is not configured."}}));
  }

  Value::Array items;
  for (const auto& session_id : store->list_session_ids()) {
    items.push_back(session_id);
  }
  return send_json(200, Value::object({{"items", Value(std::move(items))}}));
}

HttpResponse AgentServerApp::handle_session_get(const AgentServerRequestContext& context) {
  const auto agent_id = resolve_authorized_agent_id(context.access, {});
  auto* store = session_store(agent_id);
  if (!store) {
    throw HttpRequestError(404, "Session runtime is not configured.",
                           Value::object({{"error", "Session runtime is not configured."}}));
  }
  return send_json(200, session_memory_snapshot_to_value(store->get(context.params.at("sessionId"))->snapshot()));
}

HttpResponse AgentServerApp::handle_session_delete(const AgentServerRequestContext& context) {
  const auto agent_id = resolve_authorized_agent_id(context.access, {});
  auto* store = session_store(agent_id);
  if (!store) {
    throw HttpRequestError(404, "Session runtime is not configured.",
                           Value::object({{"error", "Session runtime is not configured."}}));
  }

  const auto session_id = context.params.at("sessionId");
  if (!options_.session.allow_delete) {
    throw HttpRequestError(403, "Session deletion is disabled by session policy.",
                           Value::object({{"error", "Session deletion is disabled by session policy."},
                                          {"sessionId", session_id}}));
  }

  store->clear(session_id);
  append_audit(AuditRecord{
      .type = "session.cleared",
      .method = context.method,
      .path = context.path,
      .api_key_id = context.access.api_key ? context.access.api_key->id : std::string{},
      .request_id = context.access.request_id,
      .trace_id = context.access.trace_context.trace_id,
      .session_id = session_id,
      .status_code = 200,
  });
  return send_json(200, Value::object({{"ok", true}, {"sessionId", session_id}}));
}

}  // namespace agent

