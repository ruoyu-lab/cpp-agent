namespace {

int dimensions_from_settings(const Value& settings, int fallback) {
  if (settings.is_object() && settings.at("dimensions").is_number()) {
    return std::max(1, static_cast<int>(settings.at("dimensions").as_integer()));
  }
  return std::max(1, fallback);
}

std::string image_source_hint(const MediaSource& source) {
  if (source.kind == MediaSourceKind::Inline) {
    if (!source.filename.empty()) {
      return source.filename;
    }
    return source.mime_type;
  }
  if (source.kind == MediaSourceKind::Artifact) {
    return source.key;
  }
  if (source.kind == MediaSourceKind::Path) {
    return source.path;
  }
  return source.url;
}

std::string image_to_hint(const ImageEmbeddingInput& image) {
  std::vector<std::string> parts{image.title, image.alt_text, image.text_hint, image_source_hint(image.source)};
  std::string output;
  for (const auto& part : parts) {
    if (part.empty()) {
      continue;
    }
    if (!output.empty()) {
      output += " ";
    }
    output += part;
  }
  return output;
}

EmbeddingSettings merge_embedding_settings(std::string provider, std::string model, int dimensions,
                                           std::string space_id, EmbeddingSettings settings,
                                           std::vector<std::string> modalities) {
  if (settings.model.empty()) {
    settings.model = std::move(model);
  }
  if (!settings.dimensions) {
    settings.dimensions = dimensions;
  }
  if (settings.space_id.empty()) {
    settings.space_id = space_id.empty() ? provider + ":" + settings.model : std::move(space_id);
  }
  settings.extra["modalities"] = Value::array({});
  for (const auto& modality : modalities) {
    settings.extra["modalities"].as_array().push_back(modality);
  }
  return settings;
}

}  // namespace
