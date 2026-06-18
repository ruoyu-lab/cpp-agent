#include "agent/full.hpp"
#include "agent/providers/reasoning.hpp"
#include "agent/react.hpp"
#include "../src/agent/detail/helpers.hpp"
#include "../src/agent/function_calling_loop.hpp"

#include <cassert>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>
#include <variant>

#ifndef _WIN32
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#define SMOKE_CHECK(condition)                                                               \
  do {                                                                                       \
    if (!(condition)) {                                                                      \
      std::cerr << "SMOKE_CHECK failed: " #condition << " at " << __FILE__ << ":"          \
                << __LINE__ << "\n";                                                        \
      std::abort();                                                                          \
    }                                                                                        \
  } while (false)

namespace {

agent::NativeAgentAppResolveOptions native_resolve_options(
    std::string agent_id,
    agent::NativeProviderTransport provider_transport = {},
    agent::NativeMCPTransportFactory mcp_transport_factory = {},
    agent::NativeWebAdapters web_adapters = {},
    agent::NativeDeveloperAdapters developer_adapters = {},
    agent::NativeBrowserAdapters browser_adapters = {},
    agent::NativeProviderStreamTransport provider_stream_transport = {},
    agent::NativeLlamaCppAdapters llama_adapters = {},
    agent::NativeProviderStreamingTransport provider_streaming_transport = {}) {
  agent::NativeAgentAppResolveOptions options;
  options.requested_agent_id = std::move(agent_id);
  options.provider_transport = std::move(provider_transport);
  options.provider_stream_transport = std::move(provider_stream_transport);
  options.provider_streaming_transport = std::move(provider_streaming_transport);
  options.mcp_transport_factory = std::move(mcp_transport_factory);
  options.web_adapters = std::move(web_adapters);
  options.developer_adapters = std::move(developer_adapters);
  options.browser_adapters = std::move(browser_adapters);
  options.llama_adapters = std::move(llama_adapters);
  return options;
}

// Resolve the on-disk root used by smoke fixtures (skill projects, knowledge
// dirs, native stores, audit logs, replays). Uses the OS-provided temp
// directory so artifacts never touch the source tree regardless of how the
// binary is invoked or which CMake build directory the user picked. Matches
// the convention already used by other smoke chunks (e.g. the stale-read
// fixture uses `temp_directory_path() / "agent_native_smoke_stale"`).
std::filesystem::path smoke_artifacts_root() {
  return std::filesystem::temp_directory_path() / "agent_native_smoke_artifacts";
}

agent::OpenAICompatibleRuntimeOptions openai_smoke_runtime_options() {
  return agent::OpenAICompatibleRuntimeOptions{
      .service_name = "OpenAI",
      .api_key_env_name = "OPENAI_API_KEY",
      .require_api_key = true,
      .supports_responses_api = true,
      .supports_prompt_cache_key = true,
      .capabilities = {"input.text", "input.image", "input.file", "input.audio", "input.video",
                       "output.structuredContent", "transport.inline", "transport.reference",
                       "reasoning", "reasoning.budget", "reasoning.includeThoughts"},
  };
}

#ifndef _WIN32
std::string read_loopback_http(const std::string& host, int port, const std::string& request) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  SMOKE_CHECK(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  SMOKE_CHECK(::inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1);
  SMOKE_CHECK(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  std::size_t sent = 0;
  while (sent < request.size()) {
    const auto result = ::send(fd, request.data() + sent, request.size() - sent, 0);
    SMOKE_CHECK(result > 0);
    sent += static_cast<std::size_t>(result);
  }

  std::string response;
  char buffer[4096];
  while (true) {
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    if (received <= 0) {
      break;
    }
    response.append(buffer, static_cast<std::size_t>(received));
  }
  (void)::close(fd);
  return response;
}

std::string http_body(const std::string& response) {
  const auto crlf = response.find("\r\n\r\n");
  if (crlf != std::string::npos) {
    return response.substr(crlf + 4);
  }
  const auto lf = response.find("\n\n");
  return lf == std::string::npos ? std::string{} : response.substr(lf + 2);
}
#endif

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

struct CapturedRunnerStream {
  std::vector<agent::AgentRunnerStreamEvent> events;
  agent::AgentRunnerRunResult result;
};

template <typename Input, typename... Args>
CapturedRunnerStream capture_runner_stream(agent::AgentRunner& runner, Input&& input, Args&&... args) {
  CapturedRunnerStream captured;
  auto stream = runner.streaming().stream(std::forward<Input>(input),
                              [&](const agent::AgentRunnerStreamEvent& event) {
                                captured.events.push_back(event);
                              },
                              std::forward<Args>(args)...);
  captured.result = std::move(stream.result);
  return captured;
}

template <typename Input, typename... Args>
CapturedRunnerStream capture_runner_stream(agent::AgentRunner&& runner, Input&& input, Args&&... args) {
  return capture_runner_stream(runner, std::forward<Input>(input), std::forward<Args>(args)...);
}

struct CapturedLoopStream {
  std::vector<agent::AgentLoopStreamEvent> events;
  agent::AgentLoopRunResult result;
};

template <typename Loop, typename Input, typename... Args>
CapturedLoopStream capture_loop_stream(Loop& loop,
                                       agent::SessionMemory& session,
                                       Input&& input,
                                       Args&&... args) {
  CapturedLoopStream captured;
  auto stream = loop.stream(session,
                            std::forward<Input>(input),
                            [&](const agent::AgentLoopStreamEvent& event) {
                              captured.events.push_back(event);
                            },
                            std::forward<Args>(args)...);
  captured.result = std::move(stream.result);
  return captured;
}

std::vector<agent::ModelStreamEvent> capture_model_stream(agent::ChatModelAdapter& model,
                                                          agent::GenerateParams params) {
  std::vector<agent::ModelStreamEvent> events;
  model.stream(std::move(params), [&](const agent::ModelStreamEvent& event) {
    events.push_back(event);
  });
  return events;
}

class FixedSearchEmbeddingAdapter final : public agent::TextEmbeddingAdapter {
 public:
  FixedSearchEmbeddingAdapter() : agent::TextEmbeddingAdapter("test", "fixed-search", 2, "fixed-search-v1") {}

  std::vector<agent::EmbeddingVector> embed(const std::vector<std::string>& texts,
                                            const agent::Value& settings = {},
                                            agent::CancellationToken* cancellation = nullptr) override {
    (void)settings;
    (void)cancellation;
    std::vector<agent::EmbeddingVector> embeddings;
    embeddings.reserve(texts.size());
    for (const auto& text : texts) {
      embeddings.push_back(text.find("anti") == std::string::npos
                               ? agent::EmbeddingVector{1.0, 0.0}
                               : agent::EmbeddingVector{-1.0, 0.0});
    }
    return embeddings;
  }
};

class PassthroughKnowledgeReranker final : public agent::KnowledgeReranker {
 public:
  std::vector<agent::KnowledgeSearchHit> rerank(const std::string& query,
                                                std::vector<agent::KnowledgeSearchHit> hits,
                                                std::size_t top_k) const override {
    (void)query;
    if (top_k > 0 && hits.size() > top_k) {
      hits.resize(top_k);
    }
    return hits;
  }
};

class FakeRedisRealtimeClient final : public agent::RedisRealtimeClient {
 public:
  long long publish(std::string channel, std::string message) override {
    published.emplace_back(std::move(channel), std::move(message));
    return 1;
  }

  void psubscribe(std::string pattern) override {
    patterns.push_back(std::move(pattern));
  }

  void punsubscribe(std::string pattern) override {
    patterns.erase(std::remove(patterns.begin(), patterns.end(), pattern), patterns.end());
  }

  void set_pmessage_handler(PMessageHandler handler) override {
    handler_ = std::move(handler);
  }

  void emit(const std::string& pattern, const std::string& channel, const std::string& message) {
    if (handler_) {
      handler_(pattern, channel, message);
    }
  }

  std::vector<std::pair<std::string, std::string>> published;
  std::vector<std::string> patterns;

 private:
  PMessageHandler handler_;
};

class FakeBullMQQueueClient final : public agent::BullMQQueueClient {
 public:
  struct AddedJob {
    std::string name;
    std::string task_id;
    agent::Value options;
  };

  void add(std::string name, agent::BullMQTaskJobData data, agent::BullMQJobOptions options) override {
    std::lock_guard<std::mutex> lock(mutex_);
    added_jobs.push_back(AddedJob{std::move(name), std::move(data.task_id), std::move(options.values)});
  }

  void remove_job(const std::string& job_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    removed_job_ids.push_back(job_id);
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    closed = true;
    close_count += 1;
  }

  std::mutex mutex_;
  std::vector<AddedJob> added_jobs;
  std::vector<std::string> removed_job_ids;
  bool closed = false;
  int close_count = 0;
};

class FakePgTaskClient final : public agent::PgTaskClient {
 public:
  struct QueryRecord {
    std::string sql;
    std::vector<agent::Value> params;
  };

  std::vector<agent::Value> query(std::string sql, std::vector<agent::Value> params = {}) override {
    std::lock_guard<std::mutex> lock(mutex_);
    queries.push_back(QueryRecord{sql, params});

    if (sql.find("INSERT INTO") != std::string::npos && sql.find("node_agent_tasks") != std::string::npos) {
      return {agent::Value::object({
          {"id", params.at(0)},
          {"type", params.at(1)},
          {"input", params.at(2)},
          {"idempotency_key", params.at(3)},
          {"owner_api_key_id", params.at(4)},
          {"tenant_id", params.at(5)},
          {"status", "queued"},
          {"created_at", params.at(6)},
          {"updated_at", params.at(6)},
          {"queued_at", params.at(6)},
          {"started_at", agent::Value()},
          {"completed_at", agent::Value()},
          {"cancelled_at", agent::Value()},
          {"output", agent::Value()},
          {"error", agent::Value()},
          {"metadata", params.at(7)},
      })};
    }

    if (sql.find("JOIN LATERAL") != std::string::npos) {
      return {};
    }

    if (sql.find("SELECT t.*") != std::string::npos) {
      const long long now = 1779321600000LL;
      return {agent::Value::object({
          {"id", "task-claim"},
          {"type", "agent.run"},
          {"input", agent::Value::object({{"prompt", "go"}})},
          {"idempotency_key", agent::Value()},
          {"owner_api_key_id", "key-a"},
          {"tenant_id", "tenant-a"},
          {"status", "queued"},
          {"created_at", now},
          {"updated_at", now},
          {"queued_at", now},
          {"started_at", agent::Value()},
          {"completed_at", agent::Value()},
          {"cancelled_at", agent::Value()},
          {"output", agent::Value()},
          {"error", agent::Value()},
          {"metadata", agent::Value::object({})},
      })};
    }

    if (sql.find("COUNT(*)") != std::string::npos) {
      return {agent::Value::object({{"count", "2"}})};
    }

    if (sql.find("INSERT INTO") != std::string::npos && sql.find("node_agent_task_runs") != std::string::npos) {
      return {agent::Value::object({
          {"id", params.at(0)},
          {"task_id", params.at(1)},
          {"status", "running"},
          {"started_at", params.at(2)},
          {"completed_at", agent::Value()},
          {"output", agent::Value()},
          {"error", agent::Value()},
          {"lease_expires_at", params.at(3)},
          {"heartbeat_at", params.at(2)},
          {"attempt", params.at(4)},
      })};
    }

    if (sql.find("UPDATE") != std::string::npos && sql.find("node_agent_tasks") != std::string::npos) {
      return {agent::Value::object({
          {"id", params.at(0)},
          {"type", "agent.run"},
          {"input", agent::Value::object({{"prompt", "go"}})},
          {"idempotency_key", agent::Value()},
          {"owner_api_key_id", "key-a"},
          {"tenant_id", "tenant-a"},
          {"status", "running"},
          {"created_at", 1779321600000LL},
          {"updated_at", params.at(1)},
          {"queued_at", 1779321600000LL},
          {"started_at", params.at(1)},
          {"completed_at", agent::Value()},
          {"cancelled_at", agent::Value()},
          {"output", agent::Value()},
          {"error", agent::Value()},
          {"metadata", agent::Value::object({})},
      })};
    }

    return {};
  }

  std::mutex mutex_;
  std::vector<QueryRecord> queries;
};

class FakePgApprovalClient final : public agent::PgApprovalClient {
 public:
  struct QueryRecord {
    std::string sql;
    std::vector<agent::Value> params;
  };

  std::vector<agent::Value> query(std::string sql, std::vector<agent::Value> params = {}) override {
    std::lock_guard<std::mutex> lock(mutex);
    queries.push_back(QueryRecord{sql, params});

    if (sql.find("CREATE SCHEMA") != std::string::npos ||
        sql.find("CREATE TABLE") != std::string::npos ||
        sql.find("CREATE INDEX") != std::string::npos) {
      return {};
    }

    if (sql.find("INSERT INTO") != std::string::npos && sql.find("node_agent_approvals") != std::string::npos) {
      auto row = agent::Value::object({
          {"id", params.at(0)},
          {"created_at", params.at(1)},
          {"resolved_at", agent::Value()},
          {"status", "pending"},
          {"request", params.at(2)},
          {"proposed_decision", params.at(3)},
          {"final_decision", agent::Value()},
          {"metadata", params.at(4)},
      });
      rows[row.at("id").as_string()] = row;
      return {row};
    }

    if (sql.find("UPDATE") != std::string::npos && sql.find("node_agent_approvals") != std::string::npos) {
      auto found = rows.find(params.at(0).as_string());
      if (found == rows.end() || found->second.at("status").as_string() != "pending") {
        return {};
      }
      found->second["status"] = "resolved";
      found->second["resolved_at"] = params.at(1);
      found->second["final_decision"] = params.at(2);
      return {found->second};
    }

    if (sql.find("SELECT *") != std::string::npos && sql.find("WHERE id = $1") != std::string::npos) {
      auto found = rows.find(params.at(0).as_string());
      return found == rows.end() ? std::vector<agent::Value>{} : std::vector<agent::Value>{found->second};
    }

    if (sql.find("SELECT *") != std::string::npos && sql.find("node_agent_approvals") != std::string::npos) {
      std::vector<agent::Value> output;
      for (const auto& [_, row] : rows) {
        if (params.empty() || row.at("status").as_string() == params.at(0).as_string()) {
          output.push_back(row);
        }
      }
      return output;
    }

    return {};
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    ++close_count;
  }

  std::mutex mutex;
  std::vector<QueryRecord> queries;
  std::map<std::string, agent::Value> rows;
  bool closed = false;
  int close_count = 0;
};

class FakePgAutonomousClient final : public agent::PgAutonomousClient {
 public:
  struct QueryRecord {
    std::string sql;
    std::vector<agent::Value> params;
  };

  std::vector<agent::Value> query(std::string sql, std::vector<agent::Value> params = {}) override {
    std::lock_guard<std::mutex> lock(mutex);
    queries.push_back(QueryRecord{sql, params});

    if (sql == "BEGIN" || sql == "COMMIT" || sql == "ROLLBACK" ||
        sql.find("CREATE SCHEMA") != std::string::npos ||
        sql.find("CREATE TABLE") != std::string::npos ||
        sql.find("CREATE INDEX") != std::string::npos) {
      return {};
    }

    if (sql.find("DELETE FROM") != std::string::npos &&
        sql.find("node_agent_autonomous_steps") != std::string::npos) {
      auto& items = steps[params.at(0).as_string()];
      items.clear();
      return {};
    }

    if (sql.find("INSERT INTO") != std::string::npos &&
        sql.find("node_agent_autonomous_runs") != std::string::npos) {
      auto row = agent::Value::object({
          {"id", params.at(0)},
          {"goal", params.at(1)},
          {"input", params.at(2)},
          {"status", "queued"},
          {"output", agent::Value()},
          {"error", agent::Value()},
          {"created_at", params.at(3)},
          {"updated_at", params.at(3)},
          {"completed_at", agent::Value()},
          {"metadata", params.at(4)},
      });
      runs[row.at("id").as_string()] = row;
      return {row};
    }

    if (sql.find("UPDATE") != std::string::npos &&
        sql.find("node_agent_autonomous_runs") != std::string::npos) {
      auto found = runs.find(params.back().as_string());
      if (found == runs.end()) {
        return {};
      }
      std::size_t index = 0;
      if (sql.find("goal = $") != std::string::npos) found->second["goal"] = params.at(index++);
      if (sql.find("input = $") != std::string::npos) found->second["input"] = params.at(index++);
      if (sql.find("status = $") != std::string::npos) found->second["status"] = params.at(index++);
      if (sql.find("output = $") != std::string::npos) found->second["output"] = params.at(index++);
      if (sql.find("error = $") != std::string::npos) found->second["error"] = params.at(index++);
      if (sql.find("updated_at = $") != std::string::npos) found->second["updated_at"] = params.at(index++);
      if (sql.find("completed_at = $") != std::string::npos) found->second["completed_at"] = params.at(index++);
      if (sql.find("metadata = $") != std::string::npos) found->second["metadata"] = params.at(index++);
      return {found->second};
    }

    if (sql.find("SELECT COUNT(*)") != std::string::npos &&
        sql.find("node_agent_autonomous_steps") != std::string::npos) {
      return {agent::Value::object({{"count", std::to_string(steps[params.at(0).as_string()].size())}})};
    }

    if (sql.find("INSERT INTO") != std::string::npos &&
        sql.find("node_agent_autonomous_steps") != std::string::npos) {
      auto row = agent::Value::object({
          {"id", params.at(0)},
          {"run_id", params.at(1)},
          {"step_index", params.at(2)},
          {"title", params.at(3)},
          {"objective", params.at(4)},
          {"input", params.at(5)},
          {"status", params.at(6)},
          {"attempts", params.at(7)},
          {"output", params.at(8)},
          {"error", params.at(9)},
          {"wait_reason", params.at(10)},
          {"depends_on", params.at(11)},
          {"created_at", params.at(12)},
          {"updated_at", params.at(13)},
          {"started_at", params.at(14)},
          {"completed_at", params.at(15)},
          {"metadata", params.at(16)},
      });
      steps[row.at("run_id").as_string()].push_back(row);
      return {row};
    }

    if (sql.find("UPDATE") != std::string::npos &&
        sql.find("node_agent_autonomous_steps") != std::string::npos) {
      auto& items = steps[params.at(params.size() - 2).as_string()];
      auto found = std::find_if(items.begin(), items.end(), [&](const agent::Value& row) {
        return row.at("id").as_string() == params.back().as_string();
      });
      if (found == items.end()) {
        return {};
      }
      std::size_t index = 0;
      if (sql.find("title = $") != std::string::npos) (*found)["title"] = params.at(index++);
      if (sql.find("objective = $") != std::string::npos) (*found)["objective"] = params.at(index++);
      if (sql.find("input = $") != std::string::npos) (*found)["input"] = params.at(index++);
      if (sql.find("status = $") != std::string::npos) (*found)["status"] = params.at(index++);
      if (sql.find("attempts = $") != std::string::npos) (*found)["attempts"] = params.at(index++);
      if (sql.find("output = $") != std::string::npos) (*found)["output"] = params.at(index++);
      if (sql.find("error = $") != std::string::npos) (*found)["error"] = params.at(index++);
      if (sql.find("wait_reason = $") != std::string::npos) (*found)["wait_reason"] = params.at(index++);
      if (sql.find("depends_on = $") != std::string::npos) (*found)["depends_on"] = params.at(index++);
      if (sql.find("updated_at = $") != std::string::npos) (*found)["updated_at"] = params.at(index++);
      if (sql.find("started_at = $") != std::string::npos) (*found)["started_at"] = params.at(index++);
      if (sql.find("completed_at = $") != std::string::npos) (*found)["completed_at"] = params.at(index++);
      if (sql.find("metadata = $") != std::string::npos) (*found)["metadata"] = params.at(index++);
      return {*found};
    }

    if (sql.find("INSERT INTO") != std::string::npos &&
        sql.find("node_agent_autonomous_events") != std::string::npos) {
      auto row = agent::Value::object({
          {"id", params.at(0)},
          {"run_id", params.at(1)},
          {"step_id", params.at(2)},
          {"type", params.at(3)},
          {"payload", params.at(4)},
          {"created_at", params.at(5)},
      });
      events[row.at("run_id").as_string()].push_back(row);
      return {row};
    }

    if (sql.find("INSERT INTO") != std::string::npos &&
        sql.find("node_agent_autonomous_checkpoints") != std::string::npos) {
      auto row = agent::Value::object({
          {"id", params.at(0)},
          {"run_id", params.at(1)},
          {"step_id", params.at(2)},
          {"name", params.at(3)},
          {"state", params.at(4)},
          {"created_at", params.at(5)},
      });
      checkpoints[row.at("run_id").as_string()].push_back(row);
      return {row};
    }

    if (sql.find("SELECT *") != std::string::npos &&
        sql.find("node_agent_autonomous_steps") != std::string::npos) {
      return steps[params.at(0).as_string()];
    }

    if (sql.find("SELECT *") != std::string::npos &&
        sql.find("node_agent_autonomous_events") != std::string::npos) {
      return events[params.at(0).as_string()];
    }

    if (sql.find("SELECT *") != std::string::npos &&
        sql.find("node_agent_autonomous_checkpoints") != std::string::npos) {
      return checkpoints[params.at(0).as_string()];
    }

    if (sql.find("SELECT *") != std::string::npos &&
        sql.find("node_agent_autonomous_runs") != std::string::npos &&
        sql.find("WHERE id = $1") != std::string::npos) {
      auto found = runs.find(params.at(0).as_string());
      return found == runs.end() ? std::vector<agent::Value>{} : std::vector<agent::Value>{found->second};
    }

    if (sql.find("SELECT *") != std::string::npos &&
        sql.find("node_agent_autonomous_runs") != std::string::npos) {
      std::vector<agent::Value> output;
      for (const auto& [_, row] : runs) {
        std::size_t index = 0;
        if (sql.find("status = $") != std::string::npos &&
            row.at("status").as_string() != params.at(index++).as_string()) {
          continue;
        }
        if (sql.find("metadata @> $") != std::string::npos &&
            !metadata_contains(row.at("metadata"), params.at(index++))) {
          continue;
        }
        output.push_back(row);
      }
      return output;
    }

    return {};
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex);
    closed = true;
    ++close_count;
  }

  static bool metadata_contains(const agent::Value& metadata, const agent::Value& filter_param) {
    const auto filter = filter_param.is_string() ? agent::parse_json(filter_param.as_string()) : filter_param;
    const auto source = metadata.is_string() ? agent::parse_json(metadata.as_string()) : metadata;
    if (!filter.is_object()) {
      return true;
    }
    for (const auto& [key, value] : filter.as_object()) {
      if (!source.is_object() || !source.contains(key) || source.at(key) != value) {
        return false;
      }
    }
    return true;
  }

  std::mutex mutex;
  std::vector<QueryRecord> queries;
  std::map<std::string, agent::Value> runs;
  std::map<std::string, std::vector<agent::Value>> steps;
  std::map<std::string, std::vector<agent::Value>> events;
  std::map<std::string, std::vector<agent::Value>> checkpoints;
  bool closed = false;
  int close_count = 0;
};

class FakeSocketIoToolClientSocket final : public agent::SocketIoToolClientSocket,
                                           public std::enable_shared_from_this<FakeSocketIoToolClientSocket> {
 public:
  void set_peer(std::shared_ptr<FakeSocketIoToolClientSocket> peer) {
    std::lock_guard<std::mutex> lock(mutex_);
    peer_ = std::move(peer);
  }

  void emit(std::string event_name, agent::ToolClientMessage message) override {
    std::shared_ptr<FakeSocketIoToolClientSocket> peer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      emitted_events.push_back(event_name);
      peer = peer_.lock();
    }
    if (peer) {
      peer->deliver_message(event_name, message);
    }
  }

  std::string on_message(std::string event_name, agent::SocketIoToolClientMessageListener listener) override {
    const auto id = agent::generate_uuid();
    std::lock_guard<std::mutex> lock(mutex_);
    message_listeners_[id] = MessageListener{std::move(event_name), std::move(listener)};
    return id;
  }

  std::string on_disconnect(agent::SocketIoToolClientDisconnectListener listener) override {
    const auto id = agent::generate_uuid();
    std::lock_guard<std::mutex> lock(mutex_);
    disconnect_listeners_[id] = std::move(listener);
    return id;
  }

  void off(const std::string& subscription_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    message_listeners_.erase(subscription_id);
    disconnect_listeners_.erase(subscription_id);
  }

  void disconnect(const std::string& reason) {
    std::shared_ptr<FakeSocketIoToolClientSocket> peer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      peer = peer_.lock();
    }
    if (peer) {
      peer->deliver_disconnect(reason);
    }
  }

  std::vector<std::string> emitted_events;

 private:
  struct MessageListener {
    std::string event_name;
    agent::SocketIoToolClientMessageListener listener;
  };

  void deliver_message(const std::string& event_name, const agent::ToolClientMessage& message) {
    std::map<std::string, MessageListener> listeners;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      listeners = message_listeners_;
    }
    for (const auto& [_, entry] : listeners) {
      if (entry.event_name == event_name) {
        entry.listener(message);
      }
    }
  }

  void deliver_disconnect(const std::string& reason) {
    std::map<std::string, agent::SocketIoToolClientDisconnectListener> listeners;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      listeners = disconnect_listeners_;
    }
    for (const auto& [_, listener] : listeners) {
      listener(reason);
    }
  }

  mutable std::mutex mutex_;
  std::weak_ptr<FakeSocketIoToolClientSocket> peer_;
  std::map<std::string, MessageListener> message_listeners_;
  std::map<std::string, agent::SocketIoToolClientDisconnectListener> disconnect_listeners_;
};

class FakeSocketIoToolClientServer final : public agent::SocketIoToolClientServer {
 public:
  std::string on_connection(agent::SocketIoToolClientConnectionListener listener) override {
    const auto id = agent::generate_uuid();
    std::lock_guard<std::mutex> lock(mutex_);
    connection_listeners_[id] = std::move(listener);
    return id;
  }

  void off_connection(const std::string& subscription_id) override {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_listeners_.erase(subscription_id);
  }

  void connect(std::shared_ptr<FakeSocketIoToolClientSocket> server_socket) {
    std::map<std::string, agent::SocketIoToolClientConnectionListener> listeners;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      listeners = connection_listeners_;
    }
    for (const auto& [_, listener] : listeners) {
      listener(server_socket);
    }
  }

 private:
  mutable std::mutex mutex_;
  std::map<std::string, agent::SocketIoToolClientConnectionListener> connection_listeners_;
};

}  // namespace

class ToolCallingModel final : public agent::ChatModelAdapter {
 public:
  ToolCallingModel() : ChatModelAdapter("test", "tool-caller", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    ++generate_calls_;
    for (const auto& message : params.messages) {
      const auto message_text = agent::extract_text_content(message.content);
      if (message_text.find("Observation:") != std::string::npos &&
          message.metadata.at("observation").as_bool(false) &&
          message_text.find("math.eval") != std::string::npos) {
        agent::AgentOutput response;
        response.text = "Thought: answer from the observation.\nFinal Answer: tool result: " +
            message_text;
        response.raw = agent::Value::object({
            {"usage", agent::Value::object({
                {"prompt_tokens", 20},
                {"completion_tokens", 10},
                {"total_tokens", 30},
            })},
            {"citations", agent::Value::array({
                agent::Value::object({{"id", "tool-citation"}, {"uri", "memory://tool"}}),
            })},
        });
        return build_output(std::move(response));
      }
    }

    agent::AgentOutput response;
    response.provider = provider();
    response.model = model();
    response.finish_reason = "stop";
    response.text = "Thought: evaluate the expression.\nActions:\n"
                    "[{\"tool\":\"math.eval\",\"input\":{\"expression\":\"1+2\"}}]";
    return build_output(response);
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }

 private:
  int generate_calls_ = 0;
};

class NativeToolCallingRunnerModel final : public agent::ChatModelAdapter {
 public:
  NativeToolCallingRunnerModel() : ChatModelAdapter("fixture", "native-tool-runner", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    ++generate_calls_;
    saw_native_tool_descriptors_ =
        saw_native_tool_descriptors_ ||
        std::any_of(params.tools.begin(), params.tools.end(), [](const agent::ChatToolDescriptor& tool) {
          return tool.name == "math.eval";
        });
    for (const auto& message : params.messages) {
      if (message.role != agent::MessageRole::Tool || message.name != "math.eval") {
        continue;
      }
      const auto text = agent::extract_text_content(message.content);
      return build_output(agent::AgentOutput{
          .provider = provider(),
          .model = model(),
          .text = "native tool result: " + text,
      });
    }
    agent::AgentOutput response;
    response.provider = provider();
    response.model = model();
    response.finish_reason = "tool_calls";
    response.tool_calls = {agent::ToolCall{
        .id = "call_native_math",
        .name = "math.eval",
        .arguments = agent::Value::object({{"expression", "1+2"}}),
    }};
    return build_output(std::move(response));
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] bool saw_native_tool_descriptors() const noexcept { return saw_native_tool_descriptors_; }

 private:
  int generate_calls_ = 0;
  bool saw_native_tool_descriptors_ = false;
};

class ThrowingToolRecoveryModel final : public agent::ChatModelAdapter {
 public:
  ThrowingToolRecoveryModel() : ChatModelAdapter("fixture", "throwing-tool-recovery", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    ++generate_calls_;
    for (const auto& message : params.messages) {
      const auto text = agent::extract_text_content(message.content);
      if (text.find("Observation:") != std::string::npos &&
          message.metadata.at("observation").as_bool(false) &&
          text.find("explode") != std::string::npos) {
        saw_failed_tool_message_ = text.find("boom") != std::string::npos;
        return build_output(agent::AgentOutput{
            .text = "Thought: recover from the failed tool.\nFinal Answer: recovered after tool failure",
        });
      }
    }

    agent::AgentOutput response;
    response.finish_reason = "stop";
    response.text = "Thought: run the failing tool.\nActions:\n"
                    "[{\"tool\":\"explode\",\"input\":{}}]";
    return build_output(response);
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] bool saw_failed_tool_message() const noexcept { return saw_failed_tool_message_; }

 private:
  int generate_calls_ = 0;
  bool saw_failed_tool_message_ = false;
};

class ExecutorFailureRecoveryModel final : public agent::ChatModelAdapter {
 public:
  ExecutorFailureRecoveryModel() : ChatModelAdapter("fixture", "executor-failure-recovery", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    ++generate_calls_;
    for (const auto& message : params.messages) {
      const auto text = agent::extract_text_content(message.content);
      if (text.find("Observation:") != std::string::npos &&
          message.metadata.at("observation").as_bool(false) &&
          text.find("permission.gated") != std::string::npos) {
        saw_synthetic_tool_message_ =
            text.find("Tool \\\"permission.gated\\\" threw an unexpected error.") != std::string::npos ||
            text.find("Tool \"permission.gated\" threw an unexpected error.") != std::string::npos;
        return build_output(agent::AgentOutput{
            .text = "Thought: recover from the executor failure.\nFinal Answer: recovered after executor failure",
        });
      }
    }

    agent::AgentOutput response;
    response.finish_reason = "stop";
    response.text = "Thought: run the gated tool.\nActions:\n"
                    "[{\"tool\":\"permission.gated\",\"input\":{}}]";
    return build_output(response);
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] bool saw_synthetic_tool_message() const noexcept { return saw_synthetic_tool_message_; }

 private:
  int generate_calls_ = 0;
  bool saw_synthetic_tool_message_ = false;
};

class IncompleteFinishStreamingModel final : public agent::ChatModelAdapter {
 public:
  IncompleteFinishStreamingModel() : ChatModelAdapter("fixture", "incomplete-stream", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    agent::AgentOutput response;
    response.text = "Thought: answer was truncated.\nFinal Answer: partial answer that got cut off";
    response.finish_reason = "length";
    return build_output(std::move(response));
  }
};

class CountingToolLoopModel final : public agent::ChatModelAdapter {
 public:
  CountingToolLoopModel() : ChatModelAdapter("fixture", "counting-tool-loop", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    agent::AgentOutput response;
    response.text = "Thought: continue loop " + std::to_string(generate_calls_) +
        ".\nActions:\n[{\"tool\":\"noop\",\"input\":{}}]";
    response.finish_reason = "stop";
    return build_output(std::move(response));
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }

 private:
  int generate_calls_ = 0;
};

class StreamingFixtureModel final : public agent::ChatModelAdapter {
 public:
  StreamingFixtureModel() : ChatModelAdapter("fixture", "stream-model", 0.0, 256,
                                             {"input.text", "input.image", "reasoning"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    bool react_prompt = false;
    for (const auto& message : params.messages) {
      react_prompt = react_prompt ||
                     (message.role == agent::MessageRole::System &&
                      agent::extract_text_content(message.content).find("internal tool-use format") != std::string::npos);
    }
    const std::string text = react_prompt
        ? "Thought: provide the fixture stream answer.\nFinal Answer: stream text"
        : "stream text";
    agent::AgentOutput response;
    response.text = text;
    response.reasoning = agent::ModelReasoning{.text = "thinking"};
    response.content = {
        agent::text_part(text),
        agent::image_part(agent::MediaSource{
            .kind = agent::MediaSourceKind::Url,
            .url = "https://example.test/plot.png",
            .mime_type = "image/png",
        }, "plot", "low"),
    };
    return build_output(response);
  }
};

class StreamingReActFinalModel final : public agent::ChatModelAdapter {
 public:
  StreamingReActFinalModel() : ChatModelAdapter("fixture", "stream-react-final", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    return build_output(agent::AgentOutput{
        .text = "Thought: hidden reasoning\nFinal Answer: 实时答案",
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    std::string text;
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
      for (const auto& chunk : std::vector<std::string>{
               "Thought: hidden reasoning",
               "\nFin",
               "al Answer: 实",
               "时",
               "答案",
           }) {
        text += chunk;
        on_event(agent::ModelStreamEvent{
            .type = agent::ModelStreamEventType::TextDelta,
            .provider = provider(),
            .model = model(),
            .delta = chunk,
            .text = text,
        });
      }
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::Response,
          .provider = provider(),
          .model = model(),
          .response = build_output(agent::AgentOutput{.text = text}),
      });
    }
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  int generate_calls_ = 0;
  int stream_calls_ = 0;
};

class StreamingReActReasoningModel final : public agent::ChatModelAdapter {
 public:
  StreamingReActReasoningModel()
      : ChatModelAdapter("fixture", "stream-react-reasoning", 0.0, 256, {"input.text", "reasoning"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    return build_output(agent::AgentOutput{
        .text = "Thought: summarize reasoning output.\nFinal Answer: reasoning visible",
        .reasoning = agent::ModelReasoning{.text = "reasoning plan"},
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    if (!on_event) {
      return;
    }
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::ResponseStart,
        .provider = provider(),
        .model = model(),
    });
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::ReasoningDelta,
        .provider = provider(),
        .model = model(),
        .delta = "reasoning ",
        .reasoning = "reasoning ",
    });
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::ReasoningDelta,
        .provider = provider(),
        .model = model(),
        .delta = "plan",
        .reasoning = "reasoning plan",
    });
    const std::string text = "Thought: summarize reasoning output.\nFinal Answer: reasoning visible";
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::TextDelta,
        .provider = provider(),
        .model = model(),
        .delta = text,
        .text = text,
    });
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::Response,
        .provider = provider(),
        .model = model(),
        .response = build_output(agent::AgentOutput{
            .text = text,
            .reasoning = agent::ModelReasoning{.text = "reasoning plan"},
        }),
    });
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  int generate_calls_ = 0;
  int stream_calls_ = 0;
};

class ReActReasoningProtocolLeakRecoveryModel final : public agent::ChatModelAdapter {
 public:
  ReActReasoningProtocolLeakRecoveryModel()
      : ChatModelAdapter("fixture", "react-reasoning-protocol-leak-recovery", 0.0, 256,
                         {"input.text", "reasoning"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    ++generate_calls_;
    for (auto message = params.messages.rbegin(); message != params.messages.rend(); ++message) {
      if (message->metadata.at("reasoningProtocolLeak").as_bool(false)) {
        saw_protocol_leak_observation_ =
            agent::extract_text_content(message->content).find(
                "ReAct protocol was emitted in reasoning channel") != std::string::npos;
        return build_output(agent::AgentOutput{
            .text = "Thought: re-emit in content\nFinal Answer: content answer",
            .reasoning = agent::ModelReasoning{.text = "brief reasoning"},
        });
      }
    }
    return build_output(agent::AgentOutput{
        .text = "",
        .reasoning = agent::ModelReasoning{
            .text = "Thought: misplaced protocol\nFinal Answer: reasoning answer",
        },
    });
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] bool saw_protocol_leak_observation() const noexcept {
    return saw_protocol_leak_observation_;
  }

 private:
  int generate_calls_ = 0;
  bool saw_protocol_leak_observation_ = false;
};

class ReActReasoningProtocolLeakRepeatModel final : public agent::ChatModelAdapter {
 public:
  ReActReasoningProtocolLeakRepeatModel()
      : ChatModelAdapter("fixture", "react-reasoning-protocol-leak-repeat", 0.0, 256,
                         {"input.text", "reasoning"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    return build_output(agent::AgentOutput{
        .text = "",
        .reasoning = agent::ModelReasoning{
            .text = "Thought: misplaced protocol\nFinal Answer: still in reasoning",
        },
    });
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }

 private:
  int generate_calls_ = 0;
};

class StreamingPlainFinalModel final : public agent::ChatModelAdapter {
 public:
  StreamingPlainFinalModel() : ChatModelAdapter("fixture", "stream-plain-final", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    return build_output(agent::AgentOutput{
        .text = plain_answer(),
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    std::string text;
    if (!on_event) {
      return;
    }
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::ResponseStart,
        .provider = provider(),
        .model = model(),
    });
    for (const auto& chunk : std::vector<std::string>{"你好！我是", "一个智能", "助手。"}) {
      text += chunk;
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::TextDelta,
          .provider = provider(),
          .model = model(),
          .delta = chunk,
          .text = text,
      });
    }
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::Response,
        .provider = provider(),
        .model = model(),
        .response = build_output(agent::AgentOutput{.text = text}),
    });
  }

  [[nodiscard]] static std::string plain_answer() { return "你好！我是一个智能助手。"; }
  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  int generate_calls_ = 0;
  int stream_calls_ = 0;
};

std::string utf8_stream_acceptance_text() {
  return "你好！我是 LumiAgent，一个专门协助处理本地工程和桌面任务的智能助手。请告诉我你需要完成什么任务。";
}

class StreamingReActUtf8BoundaryModel final : public agent::ChatModelAdapter {
 public:
  StreamingReActUtf8BoundaryModel()
      : ChatModelAdapter("fixture", "stream-react-utf8-boundary", 0.0, 512, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    return build_output(agent::AgentOutput{
        .text = "Thought: hidden reasoning\nFinal Answer: " + utf8_stream_acceptance_text(),
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    const auto answer = utf8_stream_acceptance_text();
    const auto first_char = answer.find("你");
    SMOKE_CHECK(first_char != std::string::npos);
    const auto split = first_char + 1;
    std::string text;
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
      for (const auto& chunk : std::vector<std::string>{
               "Thought: hidden reasoning\nFinal Answer: " + answer.substr(0, split),
               answer.substr(split),
           }) {
        text += chunk;
        on_event(agent::ModelStreamEvent{
            .type = agent::ModelStreamEventType::TextDelta,
            .provider = provider(),
            .model = model(),
            .delta = chunk,
            .text = text,
        });
      }
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::Response,
          .provider = provider(),
          .model = model(),
          .response = build_output(agent::AgentOutput{.text = text}),
      });
    }
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }
  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  int generate_calls_ = 0;
  int stream_calls_ = 0;
};

class StreamingReActActionModel final : public agent::ChatModelAdapter {
 public:
  explicit StreamingReActActionModel(std::function<int()> tool_call_count,
                                     std::vector<int>* tool_counts_during_first_stream)
      : ChatModelAdapter("fixture", "stream-react-action", 0.0, 256, {"input.text"}),
        tool_call_count_(std::move(tool_call_count)),
        tool_counts_during_first_stream_(tool_counts_during_first_stream) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{
        .text = "Thought: final\nFinal Answer: 4",
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    std::string text;
    const std::vector<std::string> chunks =
        stream_calls_ == 1
            ? std::vector<std::string>{
                  "Thought: use the calculator\nAct",
                  "ions:\n[{\"tool\":\"math.eval\",\"in",
                  "put\":{\"expression\":\"2+2\"}}]",
              }
            : std::vector<std::string>{
                  "Thought: observation is enough\nFinal ",
                  "Answer: ",
                  "4",
              };
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
      record_tool_count_during_first_stream();
      for (const auto& chunk : chunks) {
        text += chunk;
        on_event(agent::ModelStreamEvent{
            .type = agent::ModelStreamEventType::TextDelta,
            .provider = provider(),
            .model = model(),
            .delta = chunk,
            .text = text,
        });
        record_tool_count_during_first_stream();
      }
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::Response,
          .provider = provider(),
          .model = model(),
          .response = build_output(agent::AgentOutput{.text = text}),
      });
      record_tool_count_during_first_stream();
    }
  }

  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  void record_tool_count_during_first_stream() {
    if (stream_calls_ == 1 && tool_call_count_ && tool_counts_during_first_stream_) {
      tool_counts_during_first_stream_->push_back(tool_call_count_());
    }
  }

  std::function<int()> tool_call_count_;
  std::vector<int>* tool_counts_during_first_stream_ = nullptr;
  int stream_calls_ = 0;
};

class StreamingReActVisibleActionModel final : public agent::ChatModelAdapter {
 public:
  StreamingReActVisibleActionModel()
      : ChatModelAdapter("fixture", "stream-react-visible-action", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{
        .text = "Thought: final\nFinal Answer: 4",
    });
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    std::string text;
    const std::vector<std::string> chunks =
        stream_calls_ == 1
            ? std::vector<std::string>{
                  "Thought: explain first\nMessage: Let me che",
                  "ck that for you.\nActions:\n[{\"tool\":\"math.eval\",\"in",
                  "put\":{\"expression\":\"2+2\"}}]",
              }
            : std::vector<std::string>{
                  "Thought: observation is enough\nFinal ",
                  "Answer: ",
                  "4",
              };
    if (!on_event) {
      return;
    }
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::ResponseStart,
        .provider = provider(),
        .model = model(),
    });
    for (const auto& chunk : chunks) {
      text += chunk;
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::TextDelta,
          .provider = provider(),
          .model = model(),
          .delta = chunk,
          .text = text,
      });
    }
    on_event(agent::ModelStreamEvent{
        .type = agent::ModelStreamEventType::Response,
        .provider = provider(),
        .model = model(),
        .response = build_output(agent::AgentOutput{.text = text}),
    });
  }

  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  int stream_calls_ = 0;
};

class CapturingMultimodalModel final : public agent::ChatModelAdapter {
 public:
  CapturingMultimodalModel() : ChatModelAdapter("fixture", "multimodal-capture", 0.0, 256,
                                                {"input.text", "input.image"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    messages_ = params.messages;
    const auto text = react_prompt(params)
        ? std::string("Thought: respond to the multimodal input.\nFinal Answer: captured")
        : std::string("captured");
    return build_output(agent::AgentOutput{.text = text});
  }

  void stream(const agent::GenerateParams& params, StreamEventHandler on_event) override {
    messages_ = params.messages;
    const auto text = react_prompt(params)
        ? std::string("Thought: respond to the multimodal input.\nFinal Answer: captured stream")
        : std::string("captured stream");
    const auto response = build_output(agent::AgentOutput{.text = text});
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::TextDelta,
          .provider = provider(),
          .model = model(),
          .delta = text,
          .text = text,
      });
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::Response,
          .provider = provider(),
          .model = model(),
          .response = response,
      });
    }
  }

  [[nodiscard]] const std::vector<agent::AgentMessage>& messages() const noexcept { return messages_; }

 private:
  static bool react_prompt(const agent::GenerateParams& params) {
    return std::any_of(params.messages.begin(), params.messages.end(), [](const agent::AgentMessage& message) {
      return message.role == agent::MessageRole::System &&
             agent::extract_text_content(message.content).find("internal tool-use format") != std::string::npos;
    });
  }

  std::vector<agent::AgentMessage> messages_;
};

class FlakyBeforeStartStreamingModel final : public agent::ChatModelAdapter {
 public:
  FlakyBeforeStartStreamingModel() : ChatModelAdapter("fixture", "flaky-before-start-stream", 0.0, 128) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{.text = "Thought: ok.\nFinal Answer: ok"});
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    if (stream_calls_ == 1) {
      throw std::runtime_error("temporary stream failure");
    }
    const auto response = build_output(agent::AgentOutput{
        .text = "Thought: stream retry succeeded.\nFinal Answer: retried stream",
    });
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::Response,
          .response = response,
      });
    }
  }

  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }

 private:
  int stream_calls_ = 0;
};

class FailingAfterDeltaStreamingModel final : public agent::ChatModelAdapter {
 public:
  FailingAfterDeltaStreamingModel() : ChatModelAdapter("fixture", "failing-after-delta-stream", 0.0, 128) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{.text = "Thought: partial.\nFinal Answer: partial"});
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    ++stream_calls_;
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
      emitted_delta_ = true;
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::TextDelta,
          .provider = provider(),
          .model = model(),
          .delta = "Thought: partial.\nFinal Answer: partial",
          .text = "Thought: partial.\nFinal Answer: partial",
      });
    }
    throw std::runtime_error("stream failed after output");
  }

  [[nodiscard]] int stream_calls() const noexcept { return stream_calls_; }
  [[nodiscard]] bool emitted_delta() const noexcept { return emitted_delta_; }

 private:
  int stream_calls_ = 0;
  bool emitted_delta_ = false;
};

class CrashingStreamModel final : public agent::ChatModelAdapter {
 public:
  CrashingStreamModel() : ChatModelAdapter("fixture", "crashing-stream", 0.0, 128) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{.text = ""});
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
    }
    throw std::runtime_error("model adapter crashed mid-stream");
  }
};

class FrameworkErrorStreamModel final : public agent::ChatModelAdapter {
 public:
  FrameworkErrorStreamModel() : ChatModelAdapter("fixture", "framework-error-stream", 0.0, 128) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{.text = ""});
  }

  void stream(const agent::GenerateParams&, StreamEventHandler on_event) override {
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
    }
    throw agent::ProtocolError("provider sent malformed stream frame", "malformed-stream-frame");
  }
};

class CancellableStreamingModel final : public agent::ChatModelAdapter {
 public:
  explicit CancellableStreamingModel(std::atomic<bool>& started)
      : ChatModelAdapter("fixture", "cancellable-stream", 0.0, 128),
        started_(started) {}

  CancellableStreamingModel(std::atomic<bool>& started, std::atomic<bool>& saw_cancel)
      : ChatModelAdapter("fixture", "cancellable-stream", 0.0, 128),
        started_(started),
        saw_cancel_(&saw_cancel) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    return build_output(agent::AgentOutput{.text = "not used"});
  }

  void stream(const agent::GenerateParams& params, StreamEventHandler on_event) override {
    if (on_event) {
      on_event(agent::ModelStreamEvent{
          .type = agent::ModelStreamEventType::ResponseStart,
          .provider = provider(),
          .model = model(),
      });
    }
    started_.store(true);
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (params.cancellation && params.cancellation->cancelled()) {
        if (saw_cancel_) {
          saw_cancel_->store(true);
        }
        params.cancellation->throw_if_cancelled(agent::ExecutionTarget::Model);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("stream did not observe cancellation");
  }

 private:
  std::atomic<bool>& started_;
  std::atomic<bool>* saw_cancel_ = nullptr;
};

class EvalSettingsModel final : public agent::ChatModelAdapter {
 public:
  EvalSettingsModel() : ChatModelAdapter("fixture", "eval-settings-default", 0.1, 64, {"input.text", "reasoning"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    last_settings_ = params.settings;
    return build_output(agent::AgentOutput{
        .text = "Thought: preserve eval settings.\nFinal Answer: settings ok",
    });
  }

  [[nodiscard]] const agent::ModelSettings& last_settings() const noexcept { return last_settings_; }

 private:
  agent::ModelSettings last_settings_;
};

class SequentialTextModel final : public agent::ChatModelAdapter {
 public:
  explicit SequentialTextModel(std::vector<std::string> responses)
      : ChatModelAdapter("fixture", "sequential-text", 0.0, 128, {"input.text"}),
        responses_(std::move(responses)) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    const std::size_t index = responses_.empty()
                                  ? 0
                                  : std::min<std::size_t>(generate_calls_, responses_.size() - 1);
    ++generate_calls_;
    return build_output(agent::AgentOutput{.text = responses_.empty() ? "" : responses_[index]});
  }

  [[nodiscard]] int generate_calls() const noexcept { return static_cast<int>(generate_calls_); }

 private:
  std::vector<std::string> responses_;
  std::size_t generate_calls_ = 0;
};

class FailingRuntimeModel final : public agent::ChatModelAdapter {
 public:
  FailingRuntimeModel() : ChatModelAdapter("fixture", "failing-runtime", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    throw std::runtime_error("runtime model failed");
  }
};

class RetryingRuntimeModel final : public agent::ChatModelAdapter {
 public:
  RetryingRuntimeModel() : ChatModelAdapter("fixture", "retry-runtime", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    if (generate_calls_ == 1) {
      throw std::runtime_error("transient model failure");
    }
    return build_output(agent::AgentOutput{.text = "Thought: retry succeeded.\nFinal Answer: retried response"});
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }

 private:
  int generate_calls_ = 0;
};

class SlowRuntimeModel final : public agent::ChatModelAdapter {
 public:
  SlowRuntimeModel() : ChatModelAdapter("fixture", "slow-runtime", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams&) override {
    ++generate_calls_;
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
    return build_output(agent::AgentOutput{.text = "late response"});
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }

 private:
  int generate_calls_ = 0;
};

class CancellingToolModel final : public agent::ChatModelAdapter {
 public:
  CancellingToolModel() : ChatModelAdapter("fixture", "cancelling-tool-runtime", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    ++generate_calls_;
    for (const auto& message : params.messages) {
      const auto text = agent::extract_text_content(message.content);
      if (text.find("Observation:") != std::string::npos &&
          message.metadata.at("observation").as_bool(false) &&
          text.find("runtime.cancel") != std::string::npos) {
        return build_output(agent::AgentOutput{
            .text = "Thought: cancellation should have stopped the run.\nFinal Answer: unexpected continuation",
        });
      }
    }
    agent::AgentOutput response;
    response.finish_reason = "stop";
    response.text = "Thought: run the cancellation tool.\nActions:\n"
                    "[{\"tool\":\"runtime.cancel\",\"input\":{}}]";
    return build_output(response);
  }

  [[nodiscard]] int generate_calls() const noexcept { return generate_calls_; }

 private:
  int generate_calls_ = 0;
};

class CancellationAwareModel final : public agent::ChatModelAdapter {
 public:
  CancellationAwareModel() : ChatModelAdapter("fixture", "cancellation-aware", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    saw_cancellation_ = params.cancellation != nullptr;
    if (params.cancellation) {
      params.cancellation->throw_if_cancelled(agent::ExecutionTarget::Model);
    }
    return build_output(agent::AgentOutput{
        .text = "Thought: cancellation token reached the model.\nFinal Answer: model cancellation propagated",
    });
  }

  [[nodiscard]] bool saw_cancellation() const noexcept { return saw_cancellation_; }

 private:
  bool saw_cancellation_ = false;
};

class RuntimePlannerModel final : public agent::ChatModelAdapter {
 public:
  RuntimePlannerModel() : ChatModelAdapter("fixture", "runtime-planner", 0.0, 512, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string prompt_text;
    bool has_plan_message = false;
    for (const auto& message : params.messages) {
      const auto text = agent::extract_text_content(message.content);
      prompt_text += text;
      prompt_text += "\n";
      has_plan_message = has_plan_message ||
                         (message.metadata.at("source").as_string() == "planner" &&
                          text.find("Execution plan") != std::string::npos);
    }

    if (prompt_text.find("Return JSON only in this shape") != std::string::npos) {
      ++planning_calls_;
      SMOKE_CHECK(prompt_text.find("math.eval") != std::string::npos);
      return build_output(agent::AgentOutput{.text =
          "```json\n"
          "{\"goal\":\"Ship native planner\",\"steps\":["
          "{\"title\":\"Inspect requirements\",\"description\":\"Read task context\",\"toolName\":\"math.eval\"},"
          "{\"id\":\"deliver\",\"title\":\"Deliver answer\",\"dependsOn\":[\"step_1\"]}"
          "],\"notes\":[\"Keep plan concise\"]}\n"
          "```"});
    }

    SMOKE_CHECK(has_plan_message);
    SMOKE_CHECK(prompt_text.find("Inspect requirements") != std::string::npos);
    saw_plan_message_ = true;
    return build_output(agent::AgentOutput{
        .text = "Thought: answer using the injected execution plan.\nFinal Answer: planned response",
    });
  }

  [[nodiscard]] bool saw_plan_message() const noexcept { return saw_plan_message_; }
  [[nodiscard]] int planning_calls() const noexcept { return planning_calls_; }

 private:
  bool saw_plan_message_ = false;
  int planning_calls_ = 0;
};

class KnowledgeRunnerModel final : public agent::ChatModelAdapter {
 public:
  KnowledgeRunnerModel() : ChatModelAdapter("fixture", "knowledge-runner", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }
    SMOKE_CHECK(prompt_text.find("Knowledge base retrieval") != std::string::npos);
    SMOKE_CHECK(prompt_text.find("Runner Knowledge") != std::string::npos);
    saw_knowledge_context_ = true;
    return build_output(agent::AgentOutput{
        .text = "Thought: use the retrieved knowledge.\nFinal Answer: knowledge response",
    });
  }

  [[nodiscard]] bool saw_knowledge_context() const noexcept { return saw_knowledge_context_; }

 private:
  bool saw_knowledge_context_ = false;
};

class ContextServiceModel final : public agent::ChatModelAdapter {
 public:
  ContextServiceModel() : ChatModelAdapter("fixture", "context-service", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    for (const auto& message : params.messages) {
      const auto text = agent::extract_text_content(message.content);
      if (text.find("Observation:") != std::string::npos &&
          message.metadata.at("observation").as_bool(false) &&
          text.find("context.inspect") != std::string::npos) {
        agent::AgentOutput response;
        response.text = "Thought: answer from the context inspection.\nFinal Answer: context inspected: " + text;
        return build_output(std::move(response));
      }
    }
    agent::AgentOutput response;
    response.finish_reason = "stop";
    response.text = "Thought: inspect the runtime context.\nActions:\n"
                    "[{\"tool\":\"context.inspect\",\"input\":{}}]";
    return build_output(response);
  }
};

class WorkflowChildAgentModel final : public agent::ChatModelAdapter {
 public:
  WorkflowChildAgentModel() : ChatModelAdapter("fixture", "workflow-child", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    for (const auto& message : params.messages) {
      const auto text = agent::extract_text_content(message.content);
      if (text.find("Observation:") != std::string::npos &&
          message.metadata.at("observation").as_bool(false) &&
          text.find("child.context") != std::string::npos) {
        agent::AgentOutput response;
        response.text = "Thought: child context is available.\nFinal Answer: runner child context ok";
        return build_output(std::move(response));
      }
    }
    agent::AgentOutput response;
    response.finish_reason = "stop";
    response.text = "Thought: inspect child context.\nActions:\n"
                    "[{\"tool\":\"child.context\",\"input\":{}}]";
    return build_output(response);
  }
};

class SkillRuntimeModel final : public agent::ChatModelAdapter {
 public:
  SkillRuntimeModel() : ChatModelAdapter("fixture", "skill-runtime", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    calls_ += 1;
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }

    if (calls_ == 1) {
      SMOKE_CHECK(prompt_text.find("Available Anthropic-style skills") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("Audit auth module") != std::string::npos);
      agent::AgentOutput response;
      response.finish_reason = "stop";
      response.text = "Thought: use the allowed skill tool.\nActions:\n"
                      "[{\"tool\":\"fs.readText\",\"input\":{\"path\":\"README.md\"}}]";
      return build_output(response);
    }

    SMOKE_CHECK(prompt_text.find("allowed") != std::string::npos);
    return build_output(agent::AgentOutput{
        .text = "Thought: answer after the skill tool.\nFinal Answer: skill runtime ok",
    });
  }

 private:
  int calls_ = 0;
};

class SkillForkChildModel final : public agent::ChatModelAdapter {
 public:
  SkillForkChildModel() : ChatModelAdapter("fixture", "skill-fork-child", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }
    SMOKE_CHECK(prompt_text.find("Fork audit auth module") != std::string::npos);
    SMOKE_CHECK(prompt_text.find("User task:\nauth module") != std::string::npos);
    return build_output(agent::AgentOutput{
        .text = "Thought: summarize the forked audit.\nFinal Answer: child fork summary",
    });
  }
};

class SkillForkMainModel final : public agent::ChatModelAdapter {
 public:
  SkillForkMainModel() : ChatModelAdapter("fixture", "skill-fork-main", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    calls_ += 1;
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }
    if (calls_ == 1) {
      SMOKE_CHECK(prompt_text.find("Forked skill \"fork-audit\" result from agent \"reviewer\"") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("child fork summary") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("Fork audit auth module") == std::string::npos);
      agent::AgentOutput response;
      response.finish_reason = "stop";
      response.text = "Thought: inspect the fork result.\nActions:\n"
                      "[{\"tool\":\"fork.inspect\",\"input\":{}}]";
      return build_output(response);
    }
    SMOKE_CHECK(prompt_text.find("fork-service-ok") != std::string::npos);
    return build_output(agent::AgentOutput{
        .text = "Thought: answer after consuming the fork.\nFinal Answer: main consumed fork",
    });
  }

 private:
  int calls_ = 0;
};

class AutonomousRunnerModel final : public agent::ChatModelAdapter {
 public:
  AutonomousRunnerModel() : ChatModelAdapter("fixture", "autonomous-runner", 0.0, 512, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }
    prompts_.push_back(prompt_text);

    if (prompt_text.find("Create a concise execution plan") != std::string::npos) {
      SMOKE_CHECK(prompt_text.find("Goal: Ship model-led report") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("run=agent-auto-runner") != std::string::npos);
      return build_output(agent::AgentOutput{.text =
          "Thought: build a concise execution plan.\nFinal Answer: Plan:\n{\"summary\":\"model plan\",\"steps\":["
          "{\"id\":\"draft\",\"title\":\"Draft report\",\"objective\":\"Write a draft\",\"input\":{\"section\":\"draft\"}},"
          "{\"id\":\"final\",\"title\":\"Finish report\",\"objective\":\"Finalize the report\",\"dependsOn\":[\"draft\"]}"
          "]}"});
    }

    if (prompt_text.find("Current step: Finish report") != std::string::npos) {
      SMOKE_CHECK(prompt_text.find("Previous completed steps") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("draft-output") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("step=final") != std::string::npos);
      return build_output(agent::AgentOutput{.text = "Thought: finish the report.\nFinal Answer: final-output"});
    }

    if (prompt_text.find("Current step: Draft report") != std::string::npos) {
      SMOKE_CHECK(prompt_text.find("Step input") != std::string::npos);
      SMOKE_CHECK(prompt_text.find("step=draft") != std::string::npos);
      return build_output(agent::AgentOutput{.text = "Thought: draft the report.\nFinal Answer: draft-output"});
    }

    return build_output(agent::AgentOutput{
        .text = "Thought: unexpected autonomous prompt.\nFinal Answer: unexpected autonomous prompt",
    });
  }

  [[nodiscard]] const std::vector<std::string>& prompts() const noexcept { return prompts_; }

 private:
  std::vector<std::string> prompts_;
};

class ServerRuntimeModel final : public agent::ChatModelAdapter {
 public:
  ServerRuntimeModel() : ChatModelAdapter("fixture", "server-runtime", 0.0, 256, {"input.text", "reasoning"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    last_settings_ = params.settings;
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }
    prompts_.push_back(prompt_text);
    SMOKE_CHECK(prompt_text.find("server-source") != std::string::npos);
    const auto input_pos = prompt_text.rfind("server-input");
    SMOKE_CHECK(input_pos != std::string::npos);
    return build_output(agent::AgentOutput{
        .text = "Thought: respond to the server request.\nFinal Answer: server-ok:" +
            prompt_text.substr(input_pos),
        .raw = agent::Value::object({
            {"usage", agent::Value::object({
                {"prompt_tokens", 100},
                {"completion_tokens", 50},
                {"total_tokens", 150},
            })},
        }),
    });
  }

  [[nodiscard]] const std::vector<std::string>& prompts() const noexcept { return prompts_; }
  [[nodiscard]] const agent::ModelSettings& last_settings() const noexcept { return last_settings_; }

 private:
  std::vector<std::string> prompts_;
  agent::ModelSettings last_settings_;
};

class GovernanceModel final : public agent::ChatModelAdapter {
 public:
  GovernanceModel() : ChatModelAdapter("fixture", "governance-runtime", 0.0, 256, {"input.text"}) {}

  agent::AgentOutput generate(const agent::GenerateParams& params) override {
    std::string prompt_text;
    for (const auto& message : params.messages) {
      prompt_text += agent::extract_text_content(message.content);
      prompt_text += "\n";
    }
    if (prompt_text.find("schema-governance") != std::string::npos) {
      return build_output(agent::AgentOutput{
          .text = "Thought: return governed JSON.\nFinal Answer: {\"message\":\"contact jane@example.com\"}",
      });
    }
    return build_output(agent::AgentOutput{
        .text = "Thought: return plain governed output.\nFinal Answer: plain output",
    });
  }
};

class FixtureOCRProvider final : public agent::OCRProvider {
 public:
  FixtureOCRProvider() {
    metadata_.name = "fixture-ocr";
    metadata_.tier = "portable";
    metadata_.title = "Fixture OCR";
  }

  const agent::OCRProviderMetadata& metadata() const override {
    return metadata_;
  }

  agent::OCRResult recognize(const agent::OCRRequest& request) override {
    SMOKE_CHECK(!request.images.empty());
    agent::OCRResult result;
    result.text = request.images.size() == 1 && request.images.front().page_number == 1
                      ? "ocr page one"
                      : "ocr pages";
    result.regions.push_back(agent::OCRRegion{
        .text = result.text,
        .confidence = 0.9,
        .page_number = request.images.front().page_number,
    });
    return result;
  }

 private:
  agent::OCRProviderMetadata metadata_;
};

class FixturePdfRasterizer final : public agent::DocumentRasterizer {
 public:
  FixturePdfRasterizer() {
    metadata_.name = "fixture-pdf-rasterizer";
    metadata_.tier = "portable";
  }

  const agent::DocumentRasterizerMetadata& metadata() const override {
    return metadata_;
  }

  bool supports(const agent::ResolvedMedia& document) const override {
    return document.mime_type == "application/pdf";
  }

  std::vector<agent::RasterizedDocumentPage> rasterize(
      const agent::RasterizeDocumentRequest& request) override {
    agent::MediaSource page_source;
    page_source.kind = agent::MediaSourceKind::Inline;
    page_source.data = "iVBORw0KGgo=";
    page_source.mime_type = "image/png";
    page_source.filename = "page-1.png";

    agent::ResolvedMedia page;
    page.source = page_source;
    page.mime_type = "image/png";
    page.filename = "page-1.png";
    page.bytes = {1, 2, 3};
    return {agent::RasterizedDocumentPage{
        .page_number = 1,
        .media = page,
        .source = request.source,
    }};
  }

 private:
  agent::DocumentRasterizerMetadata metadata_;
};

class RecordingTelemetrySpan final : public agent::OpenTelemetrySpan {
 public:
  explicit RecordingTelemetrySpan(std::vector<std::string>& calls) : calls_(calls) {}

  void end(agent::TraceStatus status) override {
    calls_.push_back("end:" + agent::to_string(status));
  }

 private:
  std::vector<std::string>& calls_;
};

class RecordingTelemetryBridge final : public agent::OpenTelemetryBridge {
 public:
  std::vector<std::string> calls;

  void log(const agent::StructuredLogRecord& record) override {
    calls.push_back("log:" + record.message);
  }

  void counter(const std::string& name, double, const agent::Value&) override {
    calls.push_back("counter:" + name);
  }

  std::unique_ptr<agent::OpenTelemetrySpan> start_span(const std::string& name,
                                                       const agent::Value& attributes) override {
    calls.push_back("span:" + name + ":" + attributes.at("parentSpanId").as_string());
    return std::make_unique<RecordingTelemetrySpan>(calls);
  }
};

int run_mcp_fixture_stdio_server() {
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    const auto message = agent::parse_json(line);
    const auto method = message.at("method").as_string();
    const auto id = message.at("id").as_string();
    if (method == "notifications/initialized") {
      continue;
    }
    if (method == "initialize") {
      std::cout << agent::json_rpc_success_to_value(id, agent::Value::object({
          {"protocolVersion", agent::MCP_PROTOCOL_VERSION},
          {"capabilities", agent::Value::object({{"tools", agent::Value::object({{"listChanged", false}})}})},
          {"serverInfo", agent::Value::object({{"name", "stdio-fixture"}, {"version", "1.0.0"}})},
      })).stringify() << "\n" << std::flush;
      continue;
    }
    if (method == "tools/list") {
      std::cout << agent::json_rpc_success_to_value(id, agent::Value::object({
          {"tools", agent::Value::array({
              agent::Value::object({
                  {"name", "echo"},
                  {"description", "Echo input from stdio"},
                  {"inputSchema", agent::Value::object({
                      {"type", "object"},
                      {"properties", agent::Value::object({
                          {"text", agent::Value::object({{"type", "string"}})},
                      })},
                  })},
              }),
          })},
      })).stringify() << "\n" << std::flush;
      continue;
    }
    if (method == "tools/call") {
      const std::string prefix = std::getenv("MCP_FIXTURE_PREFIX") ? std::getenv("MCP_FIXTURE_PREFIX") : "stdio";
      const auto text = message.at("params").at("arguments").at("text").as_string();
      std::cout << agent::json_rpc_success_to_value(id, agent::Value::object({
          {"content", agent::Value::array({
              agent::Value::object({{"type", "text"}, {"text", prefix + ":" + text}}),
          })},
          {"structuredContent", agent::Value::object({{"ok", true}, {"prefix", prefix}})},
      })).stringify() << "\n" << std::flush;
      continue;
    }
    std::cout << agent::json_rpc_error_to_value(id, -32601, "Method not found").stringify()
              << "\n" << std::flush;
  }
  return 0;
}

int run_smoke(int argc, char** argv) {
  // Keep the smoke scenario split by domain while preserving shared function scope.
#include "smoke_chunks/01_core_tools_and_server.inc"
#include "smoke_chunks/02_config_runtime_and_permissions.inc"
#include "smoke_chunks/03_knowledge_memory_and_indexes.inc"
#include "smoke_chunks/04_web_media_and_agents.inc"
#include "smoke_chunks/05_models_providers_and_streaming.inc"
#include "smoke_chunks/06_realtime_tasks_and_skills.inc"
#include "smoke_chunks/07_mcp_workflow_and_orchestration.inc"
#include "smoke_chunks/08_replay_eval_and_cancellation.inc"
#include "smoke_chunks/09_harness_improvements.inc"
  return 0;
}

int main(int argc, char** argv) {
  try {
    return run_smoke(argc, argv);
  } catch (const agent::SchemaValidationError& error) {
    std::cerr << error.what() << "\n";
    for (const auto& issue : error.issues()) {
      std::cerr << issue.path << ": " << issue.message << "\n";
    }
    return 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    return 1;
  }
}
