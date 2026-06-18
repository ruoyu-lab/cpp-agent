namespace {
EmbeddingSettings embedding_settings_from_value(const Value& value) {
  EmbeddingSettings settings;
  if (!value.is_object()) {
    return settings;
  }
  settings.model = value.at("model").as_string();
  if (value.at("dimensions").is_number()) {
    settings.dimensions = std::max(1, static_cast<int>(value.at("dimensions").as_integer()));
  }
  settings.task_type = value.at("taskType").as_string(value.at("task_type").as_string());
  settings.space_id = value.at("spaceId").as_string(value.at("space_id").as_string());
  settings.extra = value;
  return settings;
}

void throw_if_model_cancelled(CancellationToken* cancellation) {
  if (cancellation) {
    cancellation->throw_if_cancelled(ExecutionTarget::Model);
  }
}

}  // namespace
