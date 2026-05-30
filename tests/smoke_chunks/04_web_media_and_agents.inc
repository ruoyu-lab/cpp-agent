  {
    agent::InMemoryAutonomousStore waiting_store;
    agent::StaticAutonomousPlanner waiting_planner(std::vector<agent::AutonomousPlanStep>{
        agent::AutonomousPlanStep{.id = "approval", .title = "Approval", .objective = "Wait for approval"},
    });
    int waiting_calls = 0;
    agent::CallbackAutonomousStepExecutor waiting_executor(
        [&](const agent::AutonomousStepExecutionInput&) {
          waiting_calls += 1;
          return agent::AutonomousStepExecutionResult{
              .status = agent::AutonomousStepStatus::Waiting,
              .output = agent::Value::object({{"pending", true}}),
              .wait_reason = "approval required",
          };
        });
    agent::AutonomousRunManager waiting_manager(agent::AutonomousRunManagerConfig{
        .store = &waiting_store,
        .planner = &waiting_planner,
        .executor = &waiting_executor,
    });
    auto waiting_run = waiting_manager.create_run(agent::CreateAutonomousRunInput{.goal = "Needs approval"});
    auto waiting_snapshot = waiting_manager.run(waiting_run.id);
    SMOKE_CHECK(waiting_snapshot.run.status == agent::AutonomousRunStatus::Waiting);
    SMOKE_CHECK(waiting_snapshot.steps.front().status == agent::AutonomousStepStatus::Waiting);
    SMOKE_CHECK(waiting_snapshot.steps.front().wait_reason == "approval required");
    auto completed_waiting = waiting_manager.complete_waiting_step(
        waiting_run.id, "approval", agent::Value::object({{"approved", true}}));
    SMOKE_CHECK(completed_waiting.run.status == agent::AutonomousRunStatus::Queued);
    auto resumed_waiting = waiting_manager.resume(waiting_run.id);
    SMOKE_CHECK(resumed_waiting.run.status == agent::AutonomousRunStatus::Completed);
    SMOKE_CHECK(waiting_calls == 1);
  }
  {
    auto autonomous_model = std::make_shared<AutonomousRunnerModel>();
    agent::AgentRunner autonomous_runner(agent::AgentRunnerConfig{
        .adapter = autonomous_model,
        .contexts = {
            agent::ContextSource{
                .id = "autonomous-runtime",
                .title = "Autonomous Runtime",
                .resolve = [](const agent::Value& runtime) {
                  std::vector<agent::EmbeddedContextBlock> blocks;
                  if (!runtime.contains("autonomousRunId")) {
                    return blocks;
                  }
                  std::string content = "run=" + runtime.at("autonomousRunId").as_string();
                  if (runtime.contains("autonomousStepId")) {
                    content += "\nstep=" + runtime.at("autonomousStepId").as_string();
                  }
                  blocks.push_back(agent::EmbeddedContextBlock{
                      .title = "Autonomous Runtime",
                      .content = content,
                      .priority = 10,
                  });
                  return blocks;
                },
            },
        },
        .max_iterations = 2,
    });
    agent::AgentAutonomousPlanner agent_planner(agent::AgentAutonomousPlannerConfig{
        .runner = &autonomous_runner,
        .context = agent::Value::object({{"tenant", "native"}}),
    });
    agent::AgentRunnerStepExecutor agent_executor(agent::AgentRunnerStepExecutorConfig{
        .runner = &autonomous_runner,
        .context = agent::Value::object({{"tenant", "native"}}),
    });
    agent::InMemoryAutonomousStore agent_store;
    agent::AutonomousRunManager agent_manager(agent::AutonomousRunManagerConfig{
        .store = &agent_store,
        .planner = &agent_planner,
        .executor = &agent_executor,
    });
    auto agent_run = agent_manager.create_run(agent::CreateAutonomousRunInput{
        .id = "agent-auto-runner",
        .goal = "Ship model-led report",
        .input = agent::Value::object({{"topic", "native autonomous"}}),
    });
    auto agent_snapshot = agent_manager.run(agent_run.id);
    SMOKE_CHECK(agent_snapshot.run.status == agent::AutonomousRunStatus::Completed);
    SMOKE_CHECK(agent_snapshot.steps.size() == 2);
    SMOKE_CHECK(agent_snapshot.steps.front().id == "draft");
    SMOKE_CHECK(agent_snapshot.steps.back().depends_on == std::vector<std::string>{"draft"});
    SMOKE_CHECK(agent_snapshot.steps.front().output.at("text").as_string() == "draft-output");
    SMOKE_CHECK(agent_snapshot.steps.back().output.at("text").as_string() == "final-output");
    SMOKE_CHECK(agent_snapshot.steps.back().metadata.at("text").as_string() == "final-output");
    const auto runner_checkpoint = std::find_if(
        agent_snapshot.checkpoints.begin(), agent_snapshot.checkpoints.end(),
        [](const agent::AutonomousCheckpoint& checkpoint) { return checkpoint.name == "runner.state"; });
    SMOKE_CHECK(runner_checkpoint != agent_snapshot.checkpoints.end());
    SMOKE_CHECK(runner_checkpoint->state.at("sessionId").as_string() == "autonomous:agent-auto-runner");
    SMOKE_CHECK(autonomous_model->prompts().size() == 3);
  }

  {
    agent::FileArtifactStore disk_artifacts(agent::FileArtifactStoreConfig{
        .base_dir = persistent_dir / "artifacts",
        .namespace_id = "team:ops/path",
    });
    disk_artifacts.set("answer", agent::Value::object({{"text", "file artifact"}}));
    disk_artifacts.append_log("answer", "checked", agent::Value::object({{"ok", true}}));
    SMOKE_CHECK(disk_artifacts.history("answer").size() == 2);
    SMOKE_CHECK(std::filesystem::exists(persistent_dir / "artifacts" / "team%3Aops%2Fpath.json"));
  }
  {
    agent::FileArtifactStore disk_artifacts(agent::FileArtifactStoreConfig{
        .base_dir = persistent_dir / "artifacts",
        .namespace_id = "team:ops/path",
    });
    SMOKE_CHECK(disk_artifacts.get("answer")->value.at("text").as_string() == "file artifact");
    SMOKE_CHECK(disk_artifacts.history("answer").size() == 2);
    disk_artifacts.clear();
    SMOKE_CHECK(!disk_artifacts.get("answer"));
    SMOKE_CHECK(disk_artifacts.history("answer").size() == 2);
    SMOKE_CHECK(!disk_artifacts.history().empty());
    SMOKE_CHECK(disk_artifacts.history().back().key == "*");
    SMOKE_CHECK(disk_artifacts.history().back().event == "clear-all");
  }

  {
    agent::FileSharedStateStore disk_state(agent::FileSharedStateStoreConfig{
        .base_dir = persistent_dir / "state",
        .namespace_id = "team:ops/path",
    });
    disk_state.merge("worker:a", agent::Value::object({{"count", 1}}));
    disk_state.merge("worker:a", agent::Value::object({{"done", true}}));
    SMOKE_CHECK(std::filesystem::exists(persistent_dir / "state" / "team%3Aops%2Fpath.json"));
    SMOKE_CHECK(agent::read_json_file(persistent_dir / "state" / "team%3Aops%2Fpath.json").is_array());
  }
  {
    agent::FileSharedStateStore disk_state(agent::FileSharedStateStoreConfig{
        .base_dir = persistent_dir / "state",
        .namespace_id = "team:ops/path",
    });
    auto state = disk_state.get("worker:a");
    SMOKE_CHECK(state);
    SMOKE_CHECK(state->value.at("count").as_integer() == 1);
    SMOKE_CHECK(state->value.at("done").as_bool());
  }
  {
    const auto legacy_artifact_path = persistent_dir / "legacy-artifacts" / "team" / "artifacts.json";
    agent::write_json_file(legacy_artifact_path, agent::Value::object({
        {"records", agent::Value::array({
            agent::Value::object({
                {"key", "legacy-answer"},
                {"value", agent::Value::object({{"text", "legacy artifact"}})},
                {"updatedAt", "2026-01-01T00:00:00Z"},
            }),
        })},
        {"history", agent::Value::array({})},
    }));
    agent::FileArtifactStore legacy_artifacts(persistent_dir / "legacy-artifacts", "team");
    SMOKE_CHECK(legacy_artifacts.get("legacy-answer")->value.at("text").as_string() == "legacy artifact");
    const auto legacy_state_path = persistent_dir / "legacy-state" / "team" / "shared-state.json";
    agent::write_json_file(legacy_state_path, agent::Value::object({
        {"records", agent::Value::array({
            agent::Value::object({
                {"key", "legacy-worker"},
                {"value", agent::Value::object({{"done", true}})},
                {"updatedAt", "2026-01-01T00:00:00Z"},
            }),
        })},
    }));
    agent::FileSharedStateStore legacy_state(persistent_dir / "legacy-state", "team");
    SMOKE_CHECK(legacy_state.get("legacy-worker")->value.at("done").as_bool());
  }
  {
    agent::InMemoryArtifactStore concurrent_artifacts;
    agent::InMemorySharedStateStore concurrent_state;
    agent::InMemoryMailbox concurrent_mailbox;
    constexpr int thread_count = 6;
    constexpr int iterations = 20;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto suffix = std::to_string(thread_index) + ":" + std::to_string(iteration);
          concurrent_artifacts.set("artifact:" + suffix,
                                   agent::Value::object({{"thread", thread_index}, {"iteration", iteration}}));
          concurrent_artifacts.append_log("artifact-log", "tick",
                                          agent::Value::object({{"thread", thread_index}, {"iteration", iteration}}));
          concurrent_state.merge("worker:" + std::to_string(thread_index),
                                 agent::Value::object({{"thread", thread_index}, {"last", iteration}}));
          concurrent_mailbox.publish("orchestration.concurrent",
                                     "worker-" + std::to_string(thread_index),
                                     agent::Value::object({{"iteration", iteration}}),
                                     "supervisor",
                                     "progress");
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    const auto artifact_snapshot = concurrent_artifacts.snapshot();
    SMOKE_CHECK(artifact_snapshot.records.size() == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(artifact_snapshot.history.size() == static_cast<std::size_t>(thread_count * iterations * 2));
    const auto state_records = concurrent_state.list();
    SMOKE_CHECK(state_records.size() == static_cast<std::size_t>(thread_count));
    auto inbox_messages = concurrent_mailbox.receive(agent::MailboxReadOptions{
        .channel = "orchestration.concurrent",
    });
    SMOKE_CHECK(inbox_messages.size() == static_cast<std::size_t>(thread_count * iterations));
    std::vector<std::string> message_ids;
    for (const auto& message : inbox_messages) {
      message_ids.push_back(message.id);
    }
    concurrent_mailbox.ack(message_ids);
    SMOKE_CHECK(concurrent_mailbox.receive(agent::MailboxReadOptions{
        .channel = "orchestration.concurrent",
    }).empty());
  }
  {
    const auto concurrent_artifact_dir = persistent_dir / "artifacts-concurrent";
    const auto concurrent_state_dir = persistent_dir / "state-concurrent";
    std::filesystem::remove_all(concurrent_artifact_dir);
    std::filesystem::remove_all(concurrent_state_dir);
    agent::FileArtifactStore concurrent_artifacts(concurrent_artifact_dir, "team");
    agent::FileSharedStateStore concurrent_state(concurrent_state_dir, "team");
    constexpr int thread_count = 5;
    constexpr int iterations = 12;
    std::vector<std::thread> workers;
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
      workers.emplace_back([&, thread_index]() {
        for (int iteration = 0; iteration < iterations; ++iteration) {
          const auto suffix = std::to_string(thread_index) + ":" + std::to_string(iteration);
          concurrent_artifacts.set("artifact:" + suffix,
                                   agent::Value::object({{"thread", thread_index}, {"iteration", iteration}}));
          concurrent_artifacts.append_log("artifact-log", "tick",
                                          agent::Value::object({{"thread", thread_index}, {"iteration", iteration}}));
          concurrent_state.merge("worker:" + std::to_string(thread_index),
                                 agent::Value::object({{"thread", thread_index}, {"last", iteration}}));
        }
      });
    }
    for (auto& worker : workers) {
      worker.join();
    }
    agent::FileArtifactStore reloaded_artifacts(concurrent_artifact_dir, "team");
    const auto artifact_snapshot = reloaded_artifacts.snapshot();
    SMOKE_CHECK(artifact_snapshot.records.size() == static_cast<std::size_t>(thread_count * iterations));
    SMOKE_CHECK(artifact_snapshot.history.size() == static_cast<std::size_t>(thread_count * iterations * 2));
    agent::FileSharedStateStore reloaded_state(concurrent_state_dir, "team");
    const auto state_records = reloaded_state.list();
    SMOKE_CHECK(state_records.size() == static_cast<std::size_t>(thread_count));
    for (const auto& record : state_records) {
      SMOKE_CHECK(record.value.at("last").as_integer() == iterations - 1);
    }
  }

  auto resolved_app = agent::resolve_native_agent_app(agent::Value::object({
      {"defaultAgent", "main"},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"systemPrompt", "Native config smoke."},
              {"maxIterations", 3},
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"core", "local"})}})},
              {"memory", agent::Value::object({
                  {"session", agent::Value::object({
                      {"kind", "file"},
                      {"baseDir", (persistent_dir / "config-sessions").string()},
                  })},
              })},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "config-kb"},
                      {"title", "Config KB"},
                      {"sources", agent::Value::array({
                          agent::Value::object({
                              {"type", "text"},
                              {"title", "Config Knowledge"},
                              {"text", "config input native knowledge retrieval"},
                          }),
                      })},
                  })},
                  {"retrievalOptions", agent::Value::object({
                      {"enabled", true},
                      {"topK", 1},
                      {"minScore", 0.0},
                  })},
              })},
              {"runtime", agent::Value::object({
                  {"modelSettings", agent::Value::object({{"temperature", 0.1}, {"maxOutputTokens", 64}})},
                  {"retrievalOptions", agent::Value::object({{"enabled", false}})},
                  {"planner", agent::Value::object({
                      {"kind", "static"},
                      {"plan", agent::Value::object({
                          {"goal", "Config-driven execution plan"},
                          {"steps", agent::Value::array({
                              agent::Value::object({{"title", "Handle config input"}}),
                          })},
                      })},
                  })},
              })},
          })},
      })},
  }));
  SMOKE_CHECK(resolved_app.agent_id == "main");
  SMOKE_CHECK(!resolved_app.tools.empty());
  auto resolved_run = resolved_app.runner->run("config input", "config-session");
  SMOKE_CHECK(resolved_run.text == "config input");
  SMOKE_CHECK(resolved_run.plan);
  SMOKE_CHECK(resolved_run.plan->goal == "Config-driven execution plan");
  SMOKE_CHECK(resolved_run.plan->steps.front().id == "step_1");
  SMOKE_CHECK(!resolved_run.knowledge_hits.empty());
  SMOKE_CHECK(resolved_run.knowledge_hits.front().citation.title == "Config Knowledge");
  SMOKE_CHECK(std::filesystem::exists(persistent_dir / "config-sessions" / "config-session.json"));

  std::vector<std::string> capability_fallback_calls;
  auto capability_fallback_app = agent::resolve_native_agent_app(agent::Value::object({
      {"defaultAgent", "main"},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({
                  {"provider", "ollama"},
                  {"model", "primary"},
                  {"requireCapabilities", agent::Value::array({"input.file"})},
                  {"fallbacks", agent::Value::array({
                      agent::Value::object({
                          {"provider", "openai"},
                          {"model", "fallback"},
                          {"apiKey", "test-key"},
                      }),
                  })},
              })},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    capability_fallback_calls.push_back(request.provider);
    SMOKE_CHECK(request.provider == "openai");
    SMOKE_CHECK(request.body.at("model").as_string() == "fallback");
    return agent::Value::object({
        {"id", "capability-fallback"},
        {"choices", agent::Value::array({
            agent::Value::object({
                {"message", agent::Value::object({{"content", "capability fallback ok"}})},
                {"finish_reason", "stop"},
            }),
        })},
    });
  });
  auto capability_fallback_result = capability_fallback_app.runner->run("needs file", "config-cap-fallback");
  SMOKE_CHECK(capability_fallback_result.text == "capability fallback ok");
  SMOKE_CHECK((capability_fallback_calls == std::vector<std::string>{"openai"}));

  std::vector<std::string> runtime_fallback_calls;
  auto runtime_fallback_app = agent::resolve_native_agent_app(agent::Value::object({
      {"defaultAgent", "main"},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({
                  {"provider", "openai"},
                  {"model", "primary"},
                  {"apiKey", "test-key"},
                  {"fallbacks", agent::Value::array({
                      agent::Value::object({
                          {"provider", "qwen"},
                          {"model", "fallback"},
                          {"apiKey", "test-key"},
                      }),
                  })},
              })},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    runtime_fallback_calls.push_back(request.provider);
    if (request.provider == "openai") {
      SMOKE_CHECK(request.body.at("model").as_string() == "primary");
      throw std::runtime_error("primary model unavailable");
    }
    SMOKE_CHECK(request.provider == "qwen");
    SMOKE_CHECK(request.body.at("model").as_string() == "fallback");
    return agent::Value::object({
        {"id", "runtime-fallback"},
        {"choices", agent::Value::array({
            agent::Value::object({
                {"message", agent::Value::object({{"content", "runtime fallback ok"}})},
                {"finish_reason", "stop"},
            }),
        })},
    });
  });
  auto runtime_fallback_result = runtime_fallback_app.runner->run("fallback now", "config-runtime-fallback");
  SMOKE_CHECK(runtime_fallback_result.text == "runtime fallback ok");
  SMOKE_CHECK((runtime_fallback_calls == std::vector<std::string>{"openai", "qwen"}));

#ifndef _WIN32
  const char* old_qwen_model = std::getenv("QWEN_MODEL");
  const char* old_qwen_key = std::getenv("QWEN_API_KEY");
  const char* old_qwen_base = std::getenv("QWEN_BASE_URL");
  const char* old_deepseek_model = std::getenv("DEEPSEEK_MODEL");
  const char* old_deepseek_key = std::getenv("DEEPSEEK_API_KEY");
  const char* old_deepseek_base = std::getenv("DEEPSEEK_BASE_URL");
  const bool had_qwen_model = old_qwen_model != nullptr;
  const bool had_qwen_key = old_qwen_key != nullptr;
  const bool had_qwen_base = old_qwen_base != nullptr;
  const bool had_deepseek_model = old_deepseek_model != nullptr;
  const bool had_deepseek_key = old_deepseek_key != nullptr;
  const bool had_deepseek_base = old_deepseek_base != nullptr;
  const std::string old_qwen_model_value = had_qwen_model ? old_qwen_model : "";
  const std::string old_qwen_key_value = had_qwen_key ? old_qwen_key : "";
  const std::string old_qwen_base_value = had_qwen_base ? old_qwen_base : "";
  const std::string old_deepseek_model_value = had_deepseek_model ? old_deepseek_model : "";
  const std::string old_deepseek_key_value = had_deepseek_key ? old_deepseek_key : "";
  const std::string old_deepseek_base_value = had_deepseek_base ? old_deepseek_base : "";
  SMOKE_CHECK(::setenv("QWEN_MODEL", "qwen-env-model", 1) == 0);
  SMOKE_CHECK(::setenv("QWEN_API_KEY", "qwen-env-key", 1) == 0);
  SMOKE_CHECK(::setenv("QWEN_BASE_URL", "https://qwen.env.test/v1/", 1) == 0);
  bool qwen_env_config_called = false;
  auto qwen_env_config_app = agent::resolve_native_agent_app(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "qwen"}})},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    qwen_env_config_called = true;
    SMOKE_CHECK(request.provider == "qwen");
    SMOKE_CHECK(request.base_url == "https://qwen.env.test/v1");
    SMOKE_CHECK(request.headers.at("authorization") == "Bearer qwen-env-key");
    SMOKE_CHECK(request.body.at("model").as_string() == "qwen-env-model");
    return agent::Value::object({
        {"id", "qwen-env-config"},
        {"choices", agent::Value::array({
            agent::Value::object({
                {"message", agent::Value::object({{"content", "qwen env config ok"}})},
                {"finish_reason", "stop"},
            }),
        })},
    });
  });
  auto qwen_env_config_result = qwen_env_config_app.runner->run("qwen env", "qwen-env-config-session");
  SMOKE_CHECK(qwen_env_config_called);
  SMOKE_CHECK(qwen_env_config_result.text == "qwen env config ok");

  SMOKE_CHECK(::setenv("DEEPSEEK_MODEL", "deepseek-env-model", 1) == 0);
  SMOKE_CHECK(::setenv("DEEPSEEK_API_KEY", "deepseek-env-key", 1) == 0);
  SMOKE_CHECK(::setenv("DEEPSEEK_BASE_URL", "https://deepseek.env.test/v1/", 1) == 0);
  bool deepseek_env_config_called = false;
  auto deepseek_env_config_app = agent::resolve_native_agent_app(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "deepseek"}})},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    deepseek_env_config_called = true;
    SMOKE_CHECK(request.provider == "deepseek");
    SMOKE_CHECK(request.base_url == "https://deepseek.env.test/v1");
    SMOKE_CHECK(request.headers.at("x-api-key") == "deepseek-env-key");
    SMOKE_CHECK(request.body.at("model").as_string() == "deepseek-env-model");
    return agent::Value::object({
        {"id", "deepseek-env-config"},
        {"model", "deepseek-env-model"},
        {"stop_reason", "end_turn"},
        {"content", agent::Value::array({
            agent::Value::object({{"type", "text"}, {"text", "deepseek env config ok"}}),
        })},
    });
  });
  auto deepseek_env_config_result =
      deepseek_env_config_app.runner->run("deepseek env", "deepseek-env-config-session");
  SMOKE_CHECK(deepseek_env_config_called);
  SMOKE_CHECK(deepseek_env_config_result.text == "deepseek env config ok");
  if (had_qwen_model) {
    SMOKE_CHECK(::setenv("QWEN_MODEL", old_qwen_model_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("QWEN_MODEL") == 0);
  }
  if (had_qwen_key) {
    SMOKE_CHECK(::setenv("QWEN_API_KEY", old_qwen_key_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("QWEN_API_KEY") == 0);
  }
  if (had_qwen_base) {
    SMOKE_CHECK(::setenv("QWEN_BASE_URL", old_qwen_base_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("QWEN_BASE_URL") == 0);
  }
  if (had_deepseek_model) {
    SMOKE_CHECK(::setenv("DEEPSEEK_MODEL", old_deepseek_model_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("DEEPSEEK_MODEL") == 0);
  }
  if (had_deepseek_key) {
    SMOKE_CHECK(::setenv("DEEPSEEK_API_KEY", old_deepseek_key_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("DEEPSEEK_API_KEY") == 0);
  }
  if (had_deepseek_base) {
    SMOKE_CHECK(::setenv("DEEPSEEK_BASE_URL", old_deepseek_base_value.c_str(), 1) == 0);
  } else {
    SMOKE_CHECK(::unsetenv("DEEPSEEK_BASE_URL") == 0);
  }
#endif

#ifndef _WIN32
  const auto snapshot_env = [](const char* key) {
    const char* value = std::getenv(key);
    return std::pair<bool, std::string>{value != nullptr, value != nullptr ? std::string(value) : std::string{}};
  };
  const auto restore_env = [](const char* key, const std::pair<bool, std::string>& snapshot) {
    if (snapshot.first) {
      SMOKE_CHECK(::setenv(key, snapshot.second.c_str(), 1) == 0);
    } else {
      SMOKE_CHECK(::unsetenv(key) == 0);
    }
  };
  const auto old_openai_key = snapshot_env("OPENAI_API_KEY");
  const auto old_openai_base = snapshot_env("OPENAI_BASE_URL");
  const auto old_openai_org = snapshot_env("OPENAI_ORG_ID");
  const auto old_anthropic_key = snapshot_env("ANTHROPIC_API_KEY");
  const auto old_anthropic_base = snapshot_env("ANTHROPIC_BASE_URL");
  const auto old_gemini_key = snapshot_env("GEMINI_API_KEY");
  const auto old_gemini_base = snapshot_env("GEMINI_BASE_URL");
  const auto old_ollama_base = snapshot_env("OLLAMA_BASE_URL");

  SMOKE_CHECK(::setenv("OPENAI_API_KEY", "openai-env-key", 1) == 0);
  SMOKE_CHECK(::setenv("OPENAI_BASE_URL", "https://openai.env.test/v1/", 1) == 0);
  SMOKE_CHECK(::setenv("OPENAI_ORG_ID", "openai-env-org", 1) == 0);
  bool openai_env_config_called = false;
  auto openai_env_config_app = agent::resolve_native_agent_app(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "openai"}, {"model", "gpt-env"}})},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    openai_env_config_called = true;
    SMOKE_CHECK(request.provider == "openai");
    SMOKE_CHECK(request.endpoint == "/chat/completions");
    SMOKE_CHECK(request.base_url == "https://openai.env.test/v1");
    SMOKE_CHECK(request.headers.at("authorization") == "Bearer openai-env-key");
    SMOKE_CHECK(request.headers.at("openai-organization") == "openai-env-org");
    SMOKE_CHECK(request.body.at("model").as_string() == "gpt-env");
    return agent::Value::object({
        {"id", "openai-env-config"},
        {"choices", agent::Value::array({
            agent::Value::object({
                {"message", agent::Value::object({{"content", "openai env config ok"}})},
                {"finish_reason", "stop"},
            }),
        })},
    });
  });
  auto openai_env_config_result =
      openai_env_config_app.runner->run("openai env", "openai-env-config-session");
  SMOKE_CHECK(openai_env_config_called);
  SMOKE_CHECK(openai_env_config_result.text == "openai env config ok");

  SMOKE_CHECK(::setenv("ANTHROPIC_API_KEY", "anthropic-env-key", 1) == 0);
  SMOKE_CHECK(::setenv("ANTHROPIC_BASE_URL", "https://anthropic.env.test/v1/", 1) == 0);
  bool anthropic_env_config_called = false;
  auto anthropic_env_config_app = agent::resolve_native_agent_app(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "anthropic"}, {"model", "claude-env"}})},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    anthropic_env_config_called = true;
    SMOKE_CHECK(request.provider == "anthropic");
    SMOKE_CHECK(request.base_url == "https://anthropic.env.test/v1");
    SMOKE_CHECK(request.headers.at("x-api-key") == "anthropic-env-key");
    SMOKE_CHECK(request.body.at("model").as_string() == "claude-env");
    return agent::Value::object({
        {"id", "anthropic-env-config"},
        {"model", "claude-env"},
        {"stop_reason", "end_turn"},
        {"content", agent::Value::array({
            agent::Value::object({{"type", "text"}, {"text", "anthropic env config ok"}}),
        })},
    });
  });
  auto anthropic_env_config_result =
      anthropic_env_config_app.runner->run("anthropic env", "anthropic-env-config-session");
  SMOKE_CHECK(anthropic_env_config_called);
  SMOKE_CHECK(anthropic_env_config_result.text == "anthropic env config ok");

  SMOKE_CHECK(::setenv("GEMINI_API_KEY", "gemini-env-key", 1) == 0);
  SMOKE_CHECK(::setenv("GEMINI_BASE_URL", "https://gemini.env.test/v1beta/", 1) == 0);
  bool gemini_env_config_called = false;
  auto gemini_env_config_app = agent::resolve_native_agent_app(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "gemini"}, {"model", "gemini-env"}})},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    gemini_env_config_called = true;
    SMOKE_CHECK(request.provider == "gemini");
    SMOKE_CHECK(request.base_url == "https://gemini.env.test/v1beta");
    SMOKE_CHECK(request.headers.at("x-goog-api-key") == "gemini-env-key");
    SMOKE_CHECK(request.endpoint == "/models/gemini-env:generateContent");
    return agent::Value::object({
        {"responseId", "gemini-env-config"},
        {"modelVersion", "gemini-env"},
        {"candidates", agent::Value::array({
            agent::Value::object({
                {"finishReason", "STOP"},
                {"content", agent::Value::object({
                    {"parts", agent::Value::array({
                        agent::Value::object({{"text", "gemini env config ok"}}),
                    })},
                })},
            }),
        })},
    });
  });
  auto gemini_env_config_result =
      gemini_env_config_app.runner->run("gemini env", "gemini-env-config-session");
  SMOKE_CHECK(gemini_env_config_called);
  SMOKE_CHECK(gemini_env_config_result.text == "gemini env config ok");

  SMOKE_CHECK(::setenv("OLLAMA_BASE_URL", "http://ollama.env.test/", 1) == 0);
  bool ollama_env_config_called = false;
  auto ollama_env_config_app = agent::resolve_native_agent_app(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "ollama"}, {"model", "ollama-env"}})},
          })},
      })},
  }), "main", [&](const agent::NativeProviderRequest& request) {
    ollama_env_config_called = true;
    SMOKE_CHECK(request.provider == "ollama");
    SMOKE_CHECK(request.base_url == "http://ollama.env.test/api");
    SMOKE_CHECK(request.endpoint == "/chat");
    SMOKE_CHECK(request.body.at("model").as_string() == "ollama-env");
    return agent::Value::object({
        {"model", "ollama-env"},
        {"created_at", "ollama-env-config"},
        {"done", true},
        {"done_reason", "stop"},
        {"message", agent::Value::object({{"content", "ollama env config ok"}})},
    });
  });
  auto ollama_env_config_result =
      ollama_env_config_app.runner->run("ollama env", "ollama-env-config-session");
  SMOKE_CHECK(ollama_env_config_called);
  SMOKE_CHECK(ollama_env_config_result.text == "ollama env config ok");

  restore_env("OPENAI_API_KEY", old_openai_key);
  restore_env("OPENAI_BASE_URL", old_openai_base);
  restore_env("OPENAI_ORG_ID", old_openai_org);
  restore_env("ANTHROPIC_API_KEY", old_anthropic_key);
  restore_env("ANTHROPIC_BASE_URL", old_anthropic_base);
  restore_env("GEMINI_API_KEY", old_gemini_key);
  restore_env("GEMINI_BASE_URL", old_gemini_base);
  restore_env("OLLAMA_BASE_URL", old_ollama_base);
#endif

  auto config_with_env_refs = agent::Value::object({
      {"resources", agent::Value::object({
          {"knowledgeBases", agent::Value::object({
              {"docs", agent::Value::object({
                  {"id", "docs"},
                  {"embedder", agent::Value::object({
                      {"provider", "openai"},
                      {"apiKeyEnv", "KB_OPENAI_KEY"},
                      {"baseUrlEnv", "KB_OPENAI_BASE"},
                      {"organizationEnv", "KB_ORG"},
                  })},
              })},
              {"unused", agent::Value::object({
                  {"id", "unused"},
                  {"embedder", agent::Value::object({
                      {"provider", "openai"},
                      {"apiKeyEnv", "UNUSED_ENV"},
                  })},
              })},
          })},
          {"knowledgeManagers", agent::Value::object({
              {"manager", agent::Value::object({
                  {"embedder", agent::Value::object({
                      {"provider", "gemini"},
                      {"apiKeyEnv", "MANAGER_GEMINI_KEY"},
                  })},
                  {"bases", agent::Value::array({
                      agent::Value::object({
                          {"id", "managed"},
                          {"embedder", agent::Value::object({
                              {"provider", "qwen"},
                              {"apiKeyEnv", "MANAGER_BASE_QWEN_KEY"},
                              {"baseUrlEnv", "MANAGER_BASE_QWEN_BASE"},
                          })},
                      }),
                      agent::Value::object({
                          {"id", "managed-llama"},
                          {"embedder", agent::Value::object({
                              {"provider", "llamacpp-native"},
                              {"modelPathEnv", "LLAMA_EMBED_MODEL"},
                              {"libraryDirEnv", "LLAMA_EMBED_LIBRARY_DIR"},
                          })},
                      }),
                  })},
              })},
          })},
      })},
      {"agents", agent::Value::object({
          {"primary", agent::Value::object({
              {"model", agent::Value::object({
                  {"provider", "openai"},
                  {"model", "gpt-env"},
                  {"apiKeyEnv", "OPENAI_KEY"},
                  {"baseUrlEnv", "OPENAI_BASE"},
                  {"organizationEnv", "OPENAI_ORG"},
                  {"fallbacks", agent::Value::array({
                      agent::Value::object({
                          {"provider", "ollama"},
                          {"model", "llama-env"},
                          {"baseUrlEnv", "OLLAMA_BASE"},
                      }),
                      agent::Value::object({
                          {"provider", "deepseek"},
                          {"model", "deepseek-env"},
                          {"apiKeyEnv", "DEEPSEEK_KEY"},
                      }),
                      agent::Value::object({
                          {"provider", "llamacpp-native"},
                          {"modelPathEnv", "LLAMA_MODEL"},
                          {"libraryPathEnv", "LLAMA_LIBRARY"},
                          {"libraryDirEnv", "LLAMA_LIBRARY_DIR"},
                          {"mmprojPathEnv", "LLAMA_MMPROJ"},
                          {"mtmdLibraryDirEnv", "LLAMA_MTMD_DIR"},
                      }),
                  })},
              })},
              {"web", agent::Value::object({
                  {"search", agent::Value::array({
                      agent::Value::object({
                          {"kind", "brave"},
                          {"apiKeyEnv", "BRAVE_KEY"},
                          {"baseUrlEnv", "BRAVE_BASE"},
                      }),
                  })},
              })},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({{"use", "docs"}})},
              })},
              {"runtime", agent::Value::object({
                  {"planner", agent::Value::object({
                      {"kind", "model"},
                      {"model", agent::Value::object({
                          {"provider", "anthropic"},
                          {"model", "planner-env"},
                          {"apiKeyEnv", "PLANNER_ANTHROPIC_KEY"},
                          {"baseUrlEnv", "PLANNER_ANTHROPIC_BASE"},
                      })},
                  })},
              })},
          })},
          {"secondary", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"web", agent::Value::object({
                  {"search", agent::Value::array({
                      agent::Value::object({
                          {"kind", "searxng"},
                          {"baseUrlEnv", "SEARXNG_BASE"},
                      }),
                  })},
              })},
              {"knowledge", agent::Value::object({
                  {"manager", agent::Value::object({{"use", "manager"}})},
              })},
          })},
      })},
  });
  const std::vector<std::string> expected_env_keys{
      "BRAVE_BASE",
      "BRAVE_KEY",
      "DEEPSEEK_KEY",
      "KB_OPENAI_BASE",
      "KB_OPENAI_KEY",
      "KB_ORG",
      "LLAMA_EMBED_LIBRARY_DIR",
      "LLAMA_EMBED_MODEL",
      "LLAMA_LIBRARY",
      "LLAMA_LIBRARY_DIR",
      "LLAMA_MMPROJ",
      "LLAMA_MODEL",
      "LLAMA_MTMD_DIR",
      "MANAGER_BASE_QWEN_BASE",
      "MANAGER_BASE_QWEN_KEY",
      "MANAGER_GEMINI_KEY",
      "OLLAMA_BASE",
      "OPENAI_BASE",
      "OPENAI_KEY",
      "OPENAI_ORG",
      "PLANNER_ANTHROPIC_BASE",
      "PLANNER_ANTHROPIC_KEY",
      "SEARXNG_BASE",
  };
  auto env_keys = agent::collect_referenced_env_keys(config_with_env_refs);
  SMOKE_CHECK(env_keys == expected_env_keys);
  SMOKE_CHECK(std::find(env_keys.begin(), env_keys.end(), "UNUSED_ENV") == env_keys.end());
  auto loaded_env_config = agent::define_native_loaded_agent_config(config_with_env_refs, persistent_dir);
  SMOKE_CHECK(agent::collect_referenced_env_keys(loaded_env_config) == expected_env_keys);
  std::map<std::string, std::string> env_values;
  for (const auto& key : env_keys) {
    env_values[key] = "value";
  }
  SMOKE_CHECK(agent::assert_referenced_env_keys(config_with_env_refs, env_values) == expected_env_keys);
  SMOKE_CHECK(agent::assert_referenced_env_keys(loaded_env_config, env_values) == expected_env_keys);
  env_values.erase("OPENAI_KEY");
  env_values["SEARXNG_BASE"] = "";
  bool missing_env_failed = false;
  try {
    (void)agent::assert_referenced_env_keys(config_with_env_refs, env_values);
  } catch (const agent::ConfigurationError& error) {
    const std::string message = error.what();
    missing_env_failed = message.find("OPENAI_KEY") != std::string::npos &&
                         message.find("SEARXNG_BASE") != std::string::npos;
  }
  SMOKE_CHECK(missing_env_failed);
  bool loaded_missing_env_failed = false;
  try {
    (void)agent::assert_referenced_env_keys(loaded_env_config, env_values);
  } catch (const agent::ConfigurationError& error) {
    loaded_missing_env_failed = std::string(error.what()).find("OPENAI_KEY") != std::string::npos;
  }
  SMOKE_CHECK(loaded_missing_env_failed);

  const auto config_file = persistent_dir / "agent-native.json";
  agent::write_json_file(config_file, agent::Value::object({
      {"agents", agent::Value::object({
          {"loaded", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"memory", agent::Value::object({
                  {"session", agent::Value::object({{"kind", "file"}, {"baseDir", "loaded-sessions"}})},
              })},
          })},
      })},
  }));
  auto loaded_app = agent::load_native_agent_app(config_file, "loaded");
  auto loaded_run = loaded_app.runner->run("loaded input", "loaded-session");
  SMOKE_CHECK(loaded_run.text == "loaded input");
  SMOKE_CHECK(std::filesystem::exists(persistent_dir / "loaded-sessions" / "loaded-session.json"));
  auto loaded_config_object = agent::load_native_loaded_agent_config(config_file);
  SMOKE_CHECK(loaded_config_object.cwd == persistent_dir);
  SMOKE_CHECK(loaded_config_object.path == config_file);
  auto loaded_config_app = agent::resolve_native_agent_app(loaded_config_object, "loaded");
  auto loaded_config_run = loaded_config_app.runner->run("loaded config object input", "loaded-config-session");
  SMOKE_CHECK(loaded_config_run.text == "loaded config object input");
  SMOKE_CHECK(std::filesystem::exists(persistent_dir / "loaded-sessions" / "loaded-config-session.json"));

  const auto object_config_root = persistent_dir / "object-config-root";
  std::filesystem::create_directories(object_config_root);
  auto object_config = agent::define_native_agent_config(agent::Value::object({
      {"agents", agent::Value::object({
          {"object", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"memory", agent::Value::object({
                  {"session", agent::Value::object({{"kind", "file"}, {"baseDir", "object-sessions"}})},
              })},
          })},
      })},
  }));
  auto object_loaded_config = agent::define_native_loaded_agent_config(object_config, object_config_root);
  SMOKE_CHECK(object_loaded_config.cwd == object_config_root);
  SMOKE_CHECK(object_loaded_config.config.at("_cwd").as_string() == object_config_root.string());
  agent::validate_native_agent_config(object_loaded_config);
  SMOKE_CHECK(agent::resolve_agent_id(object_loaded_config) == "object");
  SMOKE_CHECK(agent::resolve_agent_definition(object_loaded_config).at("model").at("provider").as_string() == "echo");
  auto object_app = agent::resolve_native_agent_app(object_loaded_config, "object");
  auto object_run = object_app.runner->run("object config input", "object-session");
  SMOKE_CHECK(object_run.text == "object config input");
  SMOKE_CHECK(std::filesystem::exists(object_config_root / "object-sessions" / "object-session.json"));
  auto default_loaded_config = agent::define_native_loaded_agent_config(agent::Value::object({
      {"defaultAgent", "secondary"},
      {"agents", agent::Value::object({
          {"primary", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
          })},
          {"secondary", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"systemPrompt", "secondary prompt"},
          })},
      })},
  }), object_config_root);
  SMOKE_CHECK(agent::resolve_agent_id(default_loaded_config) == "secondary");
  SMOKE_CHECK(agent::resolve_agent_definition(default_loaded_config).at("systemPrompt").as_string() ==
         "secondary prompt");
  SMOKE_CHECK(agent::resolve_agent_definition(default_loaded_config, "primary")
             .at("model")
             .at("provider")
             .as_string() == "echo");

  const auto discovered_config_root = persistent_dir / "discover-config";
  const auto discovered_nested_dir = discovered_config_root / "nested" / "child";
  std::filesystem::create_directories(discovered_nested_dir);
  const auto discovered_config_file = discovered_config_root / "node-agent.config.json";
  agent::write_json_file(discovered_config_file, agent::Value::object({
      {"agents", agent::Value::object({
          {"discovered", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"memory", agent::Value::object({
                  {"session", agent::Value::object({{"kind", "file"}, {"baseDir", "discovered-sessions"}})},
              })},
          })},
      })},
  }));
  auto discovered_config = agent::find_native_agent_config_file(discovered_nested_dir);
  SMOKE_CHECK(discovered_config);
  SMOKE_CHECK(discovered_config->filename().string() == "node-agent.config.json");
  auto discovered_app = agent::load_native_agent_app(discovered_nested_dir, "discovered");
  auto discovered_run = discovered_app.runner->run("discovered input", "discovered-session");
  SMOKE_CHECK(discovered_run.text == "discovered input");
  SMOKE_CHECK(std::filesystem::exists(discovered_config_root / "discovered-sessions" / "discovered-session.json"));
  const auto priority_config_root = persistent_dir / "discover-config-priority";
  const auto priority_nested_dir = priority_config_root / "nested";
  std::filesystem::create_directories(priority_nested_dir);
  {
    std::ofstream module_config(priority_config_root / "node-agent.config.mjs");
    module_config << "export default { agents: {} };\n";
  }
  agent::write_json_file(priority_config_root / "node-agent.config.json", agent::Value::object({
      {"agents", agent::Value::object({
          {"json", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
          })},
      })},
  }));
  auto priority_config = agent::find_native_agent_config_file(priority_nested_dir);
  SMOKE_CHECK(priority_config);
  SMOKE_CHECK(priority_config->filename().string() == "node-agent.config.mjs");
  bool module_config_failed = false;
  try {
    (void)agent::load_native_agent_config(priority_nested_dir);
  } catch (const agent::ConfigurationError& error) {
    const std::string message = error.what();
    module_config_failed = message.find("NativeConfigModuleLoader") != std::string::npos &&
                           message.find("node-agent.config.mjs") != std::string::npos;
  }
  SMOKE_CHECK(module_config_failed);
  bool module_loader_called = false;
  auto module_loader = [&](const std::filesystem::path& path) {
    module_loader_called = true;
    SMOKE_CHECK(path.filename().string() == "node-agent.config.mjs");
    return agent::Value::object({
        {"agents", agent::Value::object({
            {"module", agent::Value::object({
                {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                {"memory", agent::Value::object({
                    {"session", agent::Value::object({{"kind", "file"}, {"baseDir", "module-sessions"}})},
                })},
            })},
        })},
    });
  };
  auto loaded_module_config = agent::load_native_agent_config(priority_nested_dir, module_loader);
  SMOKE_CHECK(module_loader_called);
  SMOKE_CHECK(loaded_module_config.at("_cwd").as_string() == priority_config_root.string());
  auto module_app = agent::load_native_agent_app(priority_nested_dir, "module", module_loader);
  auto module_run = module_app.runner->run("module config input", "module-session");
  SMOKE_CHECK(module_run.text == "module config input");
  SMOKE_CHECK(std::filesystem::exists(priority_config_root / "module-sessions" / "module-session.json"));
  bool module_non_object_failed = false;
  try {
    (void)agent::load_native_agent_config(priority_nested_dir, [](const std::filesystem::path&) {
      return agent::Value::array({});
    });
  } catch (const agent::ConfigurationError& error) {
    module_non_object_failed = std::string(error.what()).find("must export an object") != std::string::npos;
  }
  SMOKE_CHECK(module_non_object_failed);
  SMOKE_CHECK(!agent::find_native_agent_config_file(persistent_dir / "missing-config-root"));
  {
    std::ostringstream cli_stdout;
    std::ostringstream cli_stderr;
    auto cli_help = agent::run_native_agent_cli({"--help"}, cli_stdout, cli_stderr);
    SMOKE_CHECK(cli_help.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("native-agent") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("--provider") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("--source") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("--no-stream") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("REPL commands") != std::string::npos);
    SMOKE_CHECK(cli_stderr.str().empty());

    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto manual_cli_chat = agent::run_native_agent_cli(
        {"chat", "--provider", "echo", "--model", "echo", "--session", "manual-cli", "manual input"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(manual_cli_chat.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("manual input") != std::string::npos);
    SMOKE_CHECK(cli_stderr.str().empty());

    const auto manual_sessions_dir = persistent_dir / "manual-cli-sessions";
    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto manual_cli_persisted = agent::run_native_agent_cli(
        {"chat", "--provider", "echo", "--model", "echo", "--sessions-dir", manual_sessions_dir.string(),
         "--session", "manual-persisted", "--no-stream", "manual persisted input"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(manual_cli_persisted.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("manual persisted input") != std::string::npos);
    SMOKE_CHECK(std::filesystem::exists(manual_sessions_dir / "manual-persisted.json"));

    const auto manual_source_path = persistent_dir / "manual-source.txt";
    {
      std::ofstream manual_source(manual_source_path);
      manual_source << "manual source retrieval phrase";
    }
    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto manual_cli_source = agent::run_native_agent_cli(
        {"chat", "--provider", "echo", "--model", "echo", "--source", manual_source_path.string(),
         "--top-k", "1", "--min-score", "0", "--debug", "--no-stream", "manual source retrieval phrase"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(manual_cli_source.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("manual source retrieval phrase") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("Knowledge Debug") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("knowledge hits") != std::string::npos);

    const auto manual_skill_project = persistent_dir / "manual-skill-project";
    const auto manual_skill_name = "manual-" + agent::generate_uuid();
    const auto manual_skill_dir = manual_skill_project / ".claude" / "skills" / manual_skill_name;
    std::filesystem::create_directories(manual_skill_dir);
    {
      std::ofstream skill_file(manual_skill_dir / "SKILL.md");
      skill_file << "---\n"
                 << "name: " << manual_skill_name << "\n"
                 << "description: Manual CLI skill\n"
                 << "user-invocable: true\n"
                 << "---\n"
                 << "Manual CLI skill prompt for $ARGUMENTS.";
    }
    const auto original_cli_cwd = std::filesystem::current_path();
    std::filesystem::current_path(manual_skill_project);
    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto manual_cli_skill = agent::run_native_agent_cli(
        {"chat", "--provider", "echo", "--model", "echo", "--no-stream",
         "/" + manual_skill_name, "manual skill payload"},
        cli_stdout,
        cli_stderr);
    std::filesystem::current_path(original_cli_cwd);
    SMOKE_CHECK(manual_cli_skill.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("manual skill payload") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("/" + manual_skill_name) == std::string::npos);

    cli_stdout.str("");
    cli_stdout.clear();
    auto cli_validate = agent::run_native_agent_cli(
        {"validate", "--config", config_file.string(), "--agent", "loaded"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_validate.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("Config OK: loaded") != std::string::npos);

    cli_stdout.str("");
    cli_stdout.clear();
    auto cli_chat = agent::run_native_agent_cli(
        {"chat", "--config", config_file.string(), "--agent", "loaded", "--session", "cli-session", "cli input"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_chat.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("cli input") != std::string::npos);
    SMOKE_CHECK(std::filesystem::exists(persistent_dir / "loaded-sessions" / "cli-session.json"));

    cli_stdout.str("");
    cli_stdout.clear();
    auto cli_chat_no_stream = agent::run_native_agent_cli(
        {"chat", "--config", config_file.string(), "--agent", "loaded", "--session", "cli-session-no-stream",
         "--no-stream", "cli no stream input"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_chat_no_stream.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("cli no stream input") != std::string::npos);

    cli_stdout.str("");
    cli_stdout.clear();
    std::istringstream cli_repl_input("first repl input\n:debug on\nsecond repl input\n:clear\nthird repl input\n:quit\n");
    auto cli_repl = agent::run_native_agent_cli(
        {"chat", "--config", config_file.string(), "--agent", "loaded", "--session", "cli-repl"},
        cli_repl_input,
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_repl.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("> ") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("first repl input") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("Debug on.") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("No retrieval debug info.") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("Cleared session cli-repl.") != std::string::npos);
    SMOKE_CHECK(cli_stdout.str().find("third repl input") != std::string::npos);
    auto repl_snapshot = agent::read_json_file(persistent_dir / "loaded-sessions" / "cli-repl.json");
    SMOKE_CHECK(repl_snapshot.at("messages").as_array().size() == 2);

    const auto cli_suite_file = persistent_dir / "cli-suite.json";
    const auto cli_report_file = persistent_dir / "cli-report.json";
    const auto cli_markdown_file = persistent_dir / "cli-report.md";
    agent::write_json_file(cli_suite_file, agent::Value::object({
        {"agent", "loaded"},
        {"cases", agent::Value::array({
            agent::Value::object({
                {"id", "cli-eval-1"},
                {"input", "cli eval input"},
                {"expectContains", agent::Value::array({"cli eval input"})},
            }),
        })},
    }));
    cli_stdout.str("");
    cli_stdout.clear();
    auto cli_eval = agent::run_native_agent_cli(
        {"eval", "--config", config_file.string(), "--suite", cli_suite_file.string(),
         "--report", cli_report_file.string(), "--markdown-report", cli_markdown_file.string()},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_eval.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("Eval: 1/1 passed") != std::string::npos);
    SMOKE_CHECK(std::filesystem::exists(cli_report_file));
    SMOKE_CHECK(std::filesystem::exists(cli_markdown_file));

    module_loader_called = false;
    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto cli_module_validate = agent::run_native_agent_cli(
        {"validate", "--config", priority_nested_dir.string(), "--agent", "module"},
        cli_stdout,
        cli_stderr,
        module_loader);
    SMOKE_CHECK(cli_module_validate.exit_code == 0);
    SMOKE_CHECK(module_loader_called);
    SMOKE_CHECK(cli_stdout.str().find("Config OK: module") != std::string::npos);
    SMOKE_CHECK(cli_stderr.str().empty());

    module_loader_called = false;
    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto cli_module_chat = agent::run_native_agent_cli(
        {"chat", "--config", priority_nested_dir.string(), "--agent", "module", "--session", "module-cli-session",
         "--no-stream", "module cli input"},
        cli_stdout,
        cli_stderr,
        module_loader);
    SMOKE_CHECK(cli_module_chat.exit_code == 0);
    SMOKE_CHECK(module_loader_called);
    SMOKE_CHECK(cli_stdout.str().find("module cli input") != std::string::npos);
    SMOKE_CHECK(std::filesystem::exists(priority_config_root / "module-sessions" / "module-cli-session.json"));

    const auto cli_module_suite_file = persistent_dir / "cli-module-suite.json";
    const auto cli_module_report_file = persistent_dir / "cli-module-report.json";
    agent::write_json_file(cli_module_suite_file, agent::Value::object({
        {"agent", "module"},
        {"cases", agent::Value::array({
            agent::Value::object({
                {"id", "cli-module-eval-1"},
                {"input", "module cli eval input"},
                {"expectContains", agent::Value::array({"module cli eval input"})},
            }),
        })},
    }));
    module_loader_called = false;
    cli_stdout.str("");
    cli_stdout.clear();
    cli_stderr.str("");
    cli_stderr.clear();
    auto cli_module_eval = agent::run_native_agent_cli(
        {"eval", "--config", priority_nested_dir.string(), "--suite", cli_module_suite_file.string(),
         "--report", cli_module_report_file.string()},
        cli_stdout,
        cli_stderr,
        module_loader);
    SMOKE_CHECK(cli_module_eval.exit_code == 0);
    SMOKE_CHECK(module_loader_called);
    SMOKE_CHECK(cli_stdout.str().find("Eval: 1/1 passed") != std::string::npos);
    SMOKE_CHECK(std::filesystem::exists(cli_module_report_file));

    const auto cli_replay_dir = persistent_dir / "cli-replays";
    auto cli_replay_manifest = agent::write_run_replay(
        cli_replay_dir,
        "cli",
        agent::Value::object({{"input", "cli replay"}}),
        agent::Value::object({{"text", "cli replay output"}}),
        {agent::Value::object({{"type", "done"}})},
        "cli-run-1",
        "loaded");
    const auto cli_run_dir = cli_replay_dir / "cli" / cli_replay_manifest.run_id;
    cli_stdout.str("");
    cli_stdout.clear();
    auto cli_replay_list = agent::run_native_agent_cli(
        {"replay", "list", "--replay-dir", cli_replay_dir.string(), "--session", "cli"},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_replay_list.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("cli-run-1") != std::string::npos);
    cli_stdout.str("");
    cli_stdout.clear();
    auto cli_replay_show = agent::run_native_agent_cli(
        {"replay", "show", "--run", cli_run_dir.string()},
        cli_stdout,
        cli_stderr);
    SMOKE_CHECK(cli_replay_show.exit_code == 0);
    SMOKE_CHECK(cli_stdout.str().find("Run: cli-run-1") != std::string::npos);
  }

  agent::validate_native_agent_config(agent::Value::object({
      {"agents", agent::Value::object({
          {"valid", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"core", "web", "browser"})}})},
          })},
      })},
  }));
  bool invalid_model_provider_failed = false;
  try {
    agent::validate_native_agent_config(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({{"provider", "unknown"}, {"model", "x"}})},
            })},
        })},
    }));
  } catch (const agent::ConfigurationError& error) {
    invalid_model_provider_failed = std::string(error.what()).find("provider must be one of") != std::string::npos;
  }
  SMOKE_CHECK(invalid_model_provider_failed);
  bool unsupported_bundle_failed = false;
  try {
    agent::validate_native_agent_config(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                {"tools", agent::Value::object({{"bundles", agent::Value::array({"missing-bundle"})}})},
            })},
        })},
    }));
  } catch (const agent::ConfigurationError& error) {
    unsupported_bundle_failed = std::string(error.what()).find("unsupported bundle") != std::string::npos;
  }
  SMOKE_CHECK(unsupported_bundle_failed);
  bool missing_resource_ref_failed = false;
  try {
    agent::validate_native_agent_config(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                {"knowledge", agent::Value::object({
                    {"base", agent::Value::object({{"use", "missing"}})},
                })},
            })},
        })},
    }));
  } catch (const agent::ConfigurationError& error) {
    missing_resource_ref_failed = std::string(error.what()).find("Unknown knowledgeBase resource") != std::string::npos;
  }
  SMOKE_CHECK(missing_resource_ref_failed);
  const auto expect_config_validation_error = [](const agent::Value& config, const std::string& expected) {
    bool failed = false;
    try {
      agent::validate_native_agent_config(config);
    } catch (const agent::ConfigurationError& error) {
      failed = std::string(error.what()).find(expected) != std::string::npos;
    }
    SMOKE_CHECK(failed);
  };
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "invalid-store"},
                      {"store", agent::Value::object({{"kind", "file"}})},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.base.store.filePath must be a non-empty string");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "invalid-vector"},
                      {"vectorIndex", agent::Value::object({
                          {"kind", "sqlite"},
                          {"filePath", "vectors.sqlite"},
                          {"dimensions", 256},
                          {"batchSize", 0},
                      })},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.base.vectorIndex.batchSize must be >= 1");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "missing-vector-dimensions"},
                      {"vectorIndex", agent::Value::object({
                          {"kind", "sqlite"},
                          {"filePath", "vectors.sqlite"},
                      })},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.base.vectorIndex.dimensions must be a positive number");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "mismatched-vector-dimensions"},
                      {"embedder", agent::Value::object({{"provider", "hash"}, {"dimensions", 32}})},
                      {"vectorIndex", agent::Value::object({
                          {"kind", "sqlite"},
                          {"filePath", "vectors.sqlite"},
                          {"dimensions", 16},
                      })},
                  })},
              })},
          })},
      })},
  }), "vectorIndex.dimensions must match embedder dimensions");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"manager", agent::Value::object({
                      {"embedder", agent::Value::object({{"provider", "hash"}, {"dimensions", 32}})},
                      {"bases", agent::Value::array({
                          agent::Value::object({
                              {"id", "manager-mismatched-vector-dimensions"},
                              {"vectorIndex", agent::Value::object({
                                  {"kind", "sqlite"},
                                  {"filePath", "vectors.sqlite"},
                                  {"dimensions", 16},
                              })},
                          }),
                      })},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.manager.bases[0].vectorIndex.dimensions must match embedder dimensions");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "invalid-context-title"},
                      {"contextTitle", 42},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.base.contextTitle must be a string");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "invalid-chunking"},
                      {"chunkSize", 0},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.base.chunkSize must be >= 1");
  expect_config_validation_error(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"knowledge", agent::Value::object({
                  {"base", agent::Value::object({
                      {"id", "invalid-ingest"},
                      {"ingestOptions", agent::Value::object({{"embeddingBatchSize", 0}})},
                  })},
              })},
          })},
      })},
  }), "agents.main.knowledge.base.ingestOptions.embeddingBatchSize must be >= 1");
  bool invalid_browser_fallback_failed = false;
  try {
    (void)agent::resolve_native_agent_app(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
                {"tools", agent::Value::object({{"bundles", agent::Value::array({"web"})}})},
                {"web", agent::Value::object({
                    {"fetcher", agent::Value::object({
                        {"browserFallback", agent::Value::object({{"minimumTextLength", -1}})},
                    })},
                })},
            })},
        })},
    }), "main");
  } catch (const agent::ConfigurationError& error) {
    invalid_browser_fallback_failed =
        std::string(error.what()).find("minimumTextLength must be >= 0") != std::string::npos;
  }
  SMOKE_CHECK(invalid_browser_fallback_failed);
  bool invalid_reasoning_budget_failed = false;
  try {
    agent::validate_native_agent_config(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({
                    {"provider", "anthropic"},
                    {"model", "claude-test"},
                    {"reasoning", agent::Value::object({{"enabled", true}, {"budget", 0}})},
                })},
            })},
        })},
    }));
  } catch (const agent::ConfigurationError& error) {
    invalid_reasoning_budget_failed =
        std::string(error.what()).find("budget must be a positive finite number") != std::string::npos;
  }
  SMOKE_CHECK(invalid_reasoning_budget_failed);
  bool invalid_reasoning_shape_failed = false;
  try {
    agent::validate_native_agent_config(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({
                    {"provider", "openai"},
                    {"model", "gpt-test"},
                    {"reasoning", agent::Value::object({{"enabled", "yes"}, {"unknown", true}})},
                })},
            })},
        })},
    }));
  } catch (const agent::ConfigurationError& error) {
    invalid_reasoning_shape_failed =
        std::string(error.what()).find("not a recognized reasoning field") != std::string::npos ||
        std::string(error.what()).find("enabled must be a boolean") != std::string::npos;
  }
  SMOKE_CHECK(invalid_reasoning_shape_failed);
  bool null_reasoning_failed = false;
  try {
    agent::validate_native_agent_config(agent::Value::object({
        {"agents", agent::Value::object({
            {"main", agent::Value::object({
                {"model", agent::Value::object({
                    {"provider", "openai"},
                    {"model", "gpt-test"},
                    {"reasoning", agent::Value()},
                })},
            })},
        })},
    }));
  } catch (const agent::ConfigurationError& error) {
    null_reasoning_failed =
        std::string(error.what()).find("reasoning must be a JSON object") != std::string::npos;
  }
  SMOKE_CHECK(null_reasoning_failed);
  agent::assert_reasoning_settings(agent::Value::object({
      {"enabled", true},
      {"budget", 12000},
      {"includeThoughts", true},
      {"tagName", "think"},
  }));
  SMOKE_CHECK(agent::normalize_reasoning_budget_to_effort(std::variant<std::monostate, std::string, double>{512.0}) ==
         "low");
  SMOKE_CHECK(agent::normalize_reasoning_budget_to_effort(std::variant<std::monostate, std::string, double>{4096.0}) ==
         "medium");
  SMOKE_CHECK(agent::normalize_reasoning_budget_to_effort(std::variant<std::monostate, std::string, double>{12000.0}) ==
         "high");
  SMOKE_CHECK(agent::normalize_reasoning_budget_to_effort(
             std::variant<std::monostate, std::string, double>{std::string("high")}) == "high");
  SMOKE_CHECK(agent::normalize_reasoning_budget_to_tokens(
             std::variant<std::monostate, std::string, double>{std::string("low")},
             agent::ReasoningBudgetTokenTable{1024, 4096, 16384}) == 1024);
  SMOKE_CHECK(agent::normalize_reasoning_budget_to_tokens(
             std::variant<std::monostate, std::string, double>{7777.0},
             agent::ReasoningBudgetTokenTable{1024, 4096, 16384}) == 7777);
  agent::ReasoningSettings base_reasoning;
  base_reasoning.enabled = true;
  base_reasoning.budget = std::string("low");
  base_reasoning.include_thoughts = true;
  agent::ReasoningSettings override_reasoning;
  override_reasoning.budget = std::string("high");
  auto merged_reasoning = agent::merge_reasoning_settings(base_reasoning, override_reasoning);
  SMOKE_CHECK(merged_reasoning);
  SMOKE_CHECK(merged_reasoning->enabled == true);
  SMOKE_CHECK(std::get<std::string>(merged_reasoning->budget) == "high");
  SMOKE_CHECK(merged_reasoning->include_thoughts == true);
  auto tagged_reasoning = agent::extract_tagged_reasoning("visible <think>hidden</think> answer");
  SMOKE_CHECK(tagged_reasoning.text == "visible  answer");
  SMOKE_CHECK(tagged_reasoning.reasoning);
  SMOKE_CHECK(tagged_reasoning.reasoning->text == "hidden");
  SMOKE_CHECK(tagged_reasoning.reasoning->format == "tagged");
  auto custom_tagged_reasoning = agent::extract_tagged_reasoning("<plan>step one</plan>done", "plan");
  SMOKE_CHECK(custom_tagged_reasoning.text == "done");
  SMOKE_CHECK(custom_tagged_reasoning.reasoning->text == "step one");
  agent::TaggedReasoningStreamParser tagged_stream_parser;
  auto tagged_stream_first = tagged_stream_parser.feed("<think>part");
  SMOKE_CHECK(tagged_stream_first.size() == 1);
  SMOKE_CHECK(tagged_stream_first.front().type == agent::TaggedReasoningDeltaType::Reasoning);
  SMOKE_CHECK(tagged_stream_first.front().delta == "part");
  auto tagged_stream_second = tagged_stream_parser.feed(" two</think>answer");
  SMOKE_CHECK(tagged_stream_second.size() == 2);
  SMOKE_CHECK(tagged_stream_second.front().type == agent::TaggedReasoningDeltaType::Reasoning);
  SMOKE_CHECK(tagged_stream_second.front().delta == " two");
  SMOKE_CHECK(tagged_stream_second.back().type == agent::TaggedReasoningDeltaType::Text);
  SMOKE_CHECK(tagged_stream_second.back().text == "answer");
  SMOKE_CHECK(tagged_stream_parser.finish().empty());
  SMOKE_CHECK(tagged_stream_parser.to_reasoning()->text == "part two");
  agent::validate_native_agent_config(agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({
                  {"provider", "qwen"},
                  {"model", "qwen-reasoning"},
                  {"reasoning", agent::Value::object({{"enabled", true}, {"budget", "high"}, {"tagName", "think"}})},
              })},
          })},
      })},
  }));

  const auto permission_target = persistent_dir / "permission-denied.txt";
  int permission_model_calls = 0;
  auto config_with_permissions = agent::Value::object({
      {"resources", agent::Value::object({
          {"permissionPolicies", agent::Value::object({
              {"writes", agent::Value::object({
                  {"kind", "capability"},
                  {"allow", agent::Value::array({"fs.write"})},
                  {"highRiskMode", "ask"},
              })},
          })},
          {"approvalHandlers", agent::Value::object({
              {"reject", agent::Value::object({
                  {"kind", "static"},
                  {"decision", "deny"},
                  {"reason", "Config says no"},
              })},
          })},
      })},
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"maxIterations", 3},
              {"model", agent::Value::object({
                  {"provider", "openai"},
                  {"model", "gpt-config"},
                  {"apiKey", "permission-model-key"},
              })},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"local"})}})},
              {"permissions", agent::Value::object({
                  {"policy", agent::Value::object({{"use", "writes"}})},
                  {"approval", agent::Value::object({{"use", "reject"}})},
              })},
          })},
      })},
  });
  auto permission_app = agent::resolve_native_agent_app(
      config_with_permissions, "main",
      [&](const agent::NativeProviderRequest& request) {
        permission_model_calls += 1;
        if (permission_model_calls == 1) {
          SMOKE_CHECK(!request.body.at("tools").as_array().empty());
          return agent::Value::object({
              {"id", "perm_1"},
              {"choices", agent::Value::array({
                  agent::Value::object({
                      {"message", agent::Value::object({
                          {"content", ""},
                          {"tool_calls", agent::Value::array({
                              agent::Value::object({
                                  {"id", "call_write"},
                                  {"type", "function"},
                                  {"function", agent::Value::object({
                                      {"name", "fs.writeText"},
                                      {"arguments", agent::safe_json_stringify(agent::Value::object({
                                          {"path", permission_target.string()},
                                          {"content", "blocked"},
                                      }))},
                                  })},
                              }),
                          })},
                      })},
                      {"finish_reason", "tool_calls"},
                  }),
              })},
          });
        }
        const auto messages_text = request.body.at("messages").stringify();
        SMOKE_CHECK(messages_text.find("fs.writeText") != std::string::npos);
        SMOKE_CHECK(messages_text.find("not permitted") != std::string::npos);
        SMOKE_CHECK(messages_text.find("Config says no") == std::string::npos);
        return agent::Value::object({
            {"id", "perm_2"},
            {"choices", agent::Value::array({
                agent::Value::object({
                    {"message", agent::Value::object({{"content", "permission checked"}})},
                    {"finish_reason", "stop"},
                }),
            })},
        });
      });
  auto permission_run = permission_app.runner->run("try write", "permission-session");
  SMOKE_CHECK(permission_model_calls == 2);
  SMOKE_CHECK(permission_run.text == "permission checked");
  SMOKE_CHECK(!std::filesystem::exists(permission_target));

  bool configured_developer_exec_called = false;
  agent::NativeDeveloperAdapters developer_adapters;
  developer_adapters.process_executor = [&](const agent::DeveloperProcessRequest& request) {
    configured_developer_exec_called = true;
    SMOKE_CHECK(request.command == "configured-command");
    SMOKE_CHECK((request.args == std::vector<std::string>{"alpha"}));
    return agent::DeveloperProcessResult{.stdout_text = "configured developer stdout"};
  };
  auto config_with_developer = agent::Value::object({
      {"agents", agent::Value::object({
          {"main", agent::Value::object({
              {"model", agent::Value::object({{"provider", "echo"}, {"model", "echo"}})},
              {"tools", agent::Value::object({{"bundles", agent::Value::array({"developer"})}})},
          })},
      })},
  });
  auto developer_app = agent::resolve_native_agent_app(config_with_developer, "main", {}, {}, {}, developer_adapters);
  agent::ToolRegistry configured_developer_tools(developer_app.tools);
  agent::ToolExecutor configured_developer_executor(configured_developer_tools);
  // shell.exec requires shell_safe capabilities — install a permissive test
  // sandbox so the configured developer flow can run end-to-end.
  struct ConfiguredDeveloperSandbox final : agent::Sandbox {
    agent::SandboxCapabilities max_capabilities() const noexcept override {
      return agent::SandboxCapabilities{
          .fs = agent::FsAccess::Unrestricted,
          .net = agent::NetAccess::Unrestricted,
          .process = agent::ProcessAccess::Unrestricted,
          .syscall = agent::SyscallAccess::Unrestricted,
      };
    }
    std::unique_ptr<agent::SandboxScope> enter(const agent::SandboxRequest& request) override {
      struct InlineScope final : agent::SandboxScope {
        explicit InlineScope(agent::SandboxRequest req) : req_(std::move(req)) {}
        const agent::SandboxRequest& request() const noexcept override { return req_; }
        agent::SandboxCapabilities effective() const noexcept override { return req_.capabilities; }
        agent::SandboxRequest req_;
      };
      return std::make_unique<InlineScope>(request);
    }
  };
  ConfiguredDeveloperSandbox configured_developer_sandbox;
  agent::ToolExecutionContext configured_developer_ctx;
  configured_developer_ctx.service_refs.sandbox = &configured_developer_sandbox;
  auto configured_developer_shell = configured_developer_executor.execute_tool_call(agent::ToolCall{
      .id = "config_developer_shell_1",
      .name = "shell.exec",
      .arguments = agent::Value::object({
          {"command", "configured-command"},
          {"args", agent::Value::array({"alpha"})},
      }),
  }, configured_developer_ctx);
  SMOKE_CHECK(configured_developer_exec_called);
  SMOKE_CHECK(configured_developer_shell.ok);
  SMOKE_CHECK(configured_developer_shell.output.find("configured developer stdout") != std::string::npos);
