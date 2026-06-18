#pragma once

#include "agent/core.hpp"

namespace agent {

struct RunTranscriptEntry {
  std::string id;
  std::uint64_t sequence = 0;
  std::string run_id;
  std::string kind;
  Value payload = Value::object({});
  Value metadata = Value::object({});
  std::string timestamp;
};

struct RunTranscriptAppendInput {
  std::string run_id;
  std::string kind;
  Value payload = Value::object({});
  Value metadata = Value::object({});
  std::string timestamp;
};

struct RunTranscriptListOptions {
  std::string run_id;
  std::string kind;
};

Value run_transcript_entry_to_value(const RunTranscriptEntry& entry);
RunTranscriptEntry run_transcript_entry_from_value(const Value& value);

class RunTranscript {
 public:
  virtual ~RunTranscript() = default;
  virtual RunTranscriptEntry append(RunTranscriptAppendInput input) = 0;
  [[nodiscard]] virtual std::vector<RunTranscriptEntry> list(RunTranscriptListOptions options = {}) const = 0;
  virtual void clear() {}
};

class InMemoryRunTranscript : public RunTranscript {
 public:
  RunTranscriptEntry append(RunTranscriptAppendInput input) override;
  [[nodiscard]] std::vector<RunTranscriptEntry> list(RunTranscriptListOptions options = {}) const override;
  void clear() override;

 private:
  mutable std::mutex mutex_;
  std::vector<RunTranscriptEntry> entries_;
  std::uint64_t next_sequence_ = 1;
};

struct FileRunTranscriptConfig {
  std::filesystem::path file_path;
};

class FileRunTranscript : public RunTranscript {
 public:
  explicit FileRunTranscript(std::filesystem::path file_path);
  explicit FileRunTranscript(FileRunTranscriptConfig config);
  RunTranscriptEntry append(RunTranscriptAppendInput input) override;
  [[nodiscard]] std::vector<RunTranscriptEntry> list(RunTranscriptListOptions options = {}) const override;
  void clear() override;

 private:
  void ensure_loaded() const;
  [[nodiscard]] std::vector<RunTranscriptEntry> read_entries() const;

  std::filesystem::path file_path_;
  mutable std::mutex mutex_;
  mutable bool loaded_ = false;
  mutable std::uint64_t next_sequence_ = 1;
};

}  // namespace agent
