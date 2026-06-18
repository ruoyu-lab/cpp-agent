AgentLoopRunResult ReActLoop::run(SessionMemory& session, const std::string& input,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  return run_input(session, create_message(MessageRole::User, input), Value(input), model_settings,
                   preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation, {});
}

AgentLoopRunResult ReActLoop::run(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_parts);
  return run_input(session, create_message(MessageRole::User, std::move(input_parts)), Value(input_text),
                   model_settings, preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation, {});
}

AgentLoopRunResult ReActLoop::run(SessionMemory& session, AgentMessage input_message,
                                  const ModelSettings& model_settings,
                                  const std::vector<AgentMessage>& preface_messages,
                                  ToolExecutionContext tool_context,
                                  Value runtime_context,
                                  AgentLoopDurableOptions durable_options,
                                  CancellationToken* cancellation) {
  return run_input(session, input_to_user_message(std::move(input_message)), Value(), model_settings,
                   preface_messages, std::move(tool_context), std::move(runtime_context),
                   std::move(durable_options), cancellation, {});
}

AgentLoopStreamResult ReActLoop::stream(SessionMemory& session, const std::string& input,
                                        AgentLoopStreamEventHandler on_event,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  auto result = run_input(session, create_message(MessageRole::User, input), Value(input), model_settings,
                          preface_messages, std::move(tool_context), std::move(runtime_context), {},
                          cancellation, std::move(on_event));
  return AgentLoopStreamResult{std::move(result)};
}

AgentLoopStreamResult ReActLoop::stream(SessionMemory& session, std::vector<MessageContentPart> input_parts,
                                        AgentLoopStreamEventHandler on_event,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  const std::string input_text = extract_text_content(input_parts);
  auto result = run_input(session, create_message(MessageRole::User, std::move(input_parts)), Value(input_text),
                          model_settings, preface_messages, std::move(tool_context), std::move(runtime_context), {},
                          cancellation, std::move(on_event));
  return AgentLoopStreamResult{std::move(result)};
}

AgentLoopStreamResult ReActLoop::stream(SessionMemory& session, AgentMessage input_message,
                                        AgentLoopStreamEventHandler on_event,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        CancellationToken* cancellation) {
  auto result = run_input(session, input_to_user_message(std::move(input_message)), Value(), model_settings,
                          preface_messages, std::move(tool_context), std::move(runtime_context), {},
                          cancellation, std::move(on_event));
  return AgentLoopStreamResult{std::move(result)};
}

AgentLoopRunResult ReActLoop::run_input(SessionMemory& session, AgentMessage input_message, Value input_value,
                                        const ModelSettings& model_settings,
                                        const std::vector<AgentMessage>& preface_messages,
                                        ToolExecutionContext tool_context,
                                        Value runtime_context,
                                        AgentLoopDurableOptions durable_options,
                                        CancellationToken* cancellation,
                                        AgentLoopStreamEventHandler on_event) {
  const std::string input_text = extract_text_content(input_message.content);
  const auto input_parts = input_message.content;
  std::vector<AgentTraceEntry> trace = durable_options.resume_state ? durable_options.resume_state->trace
                                                                    : std::vector<AgentTraceEntry>{};
  std::vector<ReActTraceEntry> react_trace = durable_options.resume_state
                                                 ? durable_options.resume_state->react_trace
                                                 : std::vector<ReActTraceEntry>{};
  AgentOutput last_response;
  bool has_last_response = false;
  std::string last_assistant_text = durable_options.resume_state
                                        ? durable_options.resume_state->last_assistant_text
                                        : std::string{};
  std::string latest_non_empty_reasoning = durable_options.resume_state
                                               ? durable_options.resume_state->latest_non_empty_reasoning
                                               : std::string{};
  int consecutive_parse_errors = durable_options.resume_state
                                     ? durable_options.resume_state->consecutive_parse_errors
                                     : 0;
  int consecutive_reasoning_protocol_leaks =
      durable_options.resume_state ? durable_options.resume_state->consecutive_reasoning_protocol_leaks : 0;
  int start_iteration = durable_options.resume_state ? std::max(0, durable_options.resume_state->next_iteration) : 0;
  std::optional<AgentOutput> resume_pending_model_response;
  std::optional<AgentLoopDurablePhase> resume_pending_phase;
  std::uint64_t loop_sequence = 0;
  AgentLoopStreamEventHandler sequenced_on_event;
  if (on_event) {
    sequenced_on_event = [on_event = std::move(on_event), &loop_sequence](const AgentLoopStreamEvent& event) {
      auto sequenced = event;
      sequenced.schema_version = kAgentStreamEventSchemaVersion;
      sequenced.sequence = ++loop_sequence;
      on_event(sequenced);
    };
  }
  const bool wants_stream_events = static_cast<bool>(sequenced_on_event);
  auto emit_stream_event = [&](AgentLoopStreamEvent event) {
    emit_loop_stream_event(sequenced_on_event, std::move(event));
  };
  const std::string run_id = config_.trace_context.run_id.empty()
                                 ? session.session_id()
                                 : config_.trace_context.run_id;
  const std::string reasoning_id = (run_id.empty() ? std::string("react-turn") : run_id) + ":reasoning";
  bool reasoning_completed_emitted = false;
  auto emit_reasoning_completed = [&](int iteration) {
    if (!wants_stream_events || reasoning_completed_emitted || latest_non_empty_reasoning.empty()) {
      return;
    }
    reasoning_completed_emitted = true;
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::ModelReasoningCompleted,
        .iteration = iteration,
        .reasoning = latest_non_empty_reasoning,
        .reasoning_id = reasoning_id,
        .reasoning_scope = "turn",
        .run_id = run_id,
    });
  };

  if (durable_options.resume_state) {
    const auto& resume = *durable_options.resume_state;
    session.restore(durable_options.resume_state->session);
    if (durable_options.resume_state->last_response) {
      last_response = *durable_options.resume_state->last_response;
      has_last_response = true;
      if (resume.phase == AgentLoopDurablePhase::ModelCompleted ||
          resume.phase == AgentLoopDurablePhase::ActionBatchParsed) {
        resume_pending_model_response = *durable_options.resume_state->last_response;
        resume_pending_phase = resume.phase;
      }
    }
  } else {
    session.add(input_message);
  }

  auto checkpoint = [&](AgentLoopDurablePhase phase,
                        int next_iteration,
                        std::optional<AgentLoopTerminationReason> termination_reason = std::nullopt) {
    if (!durable_options.on_checkpoint) {
      return;
    }
    AgentLoopDurableState state;
    state.version = 1;
    state.phase = phase;
    state.session_id = session.session_id();
    state.next_iteration = next_iteration;
    state.input_text = input_text;
    state.input_parts = input_parts;
    state.input_message = input_message;
    state.input_value = input_value;
    state.session = session.snapshot();
    state.trace = trace;
    state.react_trace = react_trace;
    if (has_last_response) {
      state.last_response = last_response;
    }
    state.last_assistant_text = last_assistant_text;
    state.latest_non_empty_reasoning = latest_non_empty_reasoning;
    state.consecutive_parse_errors = consecutive_parse_errors;
    state.consecutive_reasoning_protocol_leaks = consecutive_reasoning_protocol_leaks;
    state.termination_reason = termination_reason;
    durable_options.on_checkpoint(state);
  };

  if (!durable_options.resume_state) {
    checkpoint(AgentLoopDurablePhase::BeforeModel, 0);
  }

  for (int iteration = start_iteration; iteration < config_.max_iterations; ++iteration) {
    if (cancellation) {
      cancellation->throw_if_cancelled(ExecutionTarget::Run);
    }
    const bool resumed_model_response = resume_pending_model_response.has_value();
    const bool resumed_action_batch_parsed =
        resumed_model_response && resume_pending_phase == AgentLoopDurablePhase::ActionBatchParsed;
    AgentOutput response;
    if (resumed_model_response) {
      response = *resume_pending_model_response;
      resume_pending_model_response.reset();
      resume_pending_phase.reset();
    } else {
      maybe_auto_compact_session(session, config_.event_bus, config_.trace_context);
      EmbeddedContextAssembly context_assembly;
      const auto prompt_messages = build_prompt_messages(session, input_text, input_value, iteration,
                                                         preface_messages, runtime_context,
                                                         &context_assembly);
      const auto context_stats = build_context_stats_snapshot(session, input_text, input_value, iteration,
                                                              preface_messages, runtime_context,
                                                              context_assembly,
                                                              prompt_messages);
      if (config_.on_context_stats) {
        config_.on_context_stats(context_stats);
      }
      if (config_.event_bus) {
        Value payload = context_stats_snapshot_to_value(context_stats);
        payload["iteration"] = iteration;
        config_.event_bus->publish("context.stats", ExecutionTarget::Model, std::move(payload),
                                   child_or_root_trace_context(config_.trace_context, "react.context"));
      }
      checkpoint(AgentLoopDurablePhase::BeforeModel, iteration);
      if (wants_stream_events) {
        response = call_model_stream(iteration, prompt_messages, model_settings, cancellation,
                                     sequenced_on_event, reasoning_id, latest_non_empty_reasoning);
      } else {
        response = call_model(iteration, prompt_messages, model_settings, cancellation);
      }
    }
    if (wants_stream_events && !latest_non_empty_reasoning.empty()) {
      response.reasoning = ModelReasoning{.text = latest_non_empty_reasoning, .format = "summary"};
    } else if (response.reasoning && !response.reasoning->text.empty()) {
      latest_non_empty_reasoning = response.reasoning->text;
    }
    last_response = response;
    has_last_response = true;
    if (!resumed_model_response) {
      trace.push_back(AgentTraceEntry{.type = "model", .iteration = iteration, .response = response});
      checkpoint(AgentLoopDurablePhase::ModelCompleted, iteration);
    }

    auto parsed = config_.parser->parse(response.text, *config_.tool_registry, iteration);
    if (parsed.final_answer) {
      consecutive_parse_errors = 0;
      consecutive_reasoning_protocol_leaks = 0;
      auto final_response = response_with_final_answer(response, parsed.step.final_answer);
      if (wants_stream_events) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::AgentOutput,
            .iteration = iteration,
            .response = final_response,
        });
      }
      auto validation = config_.final_answer_validator->validate(FinalAnswerValidationInput{
          .user_request = input_text,
          .input = input_value,
          .final_answer = parsed.step.final_answer,
          .react_trace = react_trace,
          .completed_tool_results = collect_completed_tool_results(trace),
          .tools = config_.tool_registry->list(),
          .iteration = iteration,
          .runtime_context = runtime_context,
      });
      if (!validation.accepted) {
        auto rejection = parsed.step;
        rejection.type = ReActStepType::FinalRejected;
        rejection.ok = false;
        rejection.error = validation.reason.empty() ? "requested work is not completed." : validation.reason;
        auto observation = config_.observation_renderer->render_final_rejection(rejection.error, iteration);
        rejection.observation = extract_text_content(observation.content);
        react_trace.push_back(rejection);
        session.add(observation);
        if (wants_stream_events) {
          emit_stream_event(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::ReActFinalRejected,
              .iteration = iteration,
              .react_step = rejection,
          });
          emit_stream_event(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::ReActObservation,
              .iteration = iteration,
              .react_step = rejection,
          });
        }
        if (config_.event_bus) {
          Value payload = Value::object({{"iteration", iteration},
                                         {"finalAnswer", parsed.step.final_answer},
                                         {"reason", rejection.error}});
          if (validation.metadata.is_object() && !validation.metadata.as_object().empty()) {
            payload["metadata"] = validation.metadata;
          }
          config_.event_bus->publish("react.final_rejected", ExecutionTarget::Run,
                                     std::move(payload),
                                     child_or_root_trace_context(config_.trace_context, "react.final_rejected"));
        }
        checkpoint(AgentLoopDurablePhase::ToolBatchCompleted, iteration + 1);
        continue;
      }
      if (!parsed.step.visible_message.empty()) {
        if (config_.persist_visible_messages) {
          session.add(visible_message(parsed.step.visible_message, iteration));
        }
        if (wants_stream_events) {
          emit_stream_event(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::ReActMessage,
              .iteration = iteration,
              .react_step = parsed.step,
          });
        }
        if (config_.event_bus) {
          config_.event_bus->publish("react.message", ExecutionTarget::Run,
                                     Value::object({{"iteration", iteration},
                                                    {"message", parsed.step.visible_message},
                                                    {"final", false}}),
                                     child_or_root_trace_context(config_.trace_context, "react.message"));
        }
      }
      last_response = final_response;
      last_assistant_text = parsed.step.final_answer;
      react_trace.push_back(parsed.step);
      session.add(assistant_message_from_output(final_response));
      const auto termination = is_incomplete_finish_reason(response.finish_reason)
                                   ? AgentLoopTerminationReason::IncompleteResponse
                                   : AgentLoopTerminationReason::Completed;
      AgentLoopRunResult result{session.session_id(), iteration + 1, parsed.step.final_answer,
                                final_response, trace, react_trace, session.get_messages(), termination, {},
                                latest_non_empty_reasoning};
      result.usage = aggregate_trace_usage(result.trace);
      checkpoint(AgentLoopDurablePhase::Completed, iteration + 1, termination);
      if (wants_stream_events) {
        emit_reasoning_completed(iteration);
        if (!parsed.step.final_answer.empty()) {
          emit_stream_event(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::UserVisibleDelta,
              .iteration = iteration,
              .provider = final_response.provider,
              .model = final_response.model,
              .delta = parsed.step.final_answer,
              .text = parsed.step.final_answer,
          });
        }
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ReActFinal,
            .iteration = iteration,
            .react_step = parsed.step,
        });
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::Done,
            .iteration = iteration,
            .result = result,
        });
      }
      return result;
    }
    if (wants_stream_events) {
      emit_stream_event(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::AgentOutput,
          .iteration = iteration,
          .response = response,
      });
    }

    if (!parsed.ok || parsed.step.type == ReActStepType::ParseError) {
      if (response_has_reasoning_protocol_leak(response)) {
        consecutive_parse_errors = 0;
        consecutive_reasoning_protocol_leaks += 1;
        if (consecutive_reasoning_protocol_leaks >= 2) {
          throw ProtocolError("provider emitted ReAct protocol in reasoning channel.",
                              "react.reasoning_protocol_leak");
        }
        auto leak = parsed.step;
        leak.type = ReActStepType::ReasoningProtocolLeak;
        leak.iteration = iteration;
        leak.ok = false;
        leak.error = std::string(kReasoningProtocolLeakObservation);
        auto observation = config_.observation_renderer->render_reasoning_protocol_leak(iteration);
        leak.observation = extract_text_content(observation.content);
        react_trace.push_back(leak);
        session.add(observation);
        if (wants_stream_events) {
          emit_stream_event(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::ReActReasoningProtocolLeak,
              .iteration = iteration,
              .react_step = leak,
          });
          emit_stream_event(AgentLoopStreamEvent{
              .type = AgentLoopStreamEventType::ReActObservation,
              .iteration = iteration,
              .react_step = leak,
          });
        }
        if (config_.event_bus) {
          config_.event_bus->publish("react.reasoning_protocol_leak", ExecutionTarget::Run,
                                     Value::object({{"iteration", iteration},
                                                    {"error", leak.error},
                                                    {"provider", response.provider},
                                                    {"model", response.model}}),
                                     child_or_root_trace_context(config_.trace_context,
                                                                 "react.reasoning_protocol_leak"));
        }
        checkpoint(AgentLoopDurablePhase::ToolBatchCompleted, iteration + 1);
        continue;
      }
      consecutive_reasoning_protocol_leaks = 0;
      consecutive_parse_errors += 1;
      auto observation = config_.observation_renderer->render_parse_error(parsed.error, iteration);
      parsed.step.observation = extract_text_content(observation.content);
      parsed.step.ok = false;
      react_trace.push_back(parsed.step);
      if (wants_stream_events) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ReActParseError,
            .iteration = iteration,
            .react_step = parsed.step,
        });
      }
      if (consecutive_parse_errors >= config_.max_parse_errors) {
        auto response_excerpt = response.text;
        constexpr std::size_t max_excerpt = 240;
        if (response_excerpt.size() > max_excerpt) {
          response_excerpt = response_excerpt.substr(0, max_excerpt) + "...";
        }
        throw ProtocolError("ReAct loop exceeded max parse errors (" +
                                std::to_string(config_.max_parse_errors) +
                                "). Last error: " + parsed.error +
                                ". Model: " + config_.model->provider() + "/" +
                                config_.model->model() +
                                ". Last response: " + response_excerpt,
                            "react.parse_errors_exceeded");
      }
      session.add(observation);
      if (wants_stream_events) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ReActObservation,
            .iteration = iteration,
            .react_step = parsed.step,
        });
      }
      checkpoint(AgentLoopDurablePhase::ToolBatchCompleted, iteration + 1);
      continue;
    }

    consecutive_parse_errors = 0;
    consecutive_reasoning_protocol_leaks = 0;
    std::vector<ToolCall> tool_calls;
    tool_calls.reserve(parsed.step.actions.size());
    for (const auto& action : parsed.step.actions) {
      tool_calls.push_back(ToolCall{
          .id = action.id,
          .name = action.tool,
          .arguments = action.input,
      });
    }
    if (!parsed.step.visible_message.empty() && !resumed_action_batch_parsed) {
      if (config_.persist_visible_messages) {
        session.add(visible_message(parsed.step.visible_message, iteration));
      }
      if (wants_stream_events) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ReActMessage,
            .iteration = iteration,
            .react_step = parsed.step,
        });
      }
      if (config_.event_bus) {
        config_.event_bus->publish("react.message", ExecutionTarget::Run,
                                   Value::object({{"iteration", iteration},
                                                  {"message", parsed.step.visible_message},
                                                  {"final", false}}),
                                     child_or_root_trace_context(config_.trace_context, "react.message"));
      }
    }
    if (!resumed_action_batch_parsed) {
      session.add(actions_message(tool_calls));
    }
    if (wants_stream_events) {
      emit_stream_event(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::ReActActionBatch,
          .iteration = iteration,
          .tool_calls = tool_calls,
          .react_step = parsed.step,
      });
      emit_stream_event(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::ToolBatchStart,
          .iteration = iteration,
          .tool_calls = tool_calls,
      });
    }
    if (!resumed_action_batch_parsed) {
      checkpoint(AgentLoopDurablePhase::ActionBatchParsed, iteration);
    }

    std::vector<ToolExecutionResult> tool_results;
    tool_results.reserve(tool_calls.size());
    for (const auto& tool_call : tool_calls) {
      if (wants_stream_events) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ToolStart,
            .iteration = iteration,
            .tool_call = tool_call,
        });
      }
      auto effective_tool_context = build_tool_context(tool_context, iteration, cancellation, config_.trace_context);
      ToolExecutionResult tool_result;
      try {
        tool_result = config_.tool_executor->execute_tool_call(tool_call, std::move(effective_tool_context));
      } catch (const std::exception& error) {
        tool_result = synthetic_tool_failure_result(tool_call,
                                                    synthetic_tool_failure_message(tool_call, error));
        if (cancellation && cancellation->cancelled()) {
          if (wants_stream_events) {
            emit_stream_event(AgentLoopStreamEvent{
                .type = AgentLoopStreamEventType::ToolComplete,
                .iteration = iteration,
                .tool_result = tool_result,
            });
          }
          throw;
        }
      } catch (...) {
        tool_result = synthetic_tool_failure_result(
            tool_call,
            "Tool \"" + tool_call.name + "\" threw an unexpected error.");
      }
      if (wants_stream_events) {
        emit_stream_event(AgentLoopStreamEvent{
            .type = AgentLoopStreamEventType::ToolComplete,
            .iteration = iteration,
            .tool_result = tool_result,
        });
      }
      tool_results.push_back(std::move(tool_result));
    }
    trace.push_back(AgentTraceEntry{.type = "tools", .iteration = iteration, .tool_results = tool_results});
    auto observation = config_.observation_renderer->render_tool_results(tool_results, iteration);
    parsed.step.observation = extract_text_content(observation.content);
    parsed.step.ok = std::all_of(tool_results.begin(), tool_results.end(),
                                 [](const ToolExecutionResult& result) { return result.ok; });
    react_trace.push_back(parsed.step);
    session.add(observation);
    if (wants_stream_events) {
      emit_stream_event(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::ReActObservation,
          .iteration = iteration,
          .tool_results = tool_results,
          .react_step = parsed.step,
      });
      emit_stream_event(AgentLoopStreamEvent{
          .type = AgentLoopStreamEventType::ToolBatchComplete,
          .iteration = iteration,
          .tool_calls = tool_calls,
          .tool_results = tool_results,
      });
    }
    checkpoint(AgentLoopDurablePhase::ToolBatchCompleted, iteration + 1);
  }

  if (!has_last_response) {
    throw ProtocolError("ReAct loop reached the max iteration limit (" +
                            std::to_string(config_.max_iterations) +
                            ") without producing a model response.",
                        "react.max_iterations_exceeded");
  }
  AgentLoopRunResult result{session.session_id(), config_.max_iterations, last_assistant_text,
                            last_response, trace, react_trace, session.get_messages(),
                            AgentLoopTerminationReason::MaxIterations, {},
                            latest_non_empty_reasoning};
  result.usage = aggregate_trace_usage(result.trace);
  checkpoint(AgentLoopDurablePhase::Completed, config_.max_iterations, AgentLoopTerminationReason::MaxIterations);
  if (wants_stream_events) {
    emit_reasoning_completed(config_.max_iterations);
    emit_stream_event(AgentLoopStreamEvent{
        .type = AgentLoopStreamEventType::Done,
        .iteration = config_.max_iterations,
        .result = result,
    });
  }
  return result;
}
