KnowledgeBase::KnowledgeBase(std::string id, std::string tenant_id, std::string title,
                             std::shared_ptr<TextEmbeddingAdapter> embedder,
                             RecursiveTextChunker chunker,
                             std::shared_ptr<KnowledgeReranker> reranker,
                             std::shared_ptr<KnowledgeStore> store,
                             std::shared_ptr<ImageEmbeddingAdapter> image_embedder,
                             std::shared_ptr<KnowledgeTextIndex> text_index,
                             std::shared_ptr<KnowledgeVectorIndex> vector_index,
                             KnowledgeSearchOptions search_defaults,
                             std::string description,
                             std::string context_title,
                             std::shared_ptr<KnowledgeIngestionStrategy> ingestion_strategy,
                             std::shared_ptr<KnowledgeRetrievalStrategy> retrieval_strategy,
                             std::shared_ptr<KnowledgeContextRenderer> context_renderer)
    : id_(std::move(id)),
      tenant_id_(std::move(tenant_id)),
      title_(title.empty() ? id_ : std::move(title)),
      description_(std::move(description)),
      context_title_(context_title.empty() ? "Knowledge base retrieval: " + title_ : std::move(context_title)),
      embedder_(std::move(embedder)),
      image_embedder_(std::move(image_embedder)),
      chunker_(std::move(chunker)),
      reranker_(std::move(reranker)),
      store_(std::move(store)),
      text_index_(std::move(text_index)),
      vector_index_(std::move(vector_index)),
      ingestion_strategy_(std::move(ingestion_strategy)),
      retrieval_strategy_(std::move(retrieval_strategy)),
      context_renderer_(std::move(context_renderer)),
      search_defaults_(std::move(search_defaults)) {
  if (id_.empty()) {
    throw ConfigurationError("KnowledgeBase id is required.");
  }
  if (!reranker_) {
    reranker_ = std::make_shared<HeuristicKnowledgeReranker>();
  }
  if (!store_) {
    store_ = std::make_shared<InMemoryKnowledgeStore>();
  }
  if (!text_index_) {
    text_index_ = std::make_shared<InMemoryKnowledgeTextIndex>();
  }
  if (!image_embedder_) {
    image_embedder_ = std::make_shared<HashImageEmbeddingAdapter>();
  }
  if (!ingestion_strategy_) {
    ingestion_strategy_ = std::make_shared<DefaultKnowledgeIngestionStrategy>();
  }
  if (!context_renderer_) {
    context_renderer_ = std::make_shared<DefaultKnowledgeContextRenderer>();
  }
}

void KnowledgeBase::rebuild_text_index() {
  if (!text_index_) {
    return;
  }
  text_index_->clear(KnowledgeTextIndexClearOptions{id_, tenant_id_});
  text_index_->upsert(store_->list_chunks());
  text_index_ready_ = true;
}

void KnowledgeBase::ensure_text_index_ready() {
  if (!text_index_ready_) {
    rebuild_text_index();
  }
}

void KnowledgeBase::rebuild_vector_index() {
  if (!vector_index_) {
    return;
  }
  vector_index_->clear(KnowledgeVectorIndexClearOptions{id_, tenant_id_});
  vector_index_->upsert(store_->list_chunks());
  vector_index_ready_ = true;
}

void KnowledgeBase::ensure_vector_index_ready() {
  if (!vector_index_ready_) {
    rebuild_vector_index();
  }
}

KnowledgeIngestionResult KnowledgeBase::ingest_loaded_documents(
    const std::vector<LoadedKnowledgeDocument>& documents, bool replace_existing) {
  return ingest_loaded_documents(documents, KnowledgeIngestOptions{.replace_existing = replace_existing});
}

KnowledgeIngestionResult KnowledgeBase::ingest_loaded_documents(
    const std::vector<LoadedKnowledgeDocument>& documents, KnowledgeIngestOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  throw_if_knowledge_cancelled(options.cancellation);
  const auto normalized_documents = dedupe_loaded_knowledge_documents(documents, options.replace_existing);
  if (normalized_documents.empty()) {
    return KnowledgeIngestionResult{id_, tenant_id_, 0, 0, {}, {}};
  }
  const auto existing = store_->list_documents();
  const auto existing_chunks = store_->list_chunks();
  std::map<std::string, std::vector<KnowledgeDocumentRecord>> existing_by_uri;
  for (const auto& document : existing) {
    existing_by_uri[document.uri].push_back(document);
  }
  std::vector<LoadedKnowledgeDocument> documents_to_insert;
  std::set<std::string> document_ids_to_delete;
  for (const auto& document : normalized_documents) {
    throw_if_knowledge_cancelled(options.cancellation);
    const auto found = existing_by_uri.find(document.uri);
    const std::vector<KnowledgeDocumentRecord> matches =
        found == existing_by_uri.end() ? std::vector<KnowledgeDocumentRecord>{} : found->second;
    const auto content_hash = hash_knowledge_content(document.content);
    const bool unchanged = options.skip_if_unchanged &&
        std::any_of(matches.begin(), matches.end(), [&](const auto& current) {
          return hash_knowledge_content(current.content) == content_hash;
        });
    if (unchanged) {
      continue;
    }
    if (options.replace_existing) {
      for (const auto& current : matches) {
        document_ids_to_delete.insert(current.id);
      }
    }
    documents_to_insert.push_back(document);
  }
  if (documents_to_insert.empty()) {
    return KnowledgeIngestionResult{id_, tenant_id_, 0, 0, {}, {}};
  }
  if (options.replace_existing) {
    std::vector<std::string> to_delete;
    std::vector<std::string> chunks_to_delete;
    to_delete.assign(document_ids_to_delete.begin(), document_ids_to_delete.end());
    for (const auto& chunk : existing_chunks) {
      if (document_ids_to_delete.contains(chunk.document_id)) {
        chunks_to_delete.push_back(chunk.id);
      }
    }
    std::sort(chunks_to_delete.begin(), chunks_to_delete.end());
    chunks_to_delete.erase(std::unique(chunks_to_delete.begin(), chunks_to_delete.end()), chunks_to_delete.end());
    if (!to_delete.empty()) {
      std::set<std::string> seen;
      std::vector<std::string> unique_ids;
      for (const auto& id : to_delete) {
        if (seen.insert(id).second) {
          unique_ids.push_back(id);
        }
      }
      to_delete = std::move(unique_ids);
    }
    throw_if_knowledge_cancelled(options.cancellation);
    if (!chunks_to_delete.empty()) {
      store_->delete_chunks(chunks_to_delete);
    }
    if (!to_delete.empty()) {
      store_->delete_documents(to_delete);
    }
  }
  throw_if_knowledge_cancelled(options.cancellation);

  const auto now = now_iso8601();
  std::vector<KnowledgeDocumentRecord> records;
  std::vector<KnowledgeChunkRecord> chunks;
  const bool has_text_documents = std::any_of(documents_to_insert.begin(), documents_to_insert.end(), [](const auto& document) {
    return document.asset_type == KnowledgeAssetType::Text;
  });
  const bool has_image_documents = std::any_of(documents_to_insert.begin(), documents_to_insert.end(), [](const auto& document) {
    return document.asset_type == KnowledgeAssetType::Image;
  });
  const auto text_space_id = embedder_->space().id;
  const auto image_space_id = image_embedder_->space().id;
  if (has_text_documents && has_image_documents && text_space_id != image_space_id) {
    throw ConfigurationError("Cross-modal ingestion requires shared embedding space ids. Received text=\"" +
                             text_space_id + "\" and image=\"" + image_space_id + "\".");
  }

  for (const auto& loaded : documents_to_insert) {
    throw_if_knowledge_cancelled(options.cancellation);
    Value document_metadata = loaded.metadata;
    if (!loaded.alt_text.empty()) document_metadata["altText"] = loaded.alt_text;
    if (!loaded.ocr_text.empty()) document_metadata["ocrText"] = loaded.ocr_text;
    if (!loaded.caption.empty()) document_metadata["caption"] = loaded.caption;

    KnowledgeDocumentRecord record;
    record.id = generate_uuid();
    record.knowledge_base_id = id_;
    record.tenant_id = tenant_id_;
    record.source_type = loaded.source_type;
    record.asset_type = loaded.asset_type;
    record.uri = loaded.uri;
    record.title = loaded.title.empty() ? loaded.uri : loaded.title;
    record.content = loaded.content;
    record.media = loaded.media;
    record.text_hint = loaded.text_hint;
    record.metadata = document_metadata;
    record.created_at = now;
    record.updated_at = now;
    records.push_back(record);

    const auto drafts = chunker_.chunk(loaded);
    std::vector<EmbeddingVector> embeddings;
    if (loaded.asset_type == KnowledgeAssetType::Image) {
      if (!drafts.empty()) {
        auto image_vector = image_embedder_->embed_one(
            image_embedding_input_from_document(loaded),
            Value::object({{"taskType", "RETRIEVAL_DOCUMENT"}, {"spaceId", image_space_id}}),
            options.cancellation);
        embeddings.assign(drafts.size(), std::move(image_vector));
      }
    } else {
      std::vector<std::string> texts;
      texts.reserve(drafts.size());
      for (const auto& draft : drafts) {
        texts.push_back(draft.content);
      }
      const auto embedding_settings =
          Value::object({{"taskType", "RETRIEVAL_DOCUMENT"}, {"spaceId", text_space_id}});
      const std::size_t embedding_batch_size =
          options.embedding_batch_size == 0 ? texts.size() : options.embedding_batch_size;
      if (texts.empty()) {
        embeddings = {};
      } else if (embedding_batch_size >= texts.size()) {
        embeddings = embedder_->embed(texts, embedding_settings, options.cancellation);
      } else {
        for (std::size_t index = 0; index < texts.size(); index += embedding_batch_size) {
          throw_if_knowledge_cancelled(options.cancellation);
          const auto end = std::min(texts.size(), index + embedding_batch_size);
          std::vector<std::string> batch(texts.begin() + static_cast<std::ptrdiff_t>(index),
                                         texts.begin() + static_cast<std::ptrdiff_t>(end));
          auto batch_embeddings = embedder_->embed(batch, embedding_settings, options.cancellation);
          embeddings.insert(embeddings.end(),
                            std::make_move_iterator(batch_embeddings.begin()),
                            std::make_move_iterator(batch_embeddings.end()));
        }
      }
    }

    for (std::size_t index = 0; index < drafts.size(); ++index) {
      const auto& draft = drafts[index];
      Value chunk_metadata = document_metadata;
      for (const auto& [key, value] : draft.metadata.as_object()) {
        chunk_metadata[key] = value;
      }
      chunks.push_back(KnowledgeChunkRecord{generate_uuid(),
                                            record.id,
                                            id_,
                                            tenant_id_,
                                            loaded.source_type,
                                            loaded.asset_type,
                                            loaded.uri,
                                            record.title,
                                            draft.content,
                                            draft.chunk_index,
                                            draft.start_offset,
                                            draft.end_offset,
                                            draft.line_start,
                                            draft.line_end,
                                            loaded.media,
                                            loaded.asset_type == KnowledgeAssetType::Image ? image_space_id : text_space_id,
                                            chunk_metadata,
                                            index < embeddings.size() ? embeddings[index] : EmbeddingVector{},
                                            now,
                                            now});
    }
  }
  throw_if_knowledge_cancelled(options.cancellation);
  store_->upsert_documents(records);
  store_->upsert_chunks(chunks);
  rebuild_text_index();
  rebuild_vector_index();
  return KnowledgeIngestionResult{id_, tenant_id_, records.size(), chunks.size(), records, chunks};
}

KnowledgeIngestionResult KnowledgeBase::ingest(const std::vector<Value>& sources, const KnowledgeSourceLoader& loader,
                                               KnowledgeIngestOptions options) {
  if (ingestion_strategy_) {
    return ingestion_strategy_->ingest(KnowledgeIngestionStrategyContext{*this, sources, loader, options});
  }
  throw_if_knowledge_cancelled(options.cancellation);
  auto documents = load_knowledge_sources(sources, loader);
  throw_if_knowledge_cancelled(options.cancellation);
  return ingest_loaded_documents(documents, options);
}

KnowledgeDeleteResult KnowledgeBase::delete_documents(const KnowledgeDeleteOptions& options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto all_documents = store_->list_documents();
  std::vector<KnowledgeDocumentRecord> matched_documents;
  for (const auto& document : all_documents) {
    if (!options.document_ids.empty() && !vector_contains(options.document_ids, document.id)) {
      continue;
    }
    if (!options.uris.empty() && !vector_contains(options.uris, document.uri)) {
      continue;
    }
    if (!options.uri_prefix.empty() && document.uri.rfind(options.uri_prefix, 0) != 0) {
      continue;
    }
    if (!options.source_types.empty() && !vector_contains(options.source_types, document.source_type)) {
      continue;
    }
    if (!metadata_matches_filter(document.metadata, options.metadata)) {
      continue;
    }
    matched_documents.push_back(document);
  }

  KnowledgeDeleteResult result;
  result.knowledge_base_id = id_;
  result.tenant_id = tenant_id_;
  if (matched_documents.empty()) {
    return result;
  }

  std::set<std::string> matched_document_ids;
  for (const auto& document : matched_documents) {
    matched_document_ids.insert(document.id);
    result.document_ids.push_back(document.id);
  }
  for (const auto& chunk : store_->list_chunks()) {
    if (matched_document_ids.contains(chunk.document_id)) {
      result.chunk_ids.push_back(chunk.id);
    }
  }

  if (!result.chunk_ids.empty()) {
    store_->delete_chunks(result.chunk_ids);
  }
  store_->delete_documents(result.document_ids);
  result.document_count = result.document_ids.size();
  result.chunk_count = result.chunk_ids.size();
  rebuild_text_index();
  rebuild_vector_index();
  return result;
}

std::vector<KnowledgeSearchHit> KnowledgeBase::search(const std::string& query, KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return search_with_debug(query, std::move(options)).hits;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto query_vector = embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", embedder_->space().id}}),
      options.cancellation);
  return search_with_embedding(query, query_vector, std::move(options), nullptr);
}

std::vector<KnowledgeSearchHit> KnowledgeBase::search(const ImageEmbeddingInput& query, KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return search_with_debug(query, std::move(options)).hits;
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto query_vector = image_embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", image_embedder_->space().id}}),
      options.cancellation);
  const std::string query_text = image_embedding_query_text(query);
  return search_with_embedding(query_text, query_vector, std::move(options), nullptr);
}

KnowledgeSearchResult KnowledgeBase::search_with_debug(const std::string& query,
                                                       KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return retrieval_strategy_->search(*this, query, std::move(options));
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Value debug = Value::object({});
  const auto query_vector = embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", embedder_->space().id}}),
      options.cancellation);
  auto hits = search_with_embedding(query, query_vector, std::move(options), &debug);
  return KnowledgeSearchResult{std::move(hits), std::move(debug)};
}

KnowledgeSearchResult KnowledgeBase::search_with_debug(const ImageEmbeddingInput& query,
                                                       KnowledgeSearchOptions options) {
  if (retrieval_strategy_) {
    return retrieval_strategy_->search(*this, query, std::move(options));
  }
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  Value debug = Value::object({});
  const auto query_vector = image_embedder_->embed_one(
      query,
      Value::object({{"taskType", "RETRIEVAL_QUERY"}, {"spaceId", image_embedder_->space().id}}),
      options.cancellation);
  const std::string query_text = image_embedding_query_text(query);
  auto hits = search_with_embedding(query_text, query_vector, std::move(options), &debug);
  return KnowledgeSearchResult{std::move(hits), std::move(debug)};
}

std::vector<KnowledgeSearchHit> KnowledgeBase::search_with_embedding(const std::string& query_text,
                                                                     const EmbeddingVector& query_vector,
                                                                     KnowledgeSearchOptions options,
                                                                     Value* debug) {
  options = effective_knowledge_search_options(search_defaults_, options);
  const auto documents = store_->list_documents();
  const auto chunks = store_->list_chunks();
  std::map<std::string, KnowledgeDocumentRecord> document_by_id;
  for (const auto& document : documents) {
    document_by_id[document.id] = document;
  }

  std::vector<KnowledgeChunkRecord> filtered_chunks;
  filtered_chunks.reserve(chunks.size());
  for (const auto& chunk : chunks) {
    if (!options.uri_prefix.empty() && chunk.uri.rfind(options.uri_prefix, 0) != 0) {
      continue;
    }
    if (!options.document_ids.empty() && !vector_contains(options.document_ids, chunk.document_id)) {
      continue;
    }
    if (!options.chunk_ids.empty() && !vector_contains(options.chunk_ids, chunk.id)) {
      continue;
    }
    if (!options.source_types.empty() && !vector_contains(options.source_types, chunk.source_type)) {
      continue;
    }
    if (!options.asset_types.empty() &&
        std::find(options.asset_types.begin(), options.asset_types.end(), chunk.asset_type) ==
            options.asset_types.end()) {
      continue;
    }
    if (!options.space_id.empty() && chunk.embedding_space_id != options.space_id) {
      continue;
    }
    if (!metadata_matches_filter(chunk.metadata, options.metadata)) {
      continue;
    }
    filtered_chunks.push_back(chunk);
  }

  std::map<std::string, const KnowledgeChunkRecord*> chunk_by_id;
  for (const auto& chunk : filtered_chunks) {
    chunk_by_id[chunk.id] = &chunk;
  }

  const auto effective_vector_top_k = effective_knowledge_vector_top_k(options, filtered_chunks.size());
  std::vector<RankedKnowledgeChunk> vector_ranked;
  if (!filtered_chunks.empty() && effective_vector_top_k > 0) {
    if (vector_index_) {
      ensure_vector_index_ready();
      KnowledgeVectorSearchOptions vector_options;
      vector_options.knowledge_base_id = id_;
      vector_options.tenant_id = tenant_id_;
      vector_options.embedding = query_vector;
      vector_options.top_k = effective_vector_top_k;
      vector_options.min_score = 0.0;
      vector_options.source_types = options.source_types;
      vector_options.asset_types = options.asset_types;
      vector_options.uri_prefix = options.uri_prefix;
      vector_options.document_ids = options.document_ids;
      vector_options.chunk_ids = options.chunk_ids;
      vector_options.space_id = options.space_id;
      vector_options.metadata = options.metadata;
      for (const auto& match : vector_index_->search(vector_options)) {
        if (const auto found = chunk_by_id.find(match.chunk_id); found != chunk_by_id.end()) {
          vector_ranked.push_back(RankedKnowledgeChunk{found->second, match.score});
        }
      }
    } else {
      vector_ranked.reserve(filtered_chunks.size());
      for (const auto& chunk : filtered_chunks) {
        vector_ranked.push_back(RankedKnowledgeChunk{&chunk, dot_product(query_vector, chunk.embedding)});
      }
    }
    std::sort(vector_ranked.begin(), vector_ranked.end(), [](const auto& left, const auto& right) {
      if (left.score == right.score) {
        return left.chunk->id < right.chunk->id;
      }
      return left.score > right.score;
    });
    if (vector_ranked.size() > effective_vector_top_k) {
      vector_ranked.resize(effective_vector_top_k);
    }
  }

  std::map<std::string, double> vector_score_by_id;
  std::map<std::string, std::size_t> vector_rank_by_id;
  for (std::size_t index = 0; index < vector_ranked.size(); ++index) {
    vector_score_by_id[vector_ranked[index].chunk->id] = vector_ranked[index].score;
    vector_rank_by_id[vector_ranked[index].chunk->id] = index + 1;
  }

  const std::size_t effective_lexical_top_k = options.lexical_top_k == 0
                                                 ? filtered_chunks.size()
                                                 : std::min(filtered_chunks.size(), options.lexical_top_k);
  std::vector<RankedKnowledgeChunk> lexical_ranked;
  if (text_index_ && !query_text.empty() && !filtered_chunks.empty() && effective_lexical_top_k > 0) {
    ensure_text_index_ready();
    KnowledgeTextSearchOptions text_options;
    text_options.knowledge_base_id = id_;
    text_options.tenant_id = tenant_id_;
    text_options.query = query_text;
    text_options.top_k = effective_lexical_top_k;
    text_options.source_types = options.source_types;
    text_options.asset_types = options.asset_types;
    text_options.uri_prefix = options.uri_prefix;
    text_options.document_ids = options.document_ids;
    text_options.chunk_ids = options.chunk_ids;
    text_options.space_id = options.space_id;
    text_options.metadata = options.metadata;
    for (const auto& match : text_index_->search(text_options)) {
      if (const auto found = chunk_by_id.find(match.chunk_id); found != chunk_by_id.end()) {
        lexical_ranked.push_back(RankedKnowledgeChunk{found->second, match.score});
      }
    }
  } else if (!text_index_ && !query_text.empty() && !filtered_chunks.empty() && effective_lexical_top_k > 0) {
    const auto query_tokens = tokenize_knowledge_text(query_text);
    lexical_ranked.reserve(filtered_chunks.size());
    for (const auto& chunk : filtered_chunks) {
      const auto chunk_tokens = tokenize_knowledge_text(knowledge_text_index_surface(chunk));
      std::size_t overlap = 0;
      for (const auto& token : query_tokens) {
        if (std::find(chunk_tokens.begin(), chunk_tokens.end(), token) != chunk_tokens.end()) {
          ++overlap;
        }
      }
      const double score = query_tokens.empty() ? 0 : static_cast<double>(overlap) / query_tokens.size();
      if (score > 0) {
        lexical_ranked.push_back(RankedKnowledgeChunk{&chunk, score});
      }
    }
  }
  std::sort(lexical_ranked.begin(), lexical_ranked.end(), [](const auto& left, const auto& right) {
    if (left.score == right.score) {
      return left.chunk->id < right.chunk->id;
    }
    return left.score > right.score;
  });
  if (lexical_ranked.size() > effective_lexical_top_k) {
    lexical_ranked.resize(effective_lexical_top_k);
  }

  std::map<std::string, double> lexical_score_by_id;
  std::map<std::string, std::size_t> lexical_rank_by_id;
  for (std::size_t index = 0; index < lexical_ranked.size(); ++index) {
    lexical_score_by_id[lexical_ranked[index].chunk->id] = lexical_ranked[index].score;
    lexical_rank_by_id[lexical_ranked[index].chunk->id] = index + 1;
  }

  std::set<std::string> candidate_ids;
  for (const auto& item : vector_ranked) {
    candidate_ids.insert(item.chunk->id);
  }
  if (lower_copy(options.retrieval_mode) != "vector") {
    for (const auto& item : lexical_ranked) {
      candidate_ids.insert(item.chunk->id);
    }
  }

  std::vector<KnowledgeSearchHit> hits;
  for (const auto& chunk : filtered_chunks) {
    if (!candidate_ids.contains(chunk.id)) {
      continue;
    }
    const auto vector_found = vector_score_by_id.find(chunk.id);
    const double vector_score = vector_found == vector_score_by_id.end() ? 0 : vector_found->second;
    const auto lexical_found = lexical_score_by_id.find(chunk.id);
    const double lexical_score = lexical_found == lexical_score_by_id.end() ? 0 : lexical_found->second;
    const double modality_weight = knowledge_modality_weight(options, chunk.asset_type);
    double score = 0;
    if (lower_copy(options.fusion) == "rrf") {
      constexpr double rrf_offset = 60.0;
      const double rrf_base = (1.0 / (rrf_offset + 1.0)) * 2.0;
      const auto vector_rank_found = vector_rank_by_id.find(chunk.id);
      const auto lexical_rank_found = lexical_rank_by_id.find(chunk.id);
      const double vector_rrf = vector_rank_found == vector_rank_by_id.end()
                                    ? 0.0
                                    : 1.0 / (rrf_offset + static_cast<double>(vector_rank_found->second));
      const double lexical_rrf = lexical_rank_found == lexical_rank_by_id.end()
                                     ? 0.0
                                     : 1.0 / (rrf_offset + static_cast<double>(lexical_rank_found->second));
      score = ((vector_rrf + lexical_rrf) / rrf_base) * modality_weight;
    } else {
      score = (options.hybrid_alpha * vector_score + (1.0 - options.hybrid_alpha) * lexical_score) *
              modality_weight;
    }
    if (score < options.min_score) {
      continue;
    }
    const auto document_found = document_by_id.find(chunk.document_id);
    if (document_found == document_by_id.end()) {
      continue;
    }
    const auto& document = document_found->second;
    const std::string snippet = chunk.content.size() > 240 ? chunk.content.substr(0, 237) + "..." : chunk.content;
    KnowledgeCitation citation{id_,
                               title_,
                               tenant_id_,
                               document.id,
                               chunk.id,
                               chunk.source_type,
                               chunk.asset_type,
                               chunk.uri,
                               chunk.title,
                               chunk.chunk_index,
                               chunk.start_offset,
                               chunk.end_offset,
                               chunk.line_start,
                               chunk.line_end,
                               score,
                               vector_score,
                               lexical_score,
                               chunk.media,
                               chunk.embedding_space_id,
                               chunk.metadata,
                               snippet};
    hits.push_back(KnowledgeSearchHit{document, chunk, score, vector_score, lexical_score, std::nullopt, citation});
  }

  std::sort(hits.begin(), hits.end(), [](const auto& left, const auto& right) {
    return left.score > right.score;
  });
  const auto hybrid_hits = hits;
  const std::size_t rerank_top_k = options.rerank_top_k == 0
                                       ? std::min(hits.size(), std::max<std::size_t>(options.top_k, 8))
                                       : std::min(hits.size(), options.rerank_top_k);
  std::vector<KnowledgeSearchHit> candidates(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(rerank_top_k));
  std::vector<KnowledgeSearchHit> final_hits;
  if (reranker_) {
    final_hits = reranker_->rerank(query_text, std::move(candidates), options.top_k);
  } else {
    final_hits = std::move(candidates);
    if (options.top_k > 0 && final_hits.size() > options.top_k) {
      final_hits.resize(options.top_k);
    }
  }

  if (debug) {
    Value::Array vector_candidates;
    for (const auto& item : vector_ranked) {
      vector_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, *item.chunk, item.score, item.score, std::nullopt));
    }
    Value::Array lexical_candidates;
    for (const auto& item : lexical_ranked) {
      lexical_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, *item.chunk, item.score, std::nullopt, item.score));
    }
    Value::Array hybrid_candidates;
    for (const auto& hit : hybrid_hits) {
      hybrid_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, hit.chunk, hit.score, hit.vector_score, hit.lexical_score));
    }
    Value::Array final_candidates;
    for (const auto& hit : final_hits) {
      final_candidates.push_back(knowledge_search_debug_candidate_value(
          id_, tenant_id_, hit.chunk, hit.rerank_score.value_or(hit.score), hit.vector_score, hit.lexical_score));
    }
    *debug = Value::object({{"query", query_text},
                            {"totalDocumentCount", documents.size()},
                            {"totalChunkCount", chunks.size()},
                            {"filteredChunkCount", filtered_chunks.size()},
                            {"vectorCandidates", Value(std::move(vector_candidates))},
                            {"lexicalCandidates", Value(std::move(lexical_candidates))},
                            {"hybridCandidates", Value(std::move(hybrid_candidates))},
                            {"finalHits", Value(std::move(final_candidates))}});
  }
  return final_hits;
}

KnowledgeStoreStats KnowledgeBase::stats() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return store_->stats();
}

std::shared_ptr<KnowledgeStore> KnowledgeBase::store() const noexcept {
  return store_;
}

std::optional<AgentMessage> KnowledgeBase::create_context_message(const std::vector<KnowledgeSearchHit>& hits) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (context_renderer_) {
    return context_renderer_->render(KnowledgeContextRenderContext{*this, hits});
  }
  return DefaultKnowledgeContextRenderer().render(KnowledgeContextRenderContext{*this, hits});
}

KnowledgeContextResult KnowledgeBase::build_context_message(const std::string& query,
                                                            KnowledgeSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto search_result = search_with_debug(query, std::move(options));
  KnowledgeContextResult context;
  context.hits = std::move(search_result.hits);
  context.message = create_context_message(context.hits);
  context.debug = std::move(search_result.debug);
  return context;
}

KnowledgeContextResult KnowledgeBase::build_context_message(const ImageEmbeddingInput& query,
                                                            KnowledgeSearchOptions options) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto search_result = search_with_debug(query, std::move(options));
  KnowledgeContextResult context;
  context.hits = std::move(search_result.hits);
  context.message = create_context_message(context.hits);
  context.debug = std::move(search_result.debug);
  return context;
}

const std::string& KnowledgeBase::id() const noexcept {
  return id_;
}

const std::string& KnowledgeBase::tenant_id() const noexcept {
  return tenant_id_;
}

const std::string& KnowledgeBase::title() const noexcept {
  return title_;
}

const std::string& KnowledgeBase::description() const noexcept {
  return description_;
}

const std::string& KnowledgeBase::context_title() const noexcept {
  return context_title_;
}

KnowledgeIngestionPipeline::KnowledgeIngestionPipeline(KnowledgeBase& knowledge_base,
                                                       const KnowledgeSourceLoader& loader)
    : knowledge_base_(knowledge_base), loader_(loader) {}

KnowledgeIngestionResult KnowledgeIngestionPipeline::ingest(
    const std::vector<Value>& sources,
    KnowledgeIngestionPipelineOptions options) const {
  throw_if_knowledge_cancelled(options.cancellation);
  report_knowledge_progress(options.on_progress, "loading", sources.size(), 0);
  FixedKnowledgeRetryStrategy default_retry_strategy(options.max_attempts, options.retry_delay_ms);
  const auto& retry_strategy = options.retry_strategy ? *options.retry_strategy : default_retry_strategy;
  auto loaded_documents = run_with_knowledge_retry([&]() {
    return load_knowledge_sources(sources, loader_);
  }, retry_strategy, KnowledgeRetryContext{knowledge_base_, "loading"}, options.cancellation);
  throw_if_knowledge_cancelled(options.cancellation);

  report_knowledge_progress(options.on_progress, "loaded", sources.size(), loaded_documents.size());
  UriContentHashDedupeStrategy default_dedupe_strategy;
  const auto& dedupe_strategy = options.dedupe_strategy ? *options.dedupe_strategy : default_dedupe_strategy;
  auto deduped = dedupe_strategy.dedupe(KnowledgeDedupeContext{knowledge_base_,
                                                               sources,
                                                               loaded_documents,
                                                               options.replace_existing});
  report_knowledge_progress(options.on_progress, "deduping", sources.size(), deduped.size());

  SkipUnchangedKnowledgeIncrementalStrategy default_incremental_strategy;
  const auto& incremental_strategy =
      options.incremental_strategy ? *options.incremental_strategy : default_incremental_strategy;
  auto incremental = incremental_strategy.prepare(KnowledgeIncrementalContext{knowledge_base_,
                                                                             sources,
                                                                             loaded_documents,
                                                                             deduped,
                                                                             options.replace_existing,
                                                                             options.skip_if_unchanged});
  KnowledgeIngestionPipelineOptions ingest_options = options;
  ingest_options.replace_existing = incremental.replace_existing;
  ingest_options.skip_if_unchanged = incremental.skip_if_unchanged;
  return ingest_loaded_document_batches(knowledge_base_, sources.size(), incremental.documents.size(),
                                        incremental.documents, ingest_options);
}

KnowledgeSyncState InMemoryKnowledgeSyncStateStore::load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void InMemoryKnowledgeSyncStateStore::save(const KnowledgeSyncState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = state;
}

FileKnowledgeSyncStateStore::FileKnowledgeSyncStateStore(std::filesystem::path file_path)
    : file_path_(std::move(file_path)) {}

KnowledgeSyncState FileKnowledgeSyncStateStore::load() const {
  std::lock_guard<std::mutex> lock(mutex_);
  KnowledgeSyncState state;
  if (!std::filesystem::exists(file_path_)) {
    return state;
  }
  const auto raw = read_json_file(file_path_);
  for (const auto& [uri, hash] : raw.at("documentHashes").as_object()) {
    state.document_hashes[uri] = hash.as_string();
  }
  state.synced_at = raw.at("syncedAt").as_string();
  return state;
}

void FileKnowledgeSyncStateStore::save(const KnowledgeSyncState& state) {
  std::lock_guard<std::mutex> lock(mutex_);
  Value hashes = Value::object({});
  for (const auto& [uri, hash] : state.document_hashes) {
    hashes[uri] = hash;
  }
  write_json_file(file_path_, Value::object({{"documentHashes", hashes}, {"syncedAt", state.synced_at}}));
}

const std::filesystem::path& FileKnowledgeSyncStateStore::file_path() const noexcept {
  return file_path_;
}

KnowledgeSyncJob::KnowledgeSyncJob(KnowledgeBase& knowledge_base,
                                   const KnowledgeSourceLoader& loader,
                                   KnowledgeSyncStateStore& state_store)
    : knowledge_base_(knowledge_base), loader_(loader), state_store_(state_store) {}

KnowledgeSyncResult KnowledgeSyncJob::sync(const std::vector<Value>& sources, KnowledgeSyncOptions options) const {
  throw_if_knowledge_cancelled(options.cancellation);
  report_knowledge_progress(options.on_progress, "loading", sources.size(), 0);
  FixedKnowledgeRetryStrategy default_retry_strategy(options.max_attempts, options.retry_delay_ms);
  const auto& retry_strategy = options.retry_strategy ? *options.retry_strategy : default_retry_strategy;
  auto loaded_documents = run_with_knowledge_retry([&]() {
    return load_knowledge_sources(sources, loader_);
  }, retry_strategy, KnowledgeRetryContext{knowledge_base_, "loading"}, options.cancellation);
  throw_if_knowledge_cancelled(options.cancellation);

  report_knowledge_progress(options.on_progress, "loaded", sources.size(), loaded_documents.size());
  UriContentHashDedupeStrategy default_dedupe_strategy;
  const auto& dedupe_strategy = options.dedupe_strategy ? *options.dedupe_strategy : default_dedupe_strategy;
  auto deduped = dedupe_strategy.dedupe(KnowledgeDedupeContext{knowledge_base_,
                                                               sources,
                                                               loaded_documents,
                                                               options.replace_existing});
  report_knowledge_progress(options.on_progress, "deduping", sources.size(), deduped.size());

  const auto previous_state = state_store_.load();
  std::map<std::string, std::string> next_hashes;
  for (const auto& document : deduped) {
    next_hashes[document.uri] = hash_knowledge_content(document.content);
  }

  std::vector<LoadedKnowledgeDocument> changed_documents;
  for (const auto& document : deduped) {
    const auto hash = next_hashes.at(document.uri);
    const auto found = previous_state.document_hashes.find(document.uri);
    if (found == previous_state.document_hashes.end() || found->second != hash) {
      changed_documents.push_back(document);
    }
  }

  std::vector<std::string> deleted_uris;
  if (options.delete_missing) {
    for (const auto& [uri, _] : previous_state.document_hashes) {
      throw_if_knowledge_cancelled(options.cancellation);
      if (!next_hashes.contains(uri)) {
        deleted_uris.push_back(uri);
      }
    }
    if (!deleted_uris.empty()) {
      throw_if_knowledge_cancelled(options.cancellation);
      KnowledgeDeleteOptions delete_options;
      delete_options.uris = deleted_uris;
      knowledge_base_.delete_documents(delete_options);
    }
  }

  options.replace_existing = true;
  options.skip_if_unchanged = true;
  auto aggregate = ingest_loaded_document_batches(knowledge_base_, sources.size(), deduped.size(),
                                                  changed_documents, options);

  KnowledgeSyncState next_state;
  next_state.document_hashes = std::move(next_hashes);
  next_state.synced_at = now_iso8601();
  throw_if_knowledge_cancelled(options.cancellation);
  state_store_.save(next_state);

  KnowledgeSyncResult result;
  result.knowledge_base_id = aggregate.knowledge_base_id;
  result.tenant_id = aggregate.tenant_id;
  result.document_count = aggregate.document_count;
  result.chunk_count = aggregate.chunk_count;
  result.documents = std::move(aggregate.documents);
  result.chunks = std::move(aggregate.chunks);
  result.loaded_document_count = deduped.size();
  result.changed_document_count = changed_documents.size();
  result.skipped_document_count = deduped.size() - changed_documents.size();
  result.deleted_document_count = deleted_uris.size();
  result.deleted_uris = std::move(deleted_uris);
  result.synced_at = next_state.synced_at;
  return result;
}

namespace {

Value knowledge_base_definition_to_value(const KnowledgeBaseDefinition& definition) {
  return Value::object({{"id", definition.id},
                        {"tenantId", definition.tenant_id},
                        {"title", definition.title},
                        {"description", definition.description.empty() ? Value() : Value(definition.description)},
                        {"storeFilePath", definition.store_file_path.empty() ? Value() : Value(definition.store_file_path)}});
}

KnowledgeBaseDefinition knowledge_base_definition_from_value(const Value& value) {
  KnowledgeBaseDefinition definition;
  definition.id = value.at("id").as_string();
  definition.tenant_id = value.at("tenantId").as_string("default");
  definition.title = value.at("title").as_string(definition.id);
  definition.description = value.at("description").as_string();
  definition.store_file_path = value.at("storeFilePath").as_string();
  return definition;
}

Value knowledge_hits_to_value(const std::vector<KnowledgeSearchHit>& hits) {
  Value::Array values;
  for (const auto& hit : hits) {
    values.push_back(knowledge_search_hit_to_value(hit));
  }
  return Value(values);
}

}  // namespace
