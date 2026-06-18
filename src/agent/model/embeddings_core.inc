ChatProviderRegistry::ChatProviderRegistry(const ChatProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = other.factories_;
}

ChatProviderRegistry& ChatProviderRegistry::operator=(const ChatProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = other.factories_;
  return *this;
}

ChatProviderRegistry::ChatProviderRegistry(ChatProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = std::move(other.factories_);
}

ChatProviderRegistry& ChatProviderRegistry::operator=(ChatProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = std::move(other.factories_);
  return *this;
}

ChatProviderRegistry& ChatProviderRegistry::register_provider(std::string provider, ChatAdapterFactory factory) {
  if (provider.empty()) {
    throw ConfigurationError("Chat provider name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Chat provider factory is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[std::move(provider)] = std::move(factory);
  return *this;
}

bool ChatProviderRegistry::has(const std::string& provider) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(provider) != factories_.end();
}

std::shared_ptr<ChatModelAdapter> ChatProviderRegistry::create(const std::string& provider,
                                                               const Value& config) const {
  ChatAdapterFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = factories_.find(provider);
    if (found == factories_.end()) {
      throw ConfigurationError("Unknown chat provider: " + provider);
    }
    factory = found->second;
  }
  auto adapter = factory(config);
  if (!adapter) {
    throw ConfigurationError("Chat provider factory returned null for: " + provider);
  }
  return adapter;
}

TextEmbeddingAdapter::TextEmbeddingAdapter(std::string provider, std::string model, int dimensions,
                                           std::string space_id)
    : provider_(std::move(provider)),
      model_(std::move(model)),
      dimensions_(dimensions),
      space_id_(std::move(space_id)) {
  if (provider_.empty()) {
    throw ConfigurationError("Embedding provider is required.");
  }
  if (model_.empty()) {
    throw ConfigurationError("Embedding model is required.");
  }
}

const std::string& TextEmbeddingAdapter::provider() const noexcept {
  return provider_;
}

const std::string& TextEmbeddingAdapter::model() const noexcept {
  return model_;
}

int TextEmbeddingAdapter::dimensions() const noexcept {
  return dimensions_;
}

const std::string& TextEmbeddingAdapter::space_id() const noexcept {
  return space_id_;
}

EmbeddingSettings TextEmbeddingAdapter::resolve_settings(const EmbeddingSettings& settings) const {
  return merge_embedding_settings(provider_, model_, dimensions_, space_id_, settings, {"text"});
}

EmbeddingSpaceDescriptor TextEmbeddingAdapter::space() const {
  return EmbeddingSpaceDescriptor{
      .id = space_id_.empty() ? provider_ + ":" + model_ : space_id_,
      .modalities = {"text"},
      .dimensions = dimensions_,
  };
}

EmbeddingVector TextEmbeddingAdapter::embed_one(const std::string& text, const Value& settings,
                                                CancellationToken* cancellation) {
  auto vectors = embed({text}, settings, cancellation);
  return vectors.empty() ? EmbeddingVector{} : vectors.front();
}

HashEmbeddingAdapter::HashEmbeddingAdapter(int dimensions, std::string model, std::string space_id)
    : TextEmbeddingAdapter("hash", std::move(model), dimensions, std::move(space_id)) {}

std::vector<EmbeddingVector> HashEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                         const Value& settings,
                                                         CancellationToken* cancellation) {
  throw_if_model_cancelled(cancellation);
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(texts.size());
  const int dims = dimensions_from_settings(settings, dimensions());
  for (const auto& text : texts) {
    throw_if_model_cancelled(cancellation);
    EmbeddingVector vector(static_cast<std::size_t>(dims), 0);
    for (const auto& token : tokenize(text)) {
      const auto index = static_cast<std::size_t>(hash_token(token) % static_cast<std::uint32_t>(dims));
      vector[index] += 1;
    }
    vectors.push_back(normalize_vector(std::move(vector)));
  }
  throw_if_model_cancelled(cancellation);
  return vectors;
}

ClipTextEmbeddingAdapter::ClipTextEmbeddingAdapter(ClipTextEmbeddingBatch embed_batch)
    : ClipTextEmbeddingAdapter("Xenova/clip-vit-base-patch32", 0, "clip-shared-v1", std::move(embed_batch)) {}

ClipTextEmbeddingAdapter::ClipTextEmbeddingAdapter(std::string model, int dimensions,
                                                   std::string space_id,
                                                   ClipTextEmbeddingBatch embed_batch)
    : TextEmbeddingAdapter("clip", std::move(model), dimensions, std::move(space_id)),
      embed_batch_(std::move(embed_batch)) {}

std::vector<EmbeddingVector> ClipTextEmbeddingAdapter::embed(const std::vector<std::string>& texts,
                                                             const Value& settings,
                                                             CancellationToken* cancellation) {
  if (texts.empty()) {
    return {};
  }
  if (!embed_batch_) {
    throw ConfigurationError("ClipTextEmbeddingAdapter requires an injected batch function.");
  }
  throw_if_model_cancelled(cancellation);
  auto vectors = embed_batch_(texts, resolve_settings(embedding_settings_from_value(settings)));
  throw_if_model_cancelled(cancellation);
  return vectors;
}


ImageEmbeddingAdapter::ImageEmbeddingAdapter(std::string provider, std::string model, int dimensions,
                                             std::string space_id)
    : provider_(std::move(provider)),
      model_(std::move(model)),
      dimensions_(dimensions),
      space_id_(std::move(space_id)) {
  if (provider_.empty()) {
    throw ConfigurationError("Embedding provider is required.");
  }
  if (model_.empty()) {
    throw ConfigurationError("Embedding model is required.");
  }
}

const std::string& ImageEmbeddingAdapter::provider() const noexcept {
  return provider_;
}

const std::string& ImageEmbeddingAdapter::model() const noexcept {
  return model_;
}

int ImageEmbeddingAdapter::dimensions() const noexcept {
  return dimensions_;
}

const std::string& ImageEmbeddingAdapter::space_id() const noexcept {
  return space_id_;
}

EmbeddingSettings ImageEmbeddingAdapter::resolve_settings(const EmbeddingSettings& settings) const {
  return merge_embedding_settings(provider_, model_, dimensions_, space_id_, settings, {"image"});
}

EmbeddingSpaceDescriptor ImageEmbeddingAdapter::space() const {
  return EmbeddingSpaceDescriptor{
      .id = space_id_.empty() ? provider_ + ":" + model_ : space_id_,
      .modalities = {"image"},
      .dimensions = dimensions_,
  };
}

EmbeddingVector ImageEmbeddingAdapter::embed_one(const ImageEmbeddingInput& image, const Value& settings,
                                                 CancellationToken* cancellation) {
  auto vectors = embed({image}, settings, cancellation);
  return vectors.empty() ? EmbeddingVector{} : vectors.front();
}

HashImageEmbeddingAdapter::HashImageEmbeddingAdapter(int dimensions, std::string model, std::string space_id)
    : ImageEmbeddingAdapter("hash", std::move(model), dimensions, std::move(space_id)) {}

std::vector<EmbeddingVector> HashImageEmbeddingAdapter::embed(const std::vector<ImageEmbeddingInput>& images,
                                                              const Value& settings,
                                                              CancellationToken* cancellation) {
  throw_if_model_cancelled(cancellation);
  std::vector<EmbeddingVector> vectors;
  vectors.reserve(images.size());
  const int dims = dimensions_from_settings(settings, dimensions());
  for (const auto& image : images) {
    throw_if_model_cancelled(cancellation);
    EmbeddingVector vector(static_cast<std::size_t>(dims), 0);
    for (const auto& token : tokenize(image_to_hint(image))) {
      const auto index = static_cast<std::size_t>(hash_token(token) % static_cast<std::uint32_t>(dims));
      vector[index] += 1;
    }
    vectors.push_back(normalize_vector(std::move(vector)));
  }
  throw_if_model_cancelled(cancellation);
  return vectors;
}

ClipImageEmbeddingAdapter::ClipImageEmbeddingAdapter(ClipImageEmbeddingBatch embed_batch)
    : ClipImageEmbeddingAdapter("Xenova/clip-vit-base-patch32", 0, "clip-shared-v1", std::move(embed_batch)) {}

ClipImageEmbeddingAdapter::ClipImageEmbeddingAdapter(std::string model, int dimensions,
                                                     std::string space_id,
                                                     ClipImageEmbeddingBatch embed_batch)
    : ImageEmbeddingAdapter("clip", std::move(model), dimensions, std::move(space_id)),
      embed_batch_(std::move(embed_batch)) {}

std::vector<EmbeddingVector> ClipImageEmbeddingAdapter::embed(const std::vector<ImageEmbeddingInput>& images,
                                                              const Value& settings,
                                                              CancellationToken* cancellation) {
  if (images.empty()) {
    return {};
  }
  if (!embed_batch_) {
    throw ConfigurationError("ClipImageEmbeddingAdapter requires an injected batch function.");
  }
  throw_if_model_cancelled(cancellation);
  auto vectors = embed_batch_(images, resolve_settings(embedding_settings_from_value(settings)));
  throw_if_model_cancelled(cancellation);
  return vectors;
}

EmbeddingProviderRegistry::EmbeddingProviderRegistry(const EmbeddingProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = other.factories_;
}

EmbeddingProviderRegistry& EmbeddingProviderRegistry::operator=(const EmbeddingProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = other.factories_;
  return *this;
}

EmbeddingProviderRegistry::EmbeddingProviderRegistry(EmbeddingProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = std::move(other.factories_);
}

EmbeddingProviderRegistry& EmbeddingProviderRegistry::operator=(EmbeddingProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = std::move(other.factories_);
  return *this;
}

EmbeddingProviderRegistry& EmbeddingProviderRegistry::register_provider(std::string provider,
                                                                        EmbeddingAdapterFactory factory) {
  if (provider.empty()) {
    throw ConfigurationError("Embedding provider name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Embedding provider factory is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[std::move(provider)] = std::move(factory);
  return *this;
}

bool EmbeddingProviderRegistry::has(const std::string& provider) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(provider) != factories_.end();
}

std::shared_ptr<TextEmbeddingAdapter> EmbeddingProviderRegistry::create(const std::string& provider,
                                                                        const Value& config) const {
  EmbeddingAdapterFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = factories_.find(provider);
    if (found == factories_.end()) {
      throw ConfigurationError("Unknown embedding provider: " + provider);
    }
    factory = found->second;
  }
  auto adapter = factory(config);
  if (!adapter) {
    throw ConfigurationError("Embedding provider factory returned null for: " + provider);
  }
  return adapter;
}

ImageEmbeddingProviderRegistry::ImageEmbeddingProviderRegistry(const ImageEmbeddingProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = other.factories_;
}

ImageEmbeddingProviderRegistry& ImageEmbeddingProviderRegistry::operator=(
    const ImageEmbeddingProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = other.factories_;
  return *this;
}

ImageEmbeddingProviderRegistry::ImageEmbeddingProviderRegistry(ImageEmbeddingProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  factories_ = std::move(other.factories_);
}

ImageEmbeddingProviderRegistry& ImageEmbeddingProviderRegistry::operator=(
    ImageEmbeddingProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  factories_ = std::move(other.factories_);
  return *this;
}

ImageEmbeddingProviderRegistry& ImageEmbeddingProviderRegistry::register_provider(
    std::string provider, ImageEmbeddingAdapterFactory factory) {
  if (provider.empty()) {
    throw ConfigurationError("Image embedding provider name is required.");
  }
  if (!factory) {
    throw ConfigurationError("Image embedding provider factory is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  factories_[std::move(provider)] = std::move(factory);
  return *this;
}

bool ImageEmbeddingProviderRegistry::has(const std::string& provider) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return factories_.find(provider) != factories_.end();
}

std::shared_ptr<ImageEmbeddingAdapter> ImageEmbeddingProviderRegistry::create(const std::string& provider,
                                                                              const Value& config) const {
  ImageEmbeddingAdapterFactory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = factories_.find(provider);
    if (found == factories_.end()) {
      throw ConfigurationError("Unknown image embedding provider: " + provider);
    }
    factory = found->second;
  }
  auto adapter = factory(config);
  if (!adapter) {
    throw ConfigurationError("Image embedding provider factory returned null for: " + provider);
  }
  return adapter;
}
