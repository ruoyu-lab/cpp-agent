#pragma once

#include "agent/core.hpp"

#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace agent {

// Per-session key→Value storage shared by builtin `scratch.*` and `todo.*`
// tools (and any user-defined tool that wants short-term agent memory).
// Hosts inject custom implementations to add persistence (e.g. SQLite,
// Redis) via AgentRunnerConfig::scratch_store or kToolServiceScratchStore.
class ScratchStore {
 public:
  virtual ~ScratchStore() = default;
  virtual std::optional<Value> get(const std::string& session, const std::string& key) = 0;
  virtual void set(const std::string& session, const std::string& key, Value value) = 0;
  virtual bool remove(const std::string& session, const std::string& key) = 0;
  // Returns all entries for `session` filtered by `prefix` (empty prefix → all).
  // Internal-key filtering (e.g. `__todo:*` exclusion) is policy at the tool
  // layer — backends return entries verbatim.
  virtual std::map<std::string, Value> entries(const std::string& session,
                                                const std::string& prefix = "") = 0;
  virtual void clear(const std::string& session) = 0;
};

// Default: thread-safe, process-local, lost on restart.
class InMemoryScratchStore final : public ScratchStore {
 public:
  std::optional<Value> get(const std::string& session, const std::string& key) override;
  void set(const std::string& session, const std::string& key, Value value) override;
  bool remove(const std::string& session, const std::string& key) override;
  std::map<std::string, Value> entries(const std::string& session,
                                        const std::string& prefix = "") override;
  void clear(const std::string& session) override;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::map<std::string, Value>> sessions_;
};

// File-backed: persists each session as <base_dir>/{url-encoded session_id}.json.
// Survives process restart without requiring any external dependency. Mirrors
// the FileSessionStore / FileApprovalStore pattern.
struct FileScratchStoreConfig {
  std::filesystem::path base_dir;
};

class FileScratchStore final : public ScratchStore {
 public:
  explicit FileScratchStore(std::filesystem::path base_dir);
  explicit FileScratchStore(FileScratchStoreConfig config);
  std::optional<Value> get(const std::string& session, const std::string& key) override;
  void set(const std::string& session, const std::string& key, Value value) override;
  bool remove(const std::string& session, const std::string& key) override;
  std::map<std::string, Value> entries(const std::string& session,
                                        const std::string& prefix = "") override;
  void clear(const std::string& session) override;

 private:
  // Internal helpers — read/write the per-session JSON file under a mutex.
  std::filesystem::path session_path(const std::string& session) const;
  std::map<std::string, Value> read_session(const std::string& session) const;
  void write_session(const std::string& session, const std::map<std::string, Value>& data) const;

  std::filesystem::path base_dir_;
  mutable std::mutex mutex_;
};

}  // namespace agent
