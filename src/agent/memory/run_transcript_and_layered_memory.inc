namespace {

bool transcript_matches(const RunTranscriptEntry& entry, const RunTranscriptListOptions& options) {
  if (!options.run_id.empty() && entry.run_id != options.run_id) {
    return false;
  }
  if (!options.kind.empty() && entry.kind != options.kind) {
    return false;
  }
  return true;
}

RunTranscriptEntry make_transcript_entry(RunTranscriptAppendInput input, std::uint64_t sequence) {
  return RunTranscriptEntry{
      .id = generate_uuid(),
      .sequence = sequence,
      .run_id = std::move(input.run_id),
      .kind = std::move(input.kind),
      .payload = std::move(input.payload),
      .metadata = std::move(input.metadata),
      .timestamp = input.timestamp.empty() ? now_iso8601() : std::move(input.timestamp),
  };
}

std::string normalize_layer_id(std::string layer) {
  layer = trim_copy(std::move(layer));
  if (layer.empty()) {
    throw ConfigurationError("File layered memory layer id is required.");
  }
  return layer;
}

std::string default_layer_file_name(const std::string& layer) {
  return encode_uri_component(layer) + ".json";
}

std::string lower_ascii_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

double layered_record_score(const std::vector<std::string>& terms,
                            const LayeredMemoryRecord& record) {
  if (terms.empty()) {
    return 1.0;
  }
  const std::string haystack =
      lower_ascii_copy(record.content + "\n" + record.metadata.stringify(0));
  double score = 0.0;
  for (const auto& term : terms) {
    if (!term.empty() && haystack.find(term) != std::string::npos) {
      score += 1.0;
    }
  }
  return score;
}

}  // namespace

Value run_transcript_entry_to_value(const RunTranscriptEntry& entry) {
  return Value::object({
      {"id", entry.id},
      {"sequence", static_cast<long long>(entry.sequence)},
      {"runId", entry.run_id},
      {"kind", entry.kind},
      {"payload", entry.payload},
      {"metadata", entry.metadata},
      {"timestamp", entry.timestamp},
  });
}

RunTranscriptEntry run_transcript_entry_from_value(const Value& value) {
  return RunTranscriptEntry{
      .id = value.at("id").as_string(),
      .sequence = static_cast<std::uint64_t>(std::max<long long>(0, value.at("sequence").as_integer())),
      .run_id = value.at("runId").as_string(),
      .kind = value.at("kind").as_string(),
      .payload = value.at("payload"),
      .metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({}),
      .timestamp = value.at("timestamp").as_string(),
  };
}

RunTranscriptEntry InMemoryRunTranscript::append(RunTranscriptAppendInput input) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto entry = make_transcript_entry(std::move(input), next_sequence_++);
  entries_.push_back(entry);
  return entry;
}

std::vector<RunTranscriptEntry> InMemoryRunTranscript::list(RunTranscriptListOptions options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<RunTranscriptEntry> output;
  for (const auto& entry : entries_) {
    if (transcript_matches(entry, options)) {
      output.push_back(entry);
    }
  }
  return output;
}

void InMemoryRunTranscript::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
  next_sequence_ = 1;
}

FileRunTranscript::FileRunTranscript(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

FileRunTranscript::FileRunTranscript(FileRunTranscriptConfig config)
    : FileRunTranscript(std::move(config.file_path)) {}

void FileRunTranscript::ensure_loaded() const {
  if (loaded_) {
    return;
  }
  std::uint64_t max_sequence = 0;
  for (const auto& entry : read_entries()) {
    max_sequence = std::max(max_sequence, entry.sequence);
  }
  next_sequence_ = max_sequence + 1;
  loaded_ = true;
}

std::vector<RunTranscriptEntry> FileRunTranscript::read_entries() const {
  std::vector<RunTranscriptEntry> output;
  if (!std::filesystem::exists(file_path_)) {
    return output;
  }
  std::ifstream input(file_path_, std::ios::binary);
  if (!input) {
    throw ConfigurationError("Unable to read run transcript: " + file_path_.string());
  }
  std::string line;
  while (std::getline(input, line)) {
    if (trim_copy(line).empty()) {
      continue;
    }
    output.push_back(run_transcript_entry_from_value(parse_json(line)));
  }
  return output;
}

RunTranscriptEntry FileRunTranscript::append(RunTranscriptAppendInput input) {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  auto entry = make_transcript_entry(std::move(input), next_sequence_++);
  std::filesystem::create_directories(file_path_.parent_path());
  std::ofstream output(file_path_, std::ios::binary | std::ios::app);
  if (!output) {
    throw ConfigurationError("Unable to append run transcript: " + file_path_.string());
  }
  output << run_transcript_entry_to_value(entry).stringify(0) << "\n";
  return entry;
}

std::vector<RunTranscriptEntry> FileRunTranscript::list(RunTranscriptListOptions options) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ensure_loaded();
  std::vector<RunTranscriptEntry> output;
  for (const auto& entry : read_entries()) {
    if (transcript_matches(entry, options)) {
      output.push_back(entry);
    }
  }
  return output;
}

void FileRunTranscript::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::filesystem::remove(file_path_);
  loaded_ = true;
  next_sequence_ = 1;
}

Value layered_memory_record_to_value(const LayeredMemoryRecord& record) {
  return Value::object({
      {"id", record.id},
      {"layer", record.layer},
      {"content", record.content},
      {"metadata", record.metadata},
      {"createdAt", record.created_at},
      {"updatedAt", record.updated_at},
  });
}

LayeredMemoryRecord layered_memory_record_from_value(const Value& value) {
  return LayeredMemoryRecord{
      .id = value.at("id").as_string(),
      .layer = value.at("layer").as_string(),
      .content = value.at("content").as_string(),
      .metadata = value.at("metadata").is_object() ? value.at("metadata") : Value::object({}),
      .created_at = value.at("createdAt").as_string(),
      .updated_at = value.at("updatedAt").as_string(),
  };
}

FileLayeredMemoryStore::FileLayeredMemoryStore(FileLayeredMemoryConfig config)
    : base_dir_(std::move(config.base_dir)),
      default_layer_(normalize_layer_id(std::move(config.default_layer))),
      context_title_(std::move(config.context_title)) {
  register_layer(FileMemoryLayerConfig{.id = default_layer_});
  for (auto& layer : config.layers) {
    register_layer(std::move(layer));
  }
}

void FileLayeredMemoryStore::register_layer(FileMemoryLayerConfig layer) {
  layer.id = normalize_layer_id(std::move(layer.id));
  std::lock_guard<std::mutex> lock(mutex_);
  layers_[layer.id] = std::move(layer);
}

std::filesystem::path FileLayeredMemoryStore::layer_path(const std::string& layer) const {
  const auto id = normalize_layer_id(layer);
  auto found = layers_.find(id);
  const std::string file_name =
      found != layers_.end() && !found->second.file_name.empty()
          ? found->second.file_name
          : default_layer_file_name(id);
  return base_dir_ / file_name;
}

std::vector<LayeredMemoryRecord> FileLayeredMemoryStore::read_layer_file(const std::string& layer) const {
  const auto id = normalize_layer_id(layer);
  const auto path = layer_path(id);
  if (!std::filesystem::exists(path)) {
    return {};
  }
  const auto raw = read_json_file(path);
  std::vector<LayeredMemoryRecord> records;
  for (const auto& item : raw.at("records").as_array()) {
    auto record = layered_memory_record_from_value(item);
    if (record.layer == id) {
      records.push_back(std::move(record));
    }
  }
  return records;
}

void FileLayeredMemoryStore::write_layer_file(const std::string& layer,
                                              const std::vector<LayeredMemoryRecord>& records) const {
  Value::Array values;
  values.reserve(records.size());
  for (const auto& record : records) {
    values.push_back(layered_memory_record_to_value(record));
  }
  write_json_file(layer_path(layer), Value::object({{"records", Value(std::move(values))}}));
}

LayeredMemoryRecord FileLayeredMemoryStore::remember(LayeredMemoryRememberInput input) {
  auto layer = normalize_layer_id(input.layer.empty() ? default_layer_ : std::move(input.layer));
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!layers_.contains(layer)) {
      layers_[layer] = FileMemoryLayerConfig{.id = layer};
    }
  }
  auto records = read_layer_file(layer);
  const auto now = now_iso8601();
  LayeredMemoryRecord record{
      .id = input.id.empty() ? generate_uuid() : std::move(input.id),
      .layer = layer,
      .content = std::move(input.content),
      .metadata = std::move(input.metadata),
      .created_at = now,
      .updated_at = now,
  };
  records.erase(std::remove_if(records.begin(), records.end(), [&](const auto& item) {
    if (item.id == record.id) {
      record.created_at = item.created_at;
      return true;
    }
    return false;
  }), records.end());
  records.push_back(record);
  write_layer_file(layer, records);
  return record;
}

std::vector<LayeredMemoryRecord> FileLayeredMemoryStore::list_layer(std::string layer) const {
  return read_layer_file(layer.empty() ? default_layer_ : std::move(layer));
}

std::vector<std::string> FileLayeredMemoryStore::list_layers() const {
  std::set<std::string> ids;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, _] : layers_) {
      ids.insert(id);
    }
  }
  if (std::filesystem::exists(base_dir_)) {
    for (const auto& entry : std::filesystem::directory_iterator(base_dir_)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") {
        continue;
      }
      const auto stem = entry.path().stem().string();
      if (auto decoded = decode_uri_component(stem)) {
        ids.insert(*decoded);
      }
    }
  }
  return {ids.begin(), ids.end()};
}

std::vector<LayeredMemorySearchHit> FileLayeredMemoryStore::search(
    const std::string& query,
    LayeredMemorySearchOptions options) const {
  std::vector<std::string> terms;
  for (auto term : tokenize(lower_ascii_copy(query))) {
    if (!term.empty()) {
      terms.push_back(std::move(term));
    }
  }
  auto layers = options.layers.empty() ? list_layers() : std::move(options.layers);
  std::vector<LayeredMemorySearchHit> hits;
  for (const auto& layer : layers) {
    for (auto record : read_layer_file(layer)) {
      const auto score = layered_record_score(terms, record);
      if (score > 0.0) {
        hits.push_back(LayeredMemorySearchHit{std::move(record), score});
      }
    }
  }
  std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
    if (left.score != right.score) {
      return left.score > right.score;
    }
    return left.record.updated_at > right.record.updated_at;
  });
  if (options.limit > 0 && hits.size() > options.limit) {
    hits.resize(options.limit);
  }
  return hits;
}

std::optional<AgentMessage> FileLayeredMemoryStore::create_context_message(
    const std::vector<LayeredMemorySearchHit>& hits) const {
  if (hits.empty()) {
    return std::nullopt;
  }
  std::ostringstream out;
  out << context_title_;
  for (std::size_t index = 0; index < hits.size(); ++index) {
    out << "\n" << index + 1 << ". [" << hits[index].record.layer << "] "
        << hits[index].record.content;
  }
  return create_message(MessageRole::System, out.str(),
                        Value::object({{"source", "file-layered-memory"},
                                       {"hits", static_cast<long long>(hits.size())}}));
}

LayeredMemoryContextResult FileLayeredMemoryStore::build_context_message(
    const std::string& query,
    LayeredMemorySearchOptions options) const {
  auto hits = search(query, std::move(options));
  auto message = create_context_message(hits);
  return LayeredMemoryContextResult{std::move(hits), std::move(message)};
}

void FileLayeredMemoryStore::clear(std::string layer) {
  if (layer.empty()) {
    std::filesystem::remove_all(base_dir_);
    return;
  }
  std::filesystem::remove(layer_path(layer));
}
