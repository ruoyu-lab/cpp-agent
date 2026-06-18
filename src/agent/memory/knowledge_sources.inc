bool TextKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "text";
}

std::vector<LoadedKnowledgeDocument> TextKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto text = source.at("text").as_string();
  return {build_loaded_document("text",
                                source.at("uri").as_string("text://" + generate_uuid()),
                                source.at("title").as_string("Inline text"),
                                text,
                                copy_object_or_empty(source.at("metadata")))};
}

bool FileKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "file";
}

std::vector<LoadedKnowledgeDocument> FileKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("File knowledge source requires path.");
  }

  const auto path = std::filesystem::absolute(std::filesystem::path(raw_path));
  const auto extension = extension_for_path(path);
  auto metadata = copy_object_or_empty(source.at("metadata"));
  const auto title = source.at("title").as_string(path.filename().string());

  if (knowledge_image_extensions().contains(extension)) {
    const std::string alt_text = metadata_text(metadata, "altText");
    const std::string ocr_text = metadata_text(metadata, "ocrText");
    const std::string caption = metadata_text(metadata, "caption");
    std::string text_hint = metadata_text(metadata, "textHint");
    if (text_hint.empty()) text_hint = ocr_text;
    if (text_hint.empty()) text_hint = alt_text;
    if (text_hint.empty()) text_hint = caption;
    MediaSource media;
    media.kind = MediaSourceKind::Path;
    media.path = path.string();
    media.mime_type = DefaultMediaResolver::extension_to_mime(path);
    media.filename = path.filename().string();
    metadata["mediaKind"] = "path";
    metadata["path"] = path.string();
    return {build_loaded_document("file", path.string(), title, text_hint, std::move(metadata),
                                  KnowledgeAssetType::Image, media, text_hint, alt_text, ocr_text, caption)};
  }

  const auto bytes = read_binary_file(path);
  if (extension == ".pdf") {
    throw ConfigurationError("PDF knowledge sources require the optional document I/O pipeline.");
  }

  std::string content = bytes_to_text(bytes);
  if (extension == ".html" || extension == ".htm") {
    const auto parsed_title = extract_html_title(content);
    return {build_loaded_document("file", path.string(),
                                  source.at("title").as_string(parsed_title.empty() ? title : parsed_title),
                                  html_to_text(content),
                                  std::move(metadata))};
  }

  return {build_loaded_document("file", path.string(), title, std::move(content), std::move(metadata))};
}

DirectoryKnowledgeSourceLoader::DirectoryKnowledgeSourceLoader(std::shared_ptr<FileKnowledgeSourceLoader> file_loader)
    : file_loader_(std::move(file_loader)) {
  if (!file_loader_) {
    file_loader_ = std::make_shared<FileKnowledgeSourceLoader>();
  }
}

bool DirectoryKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "directory";
}

std::vector<LoadedKnowledgeDocument> DirectoryKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("Directory knowledge source requires path.");
  }

  const auto directory = std::filesystem::absolute(std::filesystem::path(raw_path));
  if (!std::filesystem::is_directory(directory)) {
    throw ConfigurationError("Path is not a directory: " + directory.string());
  }

  const bool recursive = !source.contains("recursive") || source.at("recursive").as_bool(true);
  const auto include_extensions = string_set_from_value(source.at("includeExtensions"),
                                                        default_knowledge_text_extensions());
  const auto exclude_extensions = string_set_from_value(source.at("excludeExtensions"));
  const auto exclude_directories = string_set_preserve_from_value(source.at("excludeDirectories"),
                                                                  default_knowledge_exclude_directories());

  std::vector<std::filesystem::path> files;
  const auto should_include = [&](const std::filesystem::path& path) {
    const auto extension = extension_for_path(path);
    if (!include_extensions.empty() && !include_extensions.contains(extension)) {
      return false;
    }
    if (exclude_extensions.contains(extension)) {
      return false;
    }
    return true;
  };

  if (recursive) {
    std::filesystem::recursive_directory_iterator it(directory);
    const std::filesystem::recursive_directory_iterator end;
    for (; it != end; ++it) {
      if (it->is_directory() && exclude_directories.contains(it->path().filename().string())) {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file() && should_include(it->path())) {
        files.push_back(std::filesystem::absolute(it->path()));
      }
    }
  } else {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
      if (entry.is_regular_file() && should_include(entry.path())) {
        files.push_back(std::filesystem::absolute(entry.path()));
      }
    }
  }

  std::sort(files.begin(), files.end());
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& file : files) {
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["sourceKind"] = "directory";
    auto loaded = file_loader_->load(Value::object({{"type", "file"},
                                                   {"path", file.string()},
                                                   {"metadata", metadata}}));
    for (auto& document : loaded) {
      document.source_type = "directory";
      documents.push_back(std::move(document));
    }
  }
  return documents;
}

MarkdownKnowledgeSourceLoader::MarkdownKnowledgeSourceLoader(bool recursive)
    : recursive_(recursive) {}

bool MarkdownKnowledgeSourceLoader::supports(const Value& source) const {
  return source.at("type").as_string() == "markdown";
}

std::vector<LoadedKnowledgeDocument> MarkdownKnowledgeSourceLoader::load(const Value& source) const {
  if (!supports(source)) {
    return {};
  }
  const auto raw_path = source.at("path").as_string();
  if (raw_path.empty()) {
    throw ConfigurationError("Markdown knowledge source requires path.");
  }

  const auto path = std::filesystem::absolute(std::filesystem::path(raw_path));
  const bool recursive = source.contains("recursive") ? source.at("recursive").as_bool(recursive_) : recursive_;
  std::vector<std::filesystem::path> files;
  const auto include_markdown = [](const std::filesystem::path& candidate) {
    return markdown_knowledge_extensions().contains(extension_for_path(candidate));
  };

  if (std::filesystem::is_regular_file(path)) {
    if (include_markdown(path)) {
      files.push_back(path);
    }
  } else if (std::filesystem::is_directory(path)) {
    if (recursive) {
      std::filesystem::recursive_directory_iterator it(path);
      const std::filesystem::recursive_directory_iterator end;
      for (; it != end; ++it) {
        if (it->is_regular_file() && include_markdown(it->path())) {
          files.push_back(std::filesystem::absolute(it->path()));
        }
      }
    } else {
      for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.is_regular_file() && include_markdown(entry.path())) {
          files.push_back(std::filesystem::absolute(entry.path()));
        }
      }
    }
  } else {
    throw ConfigurationError("Markdown knowledge source path does not exist: " + path.string());
  }

  std::sort(files.begin(), files.end());
  std::vector<LoadedKnowledgeDocument> documents;
  documents.reserve(files.size());
  for (const auto& file : files) {
    auto metadata = copy_object_or_empty(source.at("metadata"));
    metadata["loader"] = "markdown";
    const auto content = bytes_to_text(read_binary_file(file));
    documents.push_back(build_loaded_document("markdown",
                                              file.string(),
                                              find_markdown_title(content, file.filename().string()),
                                              content,
                                              std::move(metadata)));
  }
  return documents;
}

KnowledgeLoaderRegistry::KnowledgeLoaderRegistry(std::vector<KnowledgeLoaderProvider> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

KnowledgeLoaderRegistry::KnowledgeLoaderRegistry(const KnowledgeLoaderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
}

KnowledgeLoaderRegistry& KnowledgeLoaderRegistry::operator=(const KnowledgeLoaderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  provider_order_ = other.provider_order_;
  return *this;
}

KnowledgeLoaderRegistry::KnowledgeLoaderRegistry(KnowledgeLoaderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
}

KnowledgeLoaderRegistry& KnowledgeLoaderRegistry::operator=(KnowledgeLoaderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  provider_order_ = std::move(other.provider_order_);
  return *this;
}

KnowledgeLoaderProvider& KnowledgeLoaderRegistry::register_provider(KnowledgeLoaderProvider provider) {
  if (provider.metadata.name.empty()) {
    throw ConfigurationError("Knowledge loader provider requires metadata.name.");
  }
  if (!provider.create) {
    throw ConfigurationError("Knowledge loader provider requires create: " + provider.metadata.name);
  }
  const auto name = provider.metadata.name;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!providers_.contains(name)) {
    provider_order_.push_back(name);
  }
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

const KnowledgeLoaderProvider* KnowledgeLoaderRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : &found->second;
}

std::shared_ptr<KnowledgeSourceLoader> KnowledgeLoaderRegistry::create(const std::string& name,
                                                                       const Value& options) const {
  KnowledgeLoaderProvider::Factory factory;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = providers_.find(name);
    if (found == providers_.end()) {
      throw ConfigurationError("Unknown knowledge loader provider: " + name);
    }
    factory = found->second.create;
  }
  auto loader = factory(options, *this);
  if (!loader) {
    throw ConfigurationError("Knowledge loader provider returned null: " + name);
  }
  return loader;
}

std::vector<KnowledgeLoaderProvider> KnowledgeLoaderRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<KnowledgeLoaderProvider> providers;
  providers.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    const auto found = providers_.find(name);
    if (found != providers_.end()) {
      providers.push_back(found->second);
    }
  }
  return providers;
}

std::vector<std::string> KnowledgeLoaderRegistry::list_names() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::string> names;
  names.reserve(provider_order_.size());
  for (const auto& name : provider_order_) {
    if (providers_.contains(name)) {
      names.push_back(name);
    }
  }
  return names;
}

CompositeKnowledgeLoader::CompositeKnowledgeLoader(std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders,
                                                   bool use_default_loaders_when_empty)
    : loaders_(std::move(loaders)) {
  if (loaders_.empty() && use_default_loaders_when_empty) {
    loaders_.push_back(std::make_shared<TextKnowledgeSourceLoader>());
    loaders_.push_back(std::make_shared<FileKnowledgeSourceLoader>());
    loaders_.push_back(std::make_shared<DirectoryKnowledgeSourceLoader>());
    loaders_.push_back(std::make_shared<MarkdownKnowledgeSourceLoader>());
  }
}

void CompositeKnowledgeLoader::add_loader(std::shared_ptr<KnowledgeSourceLoader> loader) {
  if (loader) {
    loaders_.push_back(std::move(loader));
  }
}

bool CompositeKnowledgeLoader::supports(const Value&) const {
  return true;
}

std::vector<LoadedKnowledgeDocument> CompositeKnowledgeLoader::load(const Value& source) const {
  for (const auto& loader : loaders_) {
    if (loader && loader->supports(source)) {
      return loader->load(source);
    }
  }
  throw ConfigurationError("No knowledge loader found for source type: " + source.at("type").as_string());
}

std::vector<LoadedKnowledgeDocument> load_knowledge_sources(
    const std::vector<Value>& sources,
    const KnowledgeSourceLoader& loader) {
  std::vector<LoadedKnowledgeDocument> documents;
  for (const auto& source : sources) {
    auto loaded = loader.load(source);
    documents.insert(documents.end(), std::make_move_iterator(loaded.begin()), std::make_move_iterator(loaded.end()));
  }
  return documents;
}

std::shared_ptr<CompositeKnowledgeLoader> create_default_knowledge_loader() {
  std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders = {
      std::make_shared<TextKnowledgeSourceLoader>(),
      std::make_shared<FileKnowledgeSourceLoader>(),
      std::make_shared<DirectoryKnowledgeSourceLoader>(),
      std::make_shared<MarkdownKnowledgeSourceLoader>(),
  };
  return std::make_shared<CompositeKnowledgeLoader>(std::move(loaders));
}

KnowledgeLoaderRegistry create_default_knowledge_loader_registry() {
  KnowledgeLoaderRegistry registry;
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "text",
          .tier = "core-safe",
          .title = "Inline Text Loader",
          .tags = {"loader", "text"},
      },
      .create = [](const Value&, const KnowledgeLoaderRegistry&) {
        return std::make_shared<TextKnowledgeSourceLoader>();
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "file",
          .tier = "core-safe",
          .title = "File Loader",
          .tags = {"loader", "file"},
      },
      .create = [](const Value&, const KnowledgeLoaderRegistry&) {
        return std::make_shared<FileKnowledgeSourceLoader>();
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "directory",
          .tier = "core-safe",
          .title = "Directory Loader",
          .tags = {"loader", "directory"},
      },
      .create = [](const Value&, const KnowledgeLoaderRegistry&) {
        return std::make_shared<DirectoryKnowledgeSourceLoader>();
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "markdown",
          .tier = "portable",
          .title = "Markdown Loader",
          .description = "Loads Markdown files and directories.",
          .tags = {"loader", "markdown"},
      },
      .create = [](const Value& options, const KnowledgeLoaderRegistry&) {
        return std::make_shared<MarkdownKnowledgeSourceLoader>(options.at("recursive").as_bool(true));
      },
  });
  registry.register_provider(KnowledgeLoaderProvider{
      .metadata = KnowledgeProviderMetadata{
          .name = "composite",
          .tier = "core-safe",
          .title = "Composite Loader",
          .tags = {"loader", "composite"},
      },
      .create = [](const Value& options, const KnowledgeLoaderRegistry& active_registry) {
        std::vector<std::string> loader_names = string_vector_from_value(options.at("loaders"));
        if (loader_names.empty()) {
          for (const auto& provider : active_registry.list()) {
            if (provider.metadata.name != "composite") {
              loader_names.push_back(provider.metadata.name);
            }
          }
        }

        std::vector<std::shared_ptr<KnowledgeSourceLoader>> loaders;
        for (const auto& loader_name : loader_names) {
          if (loader_name == "composite") {
            continue;
          }
          if (active_registry.get(loader_name)) {
            loaders.push_back(active_registry.create(loader_name));
          }
        }
        return std::make_shared<CompositeKnowledgeLoader>(std::move(loaders), false);
      },
  });
  return registry;
}
