#pragma once

#include "agent/core.hpp"

namespace agent {

struct RunReplayManifest {
  int version = 1;
  std::string run_id;
  std::string session_id;
  std::string agent_id;
  std::string started_at;
  std::string finished_at;
  int duration_ms = 0;
  std::string status = "ok";
  std::size_t event_count = 0;
  std::size_t tool_call_count = 0;
  std::string input_file = "input.json";
  std::string result_file;
  std::string events_file = "events.json";
  std::string html_file = "replay.html";
  std::string error;
  Value metadata = Value::object({});
};

struct LoadedRunReplay {
  std::string dir_path;
  RunReplayManifest manifest;
  Value input;
  Value result;
  std::vector<Value> events;
};

struct WriteRunReplayOptions {
  std::filesystem::path base_dir;
  std::string run_id;
  std::string session_id;
  std::string agent_id;
  Value input;
  Value result;
  std::vector<Value> events;
  std::string started_at;
  std::string finished_at;
  std::string error;
  Value metadata = Value::object({});
};

struct WriteRunReplayResult {
  std::filesystem::path dir_path;
  std::filesystem::path manifest_path;
  std::filesystem::path html_path;
  RunReplayManifest manifest;
};

std::string render_run_replay_html(const LoadedRunReplay& replay);
Value run_replay_manifest_to_value(const RunReplayManifest& manifest);
RunReplayManifest run_replay_manifest_from_value(const Value& value);
LoadedRunReplay load_run_replay(const std::filesystem::path& run_dir);
std::vector<std::filesystem::path> list_session_replays(const std::filesystem::path& base_dir,
                                                        const std::string& session_id);
WriteRunReplayResult write_run_replay(const WriteRunReplayOptions& options);
RunReplayManifest write_run_replay(const std::filesystem::path& base_dir, std::string session_id,
                                   Value input, Value result = {}, std::vector<Value> events = {},
                                   std::string run_id = {}, std::string agent_id = {},
                                   std::string error = {}, Value metadata = Value::object({}));

}  // namespace agent
