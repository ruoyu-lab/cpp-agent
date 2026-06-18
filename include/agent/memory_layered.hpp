#pragma once

#include "agent/model.hpp"

namespace agent {

struct FileMemoryLayerConfig {
  std::string id;
  std::string file_name;
  std::string title;
};

struct FileLayeredMemoryConfig {
  std::filesystem::path base_dir;
  std::vector<FileMemoryLayerConfig> layers;
  std::string default_layer = "default";
  std::string context_title = "Layered file memory";
};

struct LayeredMemoryRecord {
  std::string id;
  std::string layer;
  std::string content;
  Value metadata = Value::object({});
  std::string created_at;
  std::string updated_at;
};

struct LayeredMemoryRememberInput {
  std::string id;
  std::string layer;
  std::string content;
  Value metadata = Value::object({});
};

struct LayeredMemorySearchOptions {
  std::vector<std::string> layers;
  std::size_t limit = 8;
};

struct LayeredMemorySearchHit {
  LayeredMemoryRecord record;
  double score = 0;
};

struct LayeredMemoryContextResult {
  std::vector<LayeredMemorySearchHit> hits;
  std::optional<AgentMessage> message;
};

Value layered_memory_record_to_value(const LayeredMemoryRecord& record);
LayeredMemoryRecord layered_memory_record_from_value(const Value& value);

class FileLayeredMemoryStore {
 public:
  explicit FileLayeredMemoryStore(FileLayeredMemoryConfig config);
  void register_layer(FileMemoryLayerConfig layer);
  LayeredMemoryRecord remember(LayeredMemoryRememberInput input);
  [[nodiscard]] std::vector<LayeredMemoryRecord> list_layer(std::string layer = {}) const;
  [[nodiscard]] std::vector<std::string> list_layers() const;
  [[nodiscard]] std::vector<LayeredMemorySearchHit> search(const std::string& query,
                                                           LayeredMemorySearchOptions options = {}) const;
  [[nodiscard]] std::optional<AgentMessage> create_context_message(
      const std::vector<LayeredMemorySearchHit>& hits) const;
  [[nodiscard]] LayeredMemoryContextResult build_context_message(const std::string& query,
                                                                 LayeredMemorySearchOptions options = {}) const;
  void clear(std::string layer = {});

 private:
  [[nodiscard]] std::filesystem::path layer_path(const std::string& layer) const;
  [[nodiscard]] std::vector<LayeredMemoryRecord> read_layer_file(const std::string& layer) const;
  void write_layer_file(const std::string& layer,
                        const std::vector<LayeredMemoryRecord>& records) const;

  std::filesystem::path base_dir_;
  std::string default_layer_;
  std::string context_title_;
  mutable std::mutex mutex_;
  std::map<std::string, FileMemoryLayerConfig> layers_;
};

}  // namespace agent
