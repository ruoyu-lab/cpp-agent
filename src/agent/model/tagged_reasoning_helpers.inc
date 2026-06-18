namespace {

std::size_t trailing_tag_prefix_length(const std::string& value, const std::string& tag) {
  const auto max_length = std::min(value.size(), tag.empty() ? std::size_t{0} : tag.size() - 1);
  for (std::size_t length = max_length; length > 0; --length) {
    if (value.compare(value.size() - length, length, tag, 0, length) == 0) {
      return length;
    }
  }
  return 0;
}

}  // namespace
