#pragma once

#include "agent/http.hpp"
#include "agent/messages.hpp"

#include <mutex>

namespace agent {

enum class OCRMode {
  Off,
  Fallback,
  Force,
};

std::string to_string(OCRMode mode);
OCRMode ocr_mode_from_string(const std::string& mode);

struct OCRBoundingBox {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct OCRRegion {
  std::string text;
  std::optional<double> confidence;
  std::optional<std::size_t> page_number;
  std::optional<OCRBoundingBox> bbox;
};

struct OCRRequestImage {
  ResolvedMedia media;
  MediaSource source;
  std::size_t page_number = 0;
};

struct OCRRequest {
  std::vector<OCRRequestImage> images;
  std::vector<std::string> languages;
  Value metadata = Value::object({});
};

struct OCRResult {
  std::string text;
  std::vector<OCRRegion> regions;
  Value metadata = Value::object({});
};

struct OCRProviderMetadata {
  std::string name;
  std::string tier = "portable";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
};

class OCRProvider {
 public:
  virtual ~OCRProvider() = default;
  [[nodiscard]] virtual const OCRProviderMetadata& metadata() const = 0;
  virtual OCRResult recognize(const OCRRequest& request) = 0;
};

class OCRProviderRegistry {
 public:
  explicit OCRProviderRegistry(std::vector<std::shared_ptr<OCRProvider>> providers = {});
  OCRProviderRegistry(const OCRProviderRegistry& other);
  OCRProviderRegistry& operator=(const OCRProviderRegistry& other);
  OCRProviderRegistry(OCRProviderRegistry&& other) noexcept;
  OCRProviderRegistry& operator=(OCRProviderRegistry&& other) noexcept;
  std::shared_ptr<OCRProvider> register_provider(std::shared_ptr<OCRProvider> provider);
  [[nodiscard]] std::shared_ptr<OCRProvider> get(const std::string& name) const;
  [[nodiscard]] std::vector<std::shared_ptr<OCRProvider>> list() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<OCRProvider>> providers_;
};

std::shared_ptr<OCRProvider> resolve_ocr_provider(const OCRProviderRegistry* registry,
                                                  const std::string& preferred = {});
std::shared_ptr<OCRProvider> require_ocr_provider(const OCRProviderRegistry* registry,
                                                  const std::string& preferred = {});

struct RasterizedDocumentPage {
  std::size_t page_number = 0;
  ResolvedMedia media;
  MediaSource source;
};

struct RasterizeDocumentRequest {
  ResolvedMedia document;
  MediaSource source;
  Value metadata = Value::object({});
};

struct DocumentRasterizerMetadata {
  std::string name;
  std::string tier = "portable";
  std::string title;
  std::string description;
  std::vector<std::string> tags;
};

class DocumentRasterizer {
 public:
  virtual ~DocumentRasterizer() = default;
  [[nodiscard]] virtual const DocumentRasterizerMetadata& metadata() const = 0;
  [[nodiscard]] virtual bool supports(const ResolvedMedia& document) const = 0;
  virtual std::vector<RasterizedDocumentPage> rasterize(const RasterizeDocumentRequest& request) = 0;
};

class DocumentRasterizerRegistry {
 public:
  explicit DocumentRasterizerRegistry(std::vector<std::shared_ptr<DocumentRasterizer>> rasterizers = {});
  DocumentRasterizerRegistry(const DocumentRasterizerRegistry& other);
  DocumentRasterizerRegistry& operator=(const DocumentRasterizerRegistry& other);
  DocumentRasterizerRegistry(DocumentRasterizerRegistry&& other) noexcept;
  DocumentRasterizerRegistry& operator=(DocumentRasterizerRegistry&& other) noexcept;
  std::shared_ptr<DocumentRasterizer> register_rasterizer(std::shared_ptr<DocumentRasterizer> rasterizer);
  [[nodiscard]] std::shared_ptr<DocumentRasterizer> get(const ResolvedMedia& document) const;
  [[nodiscard]] std::vector<std::shared_ptr<DocumentRasterizer>> list() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<DocumentRasterizer>> rasterizers_;
};

struct DocumentPreprocessContext {
  DefaultMediaResolver::ArtifactLookup artifact_lookup;
  std::function<ResolvedMedia(const MediaSource&, DefaultMediaResolver::ArtifactLookup)> media_resolver;
  OCRProviderRegistry* ocr_registry = nullptr;
  std::string default_ocr_provider_name;
  std::shared_ptr<OCRProvider> default_ocr_provider;
  DocumentRasterizerRegistry* document_rasterizers = nullptr;
  std::optional<OCRMode> ocr_mode;
};

using MediaResolverFunction =
    std::function<ResolvedMedia(const MediaSource&, DefaultMediaResolver::ArtifactLookup)>;

struct PreprocessedDocument {
  MediaSource source;
  std::vector<MessageContentPart> content;
  std::string mime_type;
  std::string filename;
  std::string extracted_text;
  std::string ocr_text;
  bool used_native_text = false;
  bool used_ocr = false;
  std::string rasterizer;
  std::string ocr_provider;
};

[[nodiscard]] std::string extract_pdf_text_from_bytes(const std::vector<std::uint8_t>& bytes);
MediaResolverFunction create_native_http_media_resolver(HttpTransport transport = create_native_http_transport());

class DocumentPreprocessor {
 public:
  virtual ~DocumentPreprocessor() = default;
  [[nodiscard]] virtual bool supports(const MessageContentPart& part) const = 0;
  virtual PreprocessedDocument process(const MessageContentPart& part,
                                       const DocumentPreprocessContext& context = {}) = 0;
};

class DocumentPreprocessorRegistry {
 public:
  explicit DocumentPreprocessorRegistry(std::vector<std::shared_ptr<DocumentPreprocessor>> preprocessors = {});
  DocumentPreprocessorRegistry(const DocumentPreprocessorRegistry& other);
  DocumentPreprocessorRegistry& operator=(const DocumentPreprocessorRegistry& other);
  DocumentPreprocessorRegistry(DocumentPreprocessorRegistry&& other) noexcept;
  DocumentPreprocessorRegistry& operator=(DocumentPreprocessorRegistry&& other) noexcept;
  std::shared_ptr<DocumentPreprocessor> register_preprocessor(std::shared_ptr<DocumentPreprocessor> preprocessor);
  [[nodiscard]] std::shared_ptr<DocumentPreprocessor> get(const MessageContentPart& part) const;
  [[nodiscard]] std::vector<std::shared_ptr<DocumentPreprocessor>> list() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<DocumentPreprocessor>> preprocessors_;
};

PreprocessedDocument preprocess_document_part(const DocumentPreprocessorRegistry* registry,
                                              const MessageContentPart& part,
                                              const DocumentPreprocessContext& context = {});

class DefaultDocumentPreprocessor : public DocumentPreprocessor {
 public:
  explicit DefaultDocumentPreprocessor(OCRMode default_ocr_mode = OCRMode::Fallback);
  [[nodiscard]] bool supports(const MessageContentPart& part) const override;
  PreprocessedDocument process(const MessageContentPart& part,
                               const DocumentPreprocessContext& context = {}) override;

 private:
  [[nodiscard]] ResolvedMedia resolve_media(const MessageContentPart& part,
                                            const DocumentPreprocessContext& context) const;
  [[nodiscard]] std::shared_ptr<OCRProvider> resolve_provider(const DocumentPreprocessContext& context,
                                                              OCRMode mode) const;

  DefaultMediaResolver resolver_;
  OCRMode default_ocr_mode_;
};

}  // namespace agent
