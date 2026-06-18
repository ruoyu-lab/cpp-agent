#include "agent/media.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <optional>
#include <sstream>

namespace agent {

namespace {

std::string lower_copy(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string extension_from_part(const MessageContentPart& part) {
  std::string name = part.source.filename;
  if (name.empty() && part.source.kind == MediaSourceKind::Path) {
    name = part.source.path;
  }
  if (name.empty() && part.source.kind == MediaSourceKind::Url) {
    name = part.source.url;
  }
  return lower_copy(std::filesystem::path(name).extension().string());
}

bool looks_text_like(const std::string& mime_type, const std::string& extension) {
  static const std::set<std::string> text_mimes = {
      "application/json",
      "application/xml",
      "text/markdown",
      "text/csv",
  };
  static const std::set<std::string> text_extensions = {
      ".txt", ".md", ".mdx", ".json", ".html", ".htm", ".csv", ".xml", ".yaml", ".yml",
      ".toml", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".py", ".java", ".go", ".rs",
      ".cpp", ".c", ".h", ".hpp", ".cs", ".php", ".rb", ".swift", ".kt", ".scala", ".sh",
      ".sql", ".vue", ".svelte",
  };
  return mime_type.rfind("text/", 0) == 0 || text_mimes.contains(mime_type)
         || text_extensions.contains(extension);
}

std::string filename_from_part(const MessageContentPart& part, const ResolvedMedia& resolved) {
  if (!resolved.filename.empty()) {
    return resolved.filename;
  }
  if (!part.source.filename.empty()) {
    return part.source.filename;
  }
  if (part.source.kind == MediaSourceKind::Path) {
    return path_basename(part.source.path);
  }
  if (part.source.kind == MediaSourceKind::Url) {
    return path_basename(part.source.url);
  }
  return {};
}

std::vector<MessageContentPart> text_content(std::string text) {
  return {text_part(std::move(text))};
}

std::string trim_text(std::string text) {
  return trim_copy(std::move(text));
}

bool is_http_media_url(const MediaSource& source) {
  if (source.kind != MediaSourceKind::Url) {
    return false;
  }
  const auto url = lower_copy(source.url);
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

std::string header_value(const std::map<std::string, std::string>& headers, const std::string& name) {
  const auto expected = lower_copy(name);
  for (const auto& [key, value] : headers) {
    if (lower_copy(key) == expected) {
      return value;
    }
  }
  return {};
}

std::string content_type_without_parameters(std::string content_type) {
  const auto semicolon = content_type.find(';');
  if (semicolon != std::string::npos) {
    content_type = content_type.substr(0, semicolon);
  }
  return lower_copy(trim_copy(std::move(content_type)));
}

std::string filename_from_url(std::string url) {
  const auto fragment = url.find('#');
  if (fragment != std::string::npos) {
    url = url.substr(0, fragment);
  }
  const auto query = url.find('?');
  if (query != std::string::npos) {
    url = url.substr(0, query);
  }
  return path_basename(url);
}

std::string decode_pdf_literal(std::string literal) {
  std::string output;
  output.reserve(literal.size());
  bool escaped = false;
  for (std::size_t index = 0; index < literal.size(); ++index) {
    const char ch = literal[index];
    if (escaped) {
      if (ch >= '0' && ch <= '7') {
        int value = ch - '0';
        std::size_t consumed = 1;
        while (consumed < 3 && index + 1 < literal.size() &&
               literal[index + 1] >= '0' && literal[index + 1] <= '7') {
          value = value * 8 + (literal[++index] - '0');
          ++consumed;
        }
        output.push_back(static_cast<char>(value));
        escaped = false;
        continue;
      }
      switch (ch) {
        case 'n':
          output.push_back('\n');
          break;
        case 'r':
          output.push_back('\r');
          break;
        case 't':
          output.push_back('\t');
          break;
        case 'b':
          output.push_back('\b');
          break;
        case 'f':
          output.push_back('\f');
          break;
        case '\r':
          if (index + 1 < literal.size() && literal[index + 1] == '\n') {
            ++index;
          }
          break;
        case '\n':
          break;
        default:
          output.push_back(ch);
          break;
      }
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    output.push_back(ch);
  }
  return output;
}

bool parse_pdf_literal(const std::string& text, std::size_t start, std::size_t& end, std::string& value) {
  if (start >= text.size() || text[start] != '(') {
    return false;
  }
  std::string literal;
  int depth = 1;
  bool escaped = false;
  for (std::size_t index = start + 1; index < text.size(); ++index) {
    const char ch = text[index];
    if (escaped) {
      literal.push_back('\\');
      literal.push_back(ch);
      escaped = false;
      continue;
    }
    if (ch == '\\') {
      escaped = true;
      continue;
    }
    if (ch == '(') {
      ++depth;
      literal.push_back(ch);
      continue;
    }
    if (ch == ')') {
      --depth;
      if (depth == 0) {
        end = index + 1;
        value = trim_text(decode_pdf_literal(literal));
        return true;
      }
      literal.push_back(ch);
      continue;
    }
    literal.push_back(ch);
  }
  return false;
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

bool parse_pdf_hex_string(const std::string& text, std::size_t start, std::size_t& end, std::string& value) {
  if (start >= text.size() || text[start] != '<' || (start + 1 < text.size() && text[start + 1] == '<')) {
    return false;
  }
  std::string hex;
  for (std::size_t index = start + 1; index < text.size(); ++index) {
    if (text[index] == '>') {
      end = index + 1;
      if (hex.size() % 2 != 0) {
        hex.push_back('0');
      }
      std::string decoded;
      decoded.reserve(hex.size() / 2);
      for (std::size_t pos = 0; pos + 1 < hex.size(); pos += 2) {
        const int high = hex_value(hex[pos]);
        const int low = hex_value(hex[pos + 1]);
        if (high >= 0 && low >= 0) {
          decoded.push_back(static_cast<char>((high << 4) | low));
        }
      }
      value = trim_text(decoded);
      return true;
    }
    if (!std::isspace(static_cast<unsigned char>(text[index]))) {
      hex.push_back(text[index]);
    }
  }
  return false;
}

std::string join_pdf_text_tokens(const std::vector<std::string>& tokens) {
  std::ostringstream out;
  for (const auto& token : tokens) {
    const auto text = trim_text(token);
    if (text.empty()) {
      continue;
    }
    if (!out.str().empty()) {
      out << "\n";
    }
    out << text;
  }
  return trim_text(out.str());
}

std::vector<std::string> extract_pdf_strings(const std::string& content) {
  std::vector<std::string> tokens;
  for (std::size_t index = 0; index < content.size();) {
    std::size_t end = index;
    std::string value;
    if (content[index] == '(' && parse_pdf_literal(content, index, end, value)) {
      if (value.size() >= 2) {
        tokens.push_back(std::move(value));
      }
      index = end;
      continue;
    }
    if (content[index] == '<' && parse_pdf_hex_string(content, index, end, value)) {
      if (value.size() >= 2) {
        tokens.push_back(std::move(value));
      }
      index = end;
      continue;
    }
    ++index;
  }
  return tokens;
}

std::vector<std::string> extract_pdf_text_blocks(const std::string& content) {
  std::vector<std::string> blocks;
  std::size_t pos = 0;
  while ((pos = content.find("BT", pos)) != std::string::npos) {
    const auto end = content.find("ET", pos + 2);
    if (end == std::string::npos) {
      break;
    }
    blocks.push_back(content.substr(pos + 2, end - pos - 2));
    pos = end + 2;
  }
  return blocks;
}

std::optional<std::string> flate_decode_pdf_stream(const std::string& data) {
  (void)data;
  return std::nullopt;
}

std::vector<std::string> extract_pdf_stream_contents(const std::string& raw) {
  std::vector<std::string> streams;
  std::size_t pos = 0;
  while ((pos = raw.find("stream", pos)) != std::string::npos) {
    auto data_start = pos + 6;
    if (data_start < raw.size() && raw[data_start] == '\r') {
      ++data_start;
      if (data_start < raw.size() && raw[data_start] == '\n') {
        ++data_start;
      }
    } else if (data_start < raw.size() && raw[data_start] == '\n') {
      ++data_start;
    }
    const auto data_end = raw.find("endstream", data_start);
    if (data_end == std::string::npos) {
      break;
    }
    std::string data = raw.substr(data_start, data_end - data_start);
    while (!data.empty() && (data.back() == '\n' || data.back() == '\r')) {
      data.pop_back();
    }
    const auto dictionary_start = raw.rfind("<<", pos);
    const std::string dictionary = dictionary_start == std::string::npos ? std::string{} : raw.substr(dictionary_start, pos - dictionary_start);
    if (dictionary.find("/FlateDecode") != std::string::npos) {
      if (auto decoded = flate_decode_pdf_stream(data)) {
        streams.push_back(std::move(*decoded));
      }
    } else {
      streams.push_back(std::move(data));
    }
    pos = data_end + 9;
  }
  return streams;
}

std::string extract_pdf_text_from_content(const std::string& content) {
  std::vector<std::string> tokens;
  const auto blocks = extract_pdf_text_blocks(content);
  if (!blocks.empty()) {
    for (const auto& block : blocks) {
      auto block_tokens = extract_pdf_strings(block);
      tokens.insert(tokens.end(), std::make_move_iterator(block_tokens.begin()), std::make_move_iterator(block_tokens.end()));
    }
  } else {
    tokens = extract_pdf_strings(content);
  }
  return join_pdf_text_tokens(tokens);
}

}  // namespace

std::string to_string(OCRMode mode) {
  switch (mode) {
    case OCRMode::Off:
      return "off";
    case OCRMode::Fallback:
      return "fallback";
    case OCRMode::Force:
      return "force";
  }
  return "fallback";
}

OCRMode ocr_mode_from_string(const std::string& mode) {
  if (mode == "off") return OCRMode::Off;
  if (mode == "force") return OCRMode::Force;
  return OCRMode::Fallback;
}

OCRProviderRegistry::OCRProviderRegistry(std::vector<std::shared_ptr<OCRProvider>> providers) {
  for (auto& provider : providers) {
    register_provider(std::move(provider));
  }
}

OCRProviderRegistry::OCRProviderRegistry(const OCRProviderRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = other.providers_;
}

OCRProviderRegistry& OCRProviderRegistry::operator=(const OCRProviderRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = other.providers_;
  return *this;
}

OCRProviderRegistry::OCRProviderRegistry(OCRProviderRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  providers_ = std::move(other.providers_);
}

OCRProviderRegistry& OCRProviderRegistry::operator=(OCRProviderRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  providers_ = std::move(other.providers_);
  return *this;
}

std::shared_ptr<OCRProvider> OCRProviderRegistry::register_provider(std::shared_ptr<OCRProvider> provider) {
  if (!provider) {
    throw ConfigurationError("OCR provider cannot be null.");
  }
  if (provider->metadata().name.empty()) {
    throw ConfigurationError("OCR provider metadata.name is required.");
  }
  const auto name = provider->metadata().name;
  std::lock_guard<std::mutex> lock(mutex_);
  providers_[name] = std::move(provider);
  return providers_.at(name);
}

std::shared_ptr<OCRProvider> OCRProviderRegistry::get(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = providers_.find(name);
  return found == providers_.end() ? nullptr : found->second;
}

std::vector<std::shared_ptr<OCRProvider>> OCRProviderRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<OCRProvider>> providers;
  providers.reserve(providers_.size());
  for (const auto& [_, provider] : providers_) {
    providers.push_back(provider);
  }
  return providers;
}

std::shared_ptr<OCRProvider> resolve_ocr_provider(const OCRProviderRegistry* registry,
                                                  const std::string& preferred) {
  if (!registry || preferred.empty()) {
    return nullptr;
  }
  return registry->get(preferred);
}

std::shared_ptr<OCRProvider> require_ocr_provider(const OCRProviderRegistry* registry,
                                                  const std::string& preferred) {
  auto provider = resolve_ocr_provider(registry, preferred);
  if (!provider) {
    throw ConfigurationError("OCR provider is not configured.");
  }
  return provider;
}

DocumentRasterizerRegistry::DocumentRasterizerRegistry(
    std::vector<std::shared_ptr<DocumentRasterizer>> rasterizers) {
  for (auto& rasterizer : rasterizers) {
    register_rasterizer(std::move(rasterizer));
  }
}

DocumentRasterizerRegistry::DocumentRasterizerRegistry(const DocumentRasterizerRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  rasterizers_ = other.rasterizers_;
}

DocumentRasterizerRegistry& DocumentRasterizerRegistry::operator=(const DocumentRasterizerRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  rasterizers_ = other.rasterizers_;
  return *this;
}

DocumentRasterizerRegistry::DocumentRasterizerRegistry(DocumentRasterizerRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  rasterizers_ = std::move(other.rasterizers_);
}

DocumentRasterizerRegistry& DocumentRasterizerRegistry::operator=(DocumentRasterizerRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  rasterizers_ = std::move(other.rasterizers_);
  return *this;
}

std::shared_ptr<DocumentRasterizer> DocumentRasterizerRegistry::register_rasterizer(
    std::shared_ptr<DocumentRasterizer> rasterizer) {
  if (!rasterizer) {
    throw ConfigurationError("Document rasterizer cannot be null.");
  }
  if (rasterizer->metadata().name.empty()) {
    throw ConfigurationError("Document rasterizer metadata.name is required.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  rasterizers_.push_back(std::move(rasterizer));
  return rasterizers_.back();
}

std::shared_ptr<DocumentRasterizer> DocumentRasterizerRegistry::get(const ResolvedMedia& document) const {
  const auto rasterizers = list();
  for (const auto& rasterizer : rasterizers) {
    if (rasterizer->supports(document)) {
      return rasterizer;
    }
  }
  return nullptr;
}

std::vector<std::shared_ptr<DocumentRasterizer>> DocumentRasterizerRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return rasterizers_;
}

DocumentPreprocessorRegistry::DocumentPreprocessorRegistry(
    std::vector<std::shared_ptr<DocumentPreprocessor>> preprocessors) {
  for (auto& preprocessor : preprocessors) {
    register_preprocessor(std::move(preprocessor));
  }
}

DocumentPreprocessorRegistry::DocumentPreprocessorRegistry(const DocumentPreprocessorRegistry& other) {
  std::lock_guard<std::mutex> lock(other.mutex_);
  preprocessors_ = other.preprocessors_;
}

DocumentPreprocessorRegistry& DocumentPreprocessorRegistry::operator=(const DocumentPreprocessorRegistry& other) {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  preprocessors_ = other.preprocessors_;
  return *this;
}

DocumentPreprocessorRegistry::DocumentPreprocessorRegistry(DocumentPreprocessorRegistry&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  preprocessors_ = std::move(other.preprocessors_);
}

DocumentPreprocessorRegistry& DocumentPreprocessorRegistry::operator=(DocumentPreprocessorRegistry&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::scoped_lock lock(mutex_, other.mutex_);
  preprocessors_ = std::move(other.preprocessors_);
  return *this;
}

std::shared_ptr<DocumentPreprocessor> DocumentPreprocessorRegistry::register_preprocessor(
    std::shared_ptr<DocumentPreprocessor> preprocessor) {
  if (!preprocessor) {
    throw ConfigurationError("Document preprocessor cannot be null.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  preprocessors_.push_back(std::move(preprocessor));
  return preprocessors_.back();
}

std::shared_ptr<DocumentPreprocessor> DocumentPreprocessorRegistry::get(const MessageContentPart& part) const {
  const auto preprocessors = list();
  for (const auto& preprocessor : preprocessors) {
    if (preprocessor->supports(part)) {
      return preprocessor;
    }
  }
  return nullptr;
}

std::vector<std::shared_ptr<DocumentPreprocessor>> DocumentPreprocessorRegistry::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return preprocessors_;
}

PreprocessedDocument preprocess_document_part(const DocumentPreprocessorRegistry* registry,
                                              const MessageContentPart& part,
                                              const DocumentPreprocessContext& context) {
  const auto preprocessor = registry ? registry->get(part) : nullptr;
  if (!preprocessor) {
    throw ConfigurationError("No document preprocessor is registered for the provided file part.");
  }
  return preprocessor->process(part, context);
}

MediaResolverFunction create_native_http_media_resolver(HttpTransport transport) {
  if (!transport) {
    throw ConfigurationError("Native HTTP media resolver requires an HttpTransport.");
  }
  return [transport = std::move(transport)](
             const MediaSource& source,
             DefaultMediaResolver::ArtifactLookup artifact_lookup) -> ResolvedMedia {
    DefaultMediaResolver fallback;
    if (!is_http_media_url(source)) {
      return fallback.resolve(source, std::move(artifact_lookup));
    }

    HttpRequest request;
    request.url = source.url;
    request.method = "GET";
    request.headers = {{"accept", "*/*"}};
    const auto response = transport(request);
    if (response.status < 200 || response.status >= 300) {
      throw AdapterError("Native HTTP media fetch failed with status " +
                         std::to_string(response.status) + ".", source.url);
    }

    ResolvedMedia resolved;
    resolved.source = source;
    resolved.bytes = text_to_bytes(response.body);
    resolved.mime_type = source.mime_type.empty()
                             ? content_type_without_parameters(header_value(response.headers, "content-type"))
                             : source.mime_type;
    if (resolved.mime_type.empty()) {
      resolved.mime_type = DefaultMediaResolver::extension_to_mime(filename_from_url(source.url));
    }
    if (resolved.mime_type.empty()) {
      resolved.mime_type = "application/octet-stream";
    }
    resolved.filename = source.filename.empty() ? filename_from_url(source.url) : source.filename;
    resolved.text = DefaultMediaResolver::try_decode_text(resolved.bytes, resolved.mime_type);
    return resolved;
  };
}

DefaultDocumentPreprocessor::DefaultDocumentPreprocessor(OCRMode default_ocr_mode)
    : default_ocr_mode_(default_ocr_mode) {}

bool DefaultDocumentPreprocessor::supports(const MessageContentPart& part) const {
  return part.type == ContentPartType::File;
}

ResolvedMedia DefaultDocumentPreprocessor::resolve_media(const MessageContentPart& part,
                                                         const DocumentPreprocessContext& context) const {
  if (context.media_resolver) {
    return context.media_resolver(part.source, context.artifact_lookup);
  }
  if (is_http_media_url(part.source)) {
    throw ConfigurationError("HTTP media URL preprocessing requires an injected media resolver.");
  }
  return resolver_.resolve(part.source, context.artifact_lookup);
}

std::shared_ptr<OCRProvider> DefaultDocumentPreprocessor::resolve_provider(
    const DocumentPreprocessContext& context,
    OCRMode mode) const {
  if (mode == OCRMode::Off) {
    return nullptr;
  }
  if (context.default_ocr_provider) {
    return context.default_ocr_provider;
  }
  if (!context.default_ocr_provider_name.empty()) {
    return resolve_ocr_provider(context.ocr_registry, context.default_ocr_provider_name);
  }
  return nullptr;
}

std::string extract_pdf_text_from_bytes(const std::vector<std::uint8_t>& bytes) {
  const std::string raw = bytes_to_text(bytes);
  std::vector<std::string> extracted;
  for (const auto& stream : extract_pdf_stream_contents(raw)) {
    auto text = extract_pdf_text_from_content(stream);
    if (!text.empty()) {
      extracted.push_back(std::move(text));
    }
  }
  if (extracted.empty()) {
    auto text = extract_pdf_text_from_content(raw);
    if (!text.empty()) {
      extracted.push_back(std::move(text));
    }
  }
  return join_pdf_text_tokens(extracted);
}

PreprocessedDocument DefaultDocumentPreprocessor::process(const MessageContentPart& part,
                                                          const DocumentPreprocessContext& context) {
  if (!supports(part)) {
    throw ConfigurationError("DefaultDocumentPreprocessor only supports file content parts.");
  }

  const auto resolved = resolve_media(part, context);
  const std::string extension = extension_from_part(part);
  const OCRMode ocr_mode = context.ocr_mode.value_or(default_ocr_mode_);
  const auto ocr_provider = resolve_provider(context, ocr_mode);

  PreprocessedDocument document;
  document.source = part.source;
  document.mime_type = resolved.mime_type;
  document.filename = filename_from_part(part, resolved);

  if (resolved.mime_type.rfind("image/", 0) == 0) {
    auto image = image_part(part.source, part.title.empty() ? part.text_hint : part.title, {}, part.metadata);
    if (ocr_mode != OCRMode::Off && ocr_provider) {
      auto ocr = ocr_provider->recognize(OCRRequest{
          .images = {OCRRequestImage{.media = resolved, .source = part.source, .page_number = 1}},
      });
      const auto text = trim_text(ocr.text);
      if (!text.empty()) {
        document.content = {text_part(text), image};
        document.ocr_text = text;
        document.used_ocr = true;
        document.ocr_provider = ocr_provider->metadata().name;
        return document;
      }
    }
    document.content = {image};
    return document;
  }

  if (looks_text_like(resolved.mime_type, extension)) {
    std::string text = !resolved.text.empty() ? resolved.text : bytes_to_text(resolved.bytes);
    if (resolved.mime_type == "text/html") {
      text = html_to_text(std::move(text));
    }
    document.content = text_content(text);
    document.extracted_text = text;
    document.used_native_text = true;
    return document;
  }

  if (resolved.mime_type == "application/pdf" || extension == ".pdf") {
    const auto native_text = extract_pdf_text_from_bytes(resolved.bytes);
    if (!native_text.empty() && ocr_mode != OCRMode::Force) {
      document.mime_type = "application/pdf";
      document.content = text_content(native_text);
      document.extracted_text = native_text;
      document.used_native_text = true;
      return document;
    }

    if (ocr_mode != OCRMode::Off && ocr_provider && context.document_rasterizers) {
      const auto rasterizer = context.document_rasterizers->get(resolved);
      if (rasterizer) {
        auto pages = rasterizer->rasterize(RasterizeDocumentRequest{
            .document = resolved,
            .source = part.source,
        });
        if (!pages.empty()) {
          OCRRequest request;
          request.images.reserve(pages.size());
          for (const auto& page : pages) {
            request.images.push_back(OCRRequestImage{
                .media = page.media,
                .source = page.source,
                .page_number = page.page_number,
            });
          }
          auto ocr = ocr_provider->recognize(request);
          const auto text = trim_text(ocr.text);
          if (!text.empty()) {
            document.mime_type = "application/pdf";
            document.content = text_content(text);
            document.ocr_text = text;
            document.used_ocr = true;
            document.rasterizer = rasterizer->metadata().name;
            document.ocr_provider = ocr_provider->metadata().name;
            return document;
          }
        }
      }
    }

    if (!native_text.empty()) {
      document.mime_type = "application/pdf";
      document.content = text_content(native_text);
      document.extracted_text = native_text;
      document.used_native_text = true;
      return document;
    }
  }

  throw ConfigurationError("Document preprocessor cannot convert \"" + resolved.mime_type +
                           "\" into model input.");
}

}  // namespace agent
