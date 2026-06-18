AgentServerApp::ResolvedWorkflowRuntime AgentServerApp::resolve_workflow_runtime(
    const AgentServerAccessContext& access) const {
  ResolvedWorkflowRuntime runtime;
  runtime.agent_id = resolve_authorized_agent_id(access, {});
  if (options_.workflow) {
    runtime.engine = options_.workflow->engine;
    runtime.store = options_.workflow->store ? options_.workflow->store
                                             : (options_.workflow->engine ? options_.workflow->engine->store()
                                                                          : nullptr);
    runtime.definition = options_.workflow->definition;
  }
  if (!runtime.engine && has_config_source()) {
    runtime.config_app = resolve_config_app(runtime.agent_id);
    if (runtime.config_app && runtime.config_app->workflow) {
      runtime.engine = runtime.config_app->workflow->engine.get();
      runtime.store = runtime.config_app->workflow->store
                          ? runtime.config_app->workflow->store.get()
                          : (runtime.engine ? runtime.engine->store() : nullptr);
    }
  }
  return runtime;
}

ManualApprovalQueue* AgentServerApp::approval_queue() const {
  return approval_queue_;
}
