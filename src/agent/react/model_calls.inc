AgentOutput ReActLoop::call_model(int iteration,
                                    const std::vector<AgentMessage>& messages,
                                    const ModelSettings& model_settings,
                                    CancellationToken* cancellation) {
  const auto model_trace = child_or_root_trace_context(config_.trace_context, "react.model");
  const Value request = Value::object({
      {"iteration", iteration},
      {"messageCount", messages.size()},
      {"model", model_settings.model},
  });
  if (config_.hooks.before_model) {
    ModelHookContext hook_context;
    hook_context.target = ExecutionTarget::Model;
    hook_context.trace_id = model_trace.trace_id;
    hook_context.run_id = model_trace.run_id;
    hook_context.workflow_run_id = model_trace.workflow_run_id;
    hook_context.request = request;
    config_.hooks.before_model(hook_context);
  }
  if (config_.event_bus) {
    config_.event_bus->publish("model.started", ExecutionTarget::Model,
                               Value::object({{"iteration", iteration}, {"messageCount", messages.size()}}),
                               model_trace);
  }
  try {
    AgentOutput response = execute_with_policies(
        ExecutionTarget::Model, config_.execution_policies, Value::object({{"iteration", iteration}}), cancellation,
        [&]() {
          return config_.model->generate(GenerateParams{
              .messages = messages,
              .tools = {},
              .settings = config_.model->resolve_settings(model_settings),
              .cancellation = cancellation,
          });
        },
        [&](const RetryScheduledContext& retry) {
          if (config_.event_bus) {
            config_.event_bus->publish(
                "retry.scheduled", ExecutionTarget::Model,
                retry_scheduled_event_payload(retry, Value::object({{"iteration", iteration}})),
                model_trace);
          }
        });
    if (config_.event_bus) {
      config_.event_bus->publish("model.completed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"finishReason", response.finish_reason}}),
                                 model_trace);
      emit_cache_stats_event(config_.event_bus, response, model_settings, model_trace);
    }
    if (config_.hooks.after_model) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.response = model_response_hook_value(response);
      config_.hooks.after_model(hook_context);
    }
    return response;
  } catch (const std::exception& error) {
    if (config_.hooks.on_model_error) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.error = error.what();
      config_.hooks.on_model_error(hook_context);
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.failed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"error", error.what()}}),
                                 model_trace);
    }
    throw;
  }
}

AgentOutput ReActLoop::call_model_stream(int iteration,
                                           const std::vector<AgentMessage>& messages,
                                           const ModelSettings& model_settings,
                                           CancellationToken* cancellation,
                                           AgentLoopStreamEventHandler on_event,
                                           const std::string& reasoning_id,
                                           std::string& turn_reasoning) {
  const auto model_trace = child_or_root_trace_context(config_.trace_context, "react.model");
  const Value request = Value::object({
      {"iteration", iteration},
      {"messageCount", messages.size()},
      {"model", model_settings.model},
      {"stream", true},
  });
  if (config_.hooks.before_model) {
    ModelHookContext hook_context;
    hook_context.target = ExecutionTarget::Model;
    hook_context.trace_id = model_trace.trace_id;
    hook_context.run_id = model_trace.run_id;
    hook_context.workflow_run_id = model_trace.workflow_run_id;
    hook_context.request = request;
    config_.hooks.before_model(hook_context);
  }
  if (config_.event_bus) {
    config_.event_bus->publish("model.started", ExecutionTarget::Model,
                               Value::object({{"iteration", iteration}, {"messageCount", messages.size()}}),
                               model_trace);
  }

  AgentOutput response;
  bool saw_response = false;
  bool saw_text_delta = false;
  bool model_event_emitted = false;
  bool model_start_emitted = false;
  std::string streamed_text;
  IncrementalReActStreamParser incremental(config_.parser_options);
  Utf8StreamBuffer text_utf8("ReAct model text delta");
  Utf8StreamBuffer reasoning_utf8("ReAct model reasoning delta");
  auto emit_model_start = [&](const ModelStreamEvent& event) {
    if (model_start_emitted) {
      return;
    }
    emit_loop_stream_event(on_event, AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelStart,
        .iteration = iteration,
        .provider = event.provider.empty() ? config_.model->provider() : event.provider,
        .model = event.model.empty()
                     ? (model_settings.model.empty() ? config_.model->model() : model_settings.model)
                     : event.model,
    });
    model_start_emitted = true;
  };
  auto emit_visible_delta = [&](const ReActVisibleDelta& visible, const ModelStreamEvent& event) {
    if (visible.delta.empty()) {
      return;
    }
    emit_loop_stream_event(on_event, AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::UserVisibleDelta,
        .iteration = iteration,
        .provider = event.provider.empty() ? config_.model->provider() : event.provider,
        .model = event.model.empty() ? config_.model->model() : event.model,
        .delta = visible.delta,
        .text = visible.text,
    });
    if (config_.event_bus) {
      config_.event_bus->publish("user_visible.delta", ExecutionTarget::Run,
                                 Value::object({{"iteration", iteration},
                                                {"delta", visible.delta},
                                                {"text", visible.text}}),
                                 model_trace);
    }
  };
  auto emit_model_text_delta = [&](const std::string& delta,
                                   const std::string& text,
                                   const ModelStreamEvent& event) {
    if (delta.empty()) {
      return;
    }
    emit_loop_stream_event(on_event, AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelTextDelta,
        .iteration = iteration,
        .provider = event.provider.empty() ? config_.model->provider() : event.provider,
        .model = event.model.empty() ? config_.model->model() : event.model,
        .delta = delta,
        .text = text,
    });
    if (config_.event_bus) {
      config_.event_bus->publish("model.delta", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration},
                                                {"delta", delta},
                                                {"text", text}}),
                                 model_trace);
    }
  };
  auto emit_reasoning_delta = [&](std::string delta, const ModelStreamEvent& event) {
    if (delta.empty()) {
      return;
    }
    turn_reasoning += delta;
    emit_loop_stream_event(on_event, AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelReasoningDelta,
        .iteration = iteration,
        .provider = event.provider.empty() ? config_.model->provider() : event.provider,
        .model = event.model.empty() ? config_.model->model() : event.model,
        .delta = delta,
        .reasoning = turn_reasoning,
        .reasoning_id = reasoning_id,
        .reasoning_scope = "turn",
        .run_id = model_trace.run_id,
    });
    if (config_.event_bus) {
      config_.event_bus->publish("model.reasoning_delta", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration},
                                                {"delta", delta},
                                                {"reasoning", turn_reasoning},
                                                {"reasoningId", reasoning_id},
                                                {"scope", "turn"}}),
                                 model_trace);
    }
  };
  const ModelStreamEvent fallback_stream_event{
      .type = ModelStreamEventType::TextDelta,
      .provider = config_.model->provider(),
      .model = model_settings.model.empty() ? config_.model->model() : model_settings.model,
  };

  auto stream_policies = config_.execution_policies;
  auto retry_it = stream_policies.retry.find(ExecutionTarget::Model);
  if (retry_it != stream_policies.retry.end()) {
    const auto retry_on = retry_it->second.retry_on;
    retry_it->second.retry_on = [retry_on, &model_event_emitted](const RetryContext& retry) {
      if (model_event_emitted) {
        return false;
      }
      return retry_on ? retry_on(retry) : false;
    };
  }

  try {
    (void)execute_with_policies(
        ExecutionTarget::Model, stream_policies, Value::object({{"iteration", iteration}}), cancellation,
        [&]() {
          std::vector<ModelStreamEvent> collected;
          config_.model->stream(GenerateParams{
              .messages = messages,
              .tools = {},
              .settings = config_.model->resolve_settings(model_settings),
              .cancellation = cancellation,
          },
          [&](const ModelStreamEvent& event) {
            collected.push_back(event);
            model_event_emitted = true;
            if (event.type == ModelStreamEventType::ResponseStart) {
              emit_model_start(event);
              return;
            }
            if (event.type == ModelStreamEventType::TextDelta) {
              emit_model_start(event);
              if (event.delta.empty()) {
                return;
              }
              saw_text_delta = true;
              const auto delta = text_utf8.push(event.delta);
              if (delta.empty()) {
                return;
              }
              streamed_text += delta;
              emit_model_text_delta(delta, streamed_text, event);
              if (auto visible = incremental.feed(delta)) {
                emit_visible_delta(*visible, event);
              }
              return;
            }
            if (event.type == ModelStreamEventType::ReasoningDelta) {
              emit_model_start(event);
              if (event.delta.empty()) {
                return;
              }
              const auto delta = reasoning_utf8.push(event.delta);
              emit_reasoning_delta(delta, event);
              return;
            }
            if (event.type == ModelStreamEventType::Response) {
              emit_model_start(event);
              response = event.response;
              saw_response = true;
              if (!saw_text_delta && streamed_text.empty() && !response.text.empty()) {
                const auto delta = text_utf8.push(response.text);
                streamed_text += delta;
                if (!delta.empty()) {
                  emit_model_text_delta(delta, streamed_text, event);
                  if (auto visible = incremental.feed(delta)) {
                    emit_visible_delta(*visible, event);
                  }
                }
              }
            }
          });
          return collected;
        },
        [&](const RetryScheduledContext& retry) {
          if (config_.event_bus) {
            config_.event_bus->publish(
                "retry.scheduled", ExecutionTarget::Model,
                retry_scheduled_event_payload(retry, Value::object({{"iteration", iteration},
                                                                    {"stream", true}})),
                model_trace);
          }
        });
  } catch (const std::exception& error) {
    if (config_.hooks.on_model_error) {
      ModelHookContext hook_context;
      hook_context.target = ExecutionTarget::Model;
      hook_context.trace_id = model_trace.trace_id;
      hook_context.run_id = model_trace.run_id;
      hook_context.workflow_run_id = model_trace.workflow_run_id;
      hook_context.request = request;
      hook_context.error = error.what();
      config_.hooks.on_model_error(hook_context);
    }
    if (config_.event_bus) {
      config_.event_bus->publish("model.failed", ExecutionTarget::Model,
                                 Value::object({{"iteration", iteration}, {"error", error.what()}}),
                                 model_trace);
    }
    throw;
  }

  if (!saw_response) {
    throw AdapterError("Model stream completed without a final response.");
  }
  emit_reasoning_delta(reasoning_utf8.finish(), fallback_stream_event);
  const auto tail = text_utf8.finish();
  if (!tail.empty()) {
    streamed_text += tail;
    emit_model_text_delta(tail, streamed_text, fallback_stream_event);
    if (auto visible = incremental.feed(tail)) {
      emit_visible_delta(*visible, fallback_stream_event);
    }
  }
  if (response.text.empty() && !streamed_text.empty()) {
    response.text = streamed_text;
    if (response.content.empty()) {
      response.content = {text_part(streamed_text)};
    }
  }
  if (response.reasoning && !response.reasoning->text.empty() && turn_reasoning.empty()) {
    turn_reasoning = response.reasoning->text;
  } else if (!turn_reasoning.empty() &&
             (!response.reasoning || response.reasoning->text.empty())) {
    response.reasoning = ModelReasoning{.text = turn_reasoning, .format = "summary"};
  }
  if (auto visible = incremental.finish()) {
    emit_visible_delta(*visible, fallback_stream_event);
  }
  if (config_.event_bus) {
    config_.event_bus->publish("model.completed", ExecutionTarget::Model,
                               Value::object({{"iteration", iteration}, {"finishReason", response.finish_reason}}),
                               model_trace);
    emit_cache_stats_event(config_.event_bus, response, model_settings, model_trace);
  }
  if (config_.hooks.after_model) {
    ModelHookContext hook_context;
    hook_context.target = ExecutionTarget::Model;
    hook_context.trace_id = model_trace.trace_id;
    hook_context.run_id = model_trace.run_id;
    hook_context.workflow_run_id = model_trace.workflow_run_id;
    hook_context.request = request;
    hook_context.response = model_response_hook_value(response);
    config_.hooks.after_model(hook_context);
  }
  return response;
}
