std::vector<KnowledgeDocumentRecord> InMemoryKnowledgeStore::upsert_documents(
    const std::vector<KnowledgeDocumentRecord>& documents) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& document : documents) {
    documents_[document.id] = document;
  }
  return documents;
}

std::vector<KnowledgeChunkRecord> InMemoryKnowledgeStore::upsert_chunks(
    const std::vector<KnowledgeChunkRecord>& chunks) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& chunk : chunks) {
    chunks_[chunk.id] = chunk;
  }
  return chunks;
}

std::vector<KnowledgeDocumentRecord> InMemoryKnowledgeStore::list_documents() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<KnowledgeDocumentRecord> documents;
  documents.reserve(documents_.size());
  for (const auto& [_, document] : documents_) {
    documents.push_back(document);
  }
  return documents;
}

std::vector<KnowledgeChunkRecord> InMemoryKnowledgeStore::list_chunks() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<KnowledgeChunkRecord> chunks;
  chunks.reserve(chunks_.size());
  for (const auto& [_, chunk] : chunks_) {
    chunks.push_back(chunk);
  }
  return chunks;
}

std::size_t InMemoryKnowledgeStore::delete_documents(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::size_t deleted = 0;
  for (const auto& id : ids) {
    deleted += documents_.erase(id);
  }
  return deleted;
}

std::size_t InMemoryKnowledgeStore::delete_chunks(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::size_t deleted = 0;
  for (const auto& id : ids) {
    deleted += chunks_.erase(id);
  }
  return deleted;
}

void InMemoryKnowledgeStore::clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  documents_.clear();
  chunks_.clear();
}

KnowledgeStoreStats InMemoryKnowledgeStore::stats() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return KnowledgeStoreStats{documents_.size(), chunks_.size()};
}

FileKnowledgeStore::FileKnowledgeStore(std::filesystem::path file_path) : file_path_(std::move(file_path)) {}

std::vector<KnowledgeDocumentRecord> FileKnowledgeStore::upsert_documents(
    const std::vector<KnowledgeDocumentRecord>& documents) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryKnowledgeStore::upsert_documents(documents);
  persist();
  return result;
}

std::vector<KnowledgeChunkRecord> FileKnowledgeStore::upsert_chunks(
    const std::vector<KnowledgeChunkRecord>& chunks) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  auto result = InMemoryKnowledgeStore::upsert_chunks(chunks);
  persist();
  return result;
}

std::vector<KnowledgeDocumentRecord> FileKnowledgeStore::list_documents() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryKnowledgeStore::list_documents();
}

std::vector<KnowledgeChunkRecord> FileKnowledgeStore::list_chunks() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryKnowledgeStore::list_chunks();
}

std::size_t FileKnowledgeStore::delete_documents(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  const auto deleted = InMemoryKnowledgeStore::delete_documents(ids);
  persist();
  return deleted;
}

std::size_t FileKnowledgeStore::delete_chunks(const std::vector<std::string>& ids) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  const auto deleted = InMemoryKnowledgeStore::delete_chunks(ids);
  persist();
  return deleted;
}

void FileKnowledgeStore::clear() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  InMemoryKnowledgeStore::clear();
  persist();
}

KnowledgeStoreStats FileKnowledgeStore::stats() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ensure_loaded();
  return InMemoryKnowledgeStore::stats();
}

const std::filesystem::path& FileKnowledgeStore::file_path() const noexcept {
  return file_path_;
}

void FileKnowledgeStore::ensure_loaded() const {
  if (loaded_) {
    return;
  }
  loaded_ = true;
  documents_.clear();
  chunks_.clear();
  if (!std::filesystem::exists(file_path_)) {
    return;
  }

  const auto raw = read_json_file(file_path_);
  for (const auto& item : raw.at("documents").as_array()) {
    auto document = knowledge_document_record_from_value(item);
    if (!document.id.empty()) {
      const auto document_id = document.id;
      documents_[document_id] = std::move(document);
    }
  }
  for (const auto& item : raw.at("chunks").as_array()) {
    auto chunk = knowledge_chunk_record_from_value(item);
    if (!chunk.id.empty()) {
      const auto chunk_id = chunk.id;
      chunks_[chunk_id] = std::move(chunk);
    }
  }
}

void FileKnowledgeStore::persist() const {
  Value::Array documents;
  for (const auto& [_, document] : documents_) {
    documents.push_back(knowledge_document_record_to_value(document));
  }
  Value::Array chunks;
  for (const auto& [_, chunk] : chunks_) {
    chunks.push_back(knowledge_chunk_record_to_value(chunk));
  }
  write_json_file(file_path_, Value::object({{"documents", Value(documents)}, {"chunks", Value(chunks)}}));
}
