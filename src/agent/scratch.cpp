#include "agent/scratch.hpp"
#include "detail/helpers.hpp"

#include <utility>

namespace agent {

namespace {

std::map<std::string, Value> filter_by_prefix(const std::map<std::string, Value>& source,
                                              const std::string& prefix) {
  if (prefix.empty()) {
    return source;
  }
  std::map<std::string, Value> filtered;
  for (const auto& [key, value] : source) {
    if (key.rfind(prefix, 0) == 0) {
      filtered.emplace(key, value);
    }
  }
  return filtered;
}

}  // namespace

// ---------------------------------------------------------------------------
// InMemoryScratchStore
// ---------------------------------------------------------------------------

std::optional<Value> InMemoryScratchStore::get(const std::string& session,
                                                const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto session_it = sessions_.find(session);
  if (session_it == sessions_.end()) {
    return std::nullopt;
  }
  const auto key_it = session_it->second.find(key);
  if (key_it == session_it->second.end()) {
    return std::nullopt;
  }
  return key_it->second;
}

void InMemoryScratchStore::set(const std::string& session, const std::string& key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_[session][key] = std::move(value);
}

bool InMemoryScratchStore::remove(const std::string& session, const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto session_it = sessions_.find(session);
  if (session_it == sessions_.end()) {
    return false;
  }
  return session_it->second.erase(key) > 0;
}

std::map<std::string, Value> InMemoryScratchStore::entries(const std::string& session,
                                                            const std::string& prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto session_it = sessions_.find(session);
  if (session_it == sessions_.end()) {
    return {};
  }
  return filter_by_prefix(session_it->second, prefix);
}

void InMemoryScratchStore::clear(const std::string& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  sessions_.erase(session);
}

// ---------------------------------------------------------------------------
// FileScratchStore
// ---------------------------------------------------------------------------

FileScratchStore::FileScratchStore(std::filesystem::path base_dir)
    : base_dir_(std::move(base_dir)) {}

FileScratchStore::FileScratchStore(FileScratchStoreConfig config)
    : FileScratchStore(std::move(config.base_dir)) {}

std::filesystem::path FileScratchStore::session_path(const std::string& session) const {
  return base_dir_ / (encode_uri_component(session) + ".json");
}

std::map<std::string, Value> FileScratchStore::read_session(const std::string& session) const {
  const auto path = session_path(session);
  if (!std::filesystem::exists(path)) {
    return {};
  }
  const auto raw = read_json_file(path);
  std::map<std::string, Value> data;
  if (raw.is_object()) {
    for (const auto& [key, value] : raw.as_object()) {
      data.emplace(key, value);
    }
  }
  return data;
}

void FileScratchStore::write_session(const std::string& session,
                                      const std::map<std::string, Value>& data) const {
  Value::Object object;
  for (const auto& [key, value] : data) {
    object.emplace(key, value);
  }
  write_json_file(session_path(session), Value(object));
}

std::optional<Value> FileScratchStore::get(const std::string& session, const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto data = read_session(session);
  const auto it = data.find(key);
  if (it == data.end()) {
    return std::nullopt;
  }
  return it->second;
}

void FileScratchStore::set(const std::string& session, const std::string& key, Value value) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto data = read_session(session);
  data[key] = std::move(value);
  write_session(session, data);
}

bool FileScratchStore::remove(const std::string& session, const std::string& key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto data = read_session(session);
  const auto erased = data.erase(key) > 0;
  if (erased) {
    write_session(session, data);
  }
  return erased;
}

std::map<std::string, Value> FileScratchStore::entries(const std::string& session,
                                                        const std::string& prefix) {
  std::lock_guard<std::mutex> lock(mutex_);
  return filter_by_prefix(read_session(session), prefix);
}

void FileScratchStore::clear(const std::string& session) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::filesystem::remove(session_path(session));
}

}  // namespace agent
