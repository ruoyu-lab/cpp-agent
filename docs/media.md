# Media API

The native media module mirrors the NodeJS media, OCR, document rasterizer, and
media generation interfaces while keeping production extractors and generators
injectable. It resolves media sources, preprocesses file parts into model-ready
content, coordinates OCR or rasterizer providers when native text extraction is
not enough, and exposes image/video/audio generation as a provider protocol
separate from chat models.

## Media Resolution

Media sources come from message content parts:

```cpp
agent::MediaSource source;
source.kind = agent::MediaSourceKind::Path;
source.path = "/workspace/report.txt";

agent::DefaultMediaResolver resolver;
auto resolved = resolver.resolve(source);
```

`DefaultMediaResolver` supports inline base64 data, data URLs, file URLs, local
paths, and artifact lookups. `create_native_http_media_resolver` adds
zero-dependency plain-HTTP fetching for URL sources when the host includes
`agent/media_native.hpp` and links `agent_runtime_io_native`. HTTPS/TLS media
fetching must be provided through an injected resolver or HTTP transport.

`ResolvedMedia` preserves the original source, MIME type, filename, bytes, and
decoded text when the payload is text-like.

## Media Generation Providers

`MediaGenerationProvider` is independent from `ChatModelAdapter`. Providers
declare `MediaGenerationCapabilities`, validate a
`MediaGenerationRequest`, and generate a `AgentOutput` with image,
video, or audio artifacts:

```cpp
class MyMediaGenerator final : public agent::MediaGenerationProvider {
 public:
  const agent::MediaGenerationProviderMetadata& metadata() const override {
    return metadata_;
  }

  agent::MediaGenerationCapabilities capabilities() const override {
    return agent::MediaGenerationCapabilities{
        .kinds = {agent::MediaGenerationKind::Image, agent::MediaGenerationKind::Audio},
        .models = {"studio-v1"},
        .mime_types = {"image/png", "audio/wav"},
    };
  }

  agent::AgentOutput generate(
      const agent::MediaGenerationRequest& request,
      const agent::MediaGenerationContext& context = {}) override;

 private:
  agent::MediaGenerationProviderMetadata metadata_{.name = "studio", .title = "Studio"};
};

agent::MediaGenerationProviderRegistry registry;
registry.register_provider(std::make_shared<MyMediaGenerator>());
```

`MediaGenerationContext` carries a `CancellationToken*`, an optional progress
callback, and host metadata. The default `validate` implementation checks
provider kind support, prompt presence, count, dimensions, duration, and audio
sample rate; providers can extend it for provider-specific constraints.

Generated media is expressed as `AgentArtifact` records with `MediaSource`
payloads. Artifact delivery avoids forcing UI layers to consume large inline
base64:

```cpp
agent::InMemoryArtifactStore artifacts;
agent::MediaGenerationRequest request;
request.kind = agent::MediaGenerationKind::Image;
request.prompt = "diagram";

auto result = generator.generate(request);
agent::MediaGenerationOutputValueOptions options;
options.artifact_writer = agent::create_in_memory_media_artifact_writer(&artifacts);
options.artifact_key_prefix = "media/generated";
auto materialized = agent::materialize_media_generation_output(result, options);
auto value = agent::media_generation_output_to_value(materialized);
```

When an artifact writer is supplied, generated media is stored as artifact
values and result/content surfaces use `MediaSourceKind::Artifact`. Public
result serialization omits inline `data` unless the data lives behind an
artifact reference.

## OCR Providers

Implement `OCRProvider` to plug in host OCR:

```cpp
class MyOCR final : public agent::OCRProvider {
 public:
  const agent::OCRProviderMetadata& metadata() const override { return metadata_; }

  agent::OCRResult recognize(const agent::OCRRequest& request) override {
    return agent::OCRResult{.text = "recognized text"};
  }

 private:
  agent::OCRProviderMetadata metadata_{.name = "host-ocr"};
};

agent::OCRProviderRegistry registry;
registry.register_provider(std::make_shared<MyOCR>());
```

`OCRMode` controls use of OCR during preprocessing:

- `Off`: never use OCR.
- `Fallback`: use native extraction first, then OCR when needed.
- `Force`: prefer OCR/rasterized OCR for supported documents.

## Document Rasterizers

Implement `DocumentRasterizer` to convert documents such as PDFs into page
images for OCR:

```cpp
class MyRasterizer final : public agent::DocumentRasterizer {
 public:
  const agent::DocumentRasterizerMetadata& metadata() const override { return metadata_; }
  bool supports(const agent::ResolvedMedia& document) const override {
    return document.mime_type == "application/pdf";
  }
  std::vector<agent::RasterizedDocumentPage> rasterize(
      const agent::RasterizeDocumentRequest& request) override;

 private:
  agent::DocumentRasterizerMetadata metadata_{.name = "host-rasterizer"};
};
```

The registry selects the first rasterizer whose `supports` method accepts the
resolved document.

## Document Preprocessing

`DefaultDocumentPreprocessor` converts file content parts into text or image
parts:

```cpp
agent::DocumentPreprocessorRegistry preprocessors({
    std::make_shared<agent::DefaultDocumentPreprocessor>(),
});

auto document = agent::preprocess_document_part(
    &preprocessors,
    agent::file_part(source, "Report"),
    agent::DocumentPreprocessContext{.ocr_registry = &registry});
```

Behavior:

- Images remain image parts. If OCR is enabled and returns text, text is
  prepended before the image part.
- Text-like files become text parts. HTML is normalized to visible text.
- PDFs use native text extraction when available. If forced or native text is
  missing, a configured rasterizer plus OCR provider can produce text.
- Unsupported media types fail explicitly instead of returning empty content.

`PreprocessedDocument` reports which path was used with `used_native_text`,
`used_ocr`, `rasterizer`, `ocr_provider`, and extracted text fields.

## Builtin Tools

`create_media_generation_builtin_tools` exposes the provider protocol through
standard tools:

- `media.generateImage`
- `media.generateVideo`
- `media.generateAudio`

The tools can capture a registry directly or read
`ToolExecutionServices::media_generation_registry` at invocation time. They
forward `CancellationToken`, emit `media.generation.progress` tool progress
events, validate requests through the provider, and return artifact-friendly
structured values plus image/video/audio content parts when assets have
resolvable sources.

## Zero-Dependency Boundary

The C++ runtime includes interfaces, registries, basic MIME inference, native
text decoding, simple PDF text extraction, and plain-HTTP media fetching. It
does not link OCR engines, PDF rasterization engines, browser automation,
media generation backends, or model-specific media processors by default.
Those are injected through the provider interfaces above.
