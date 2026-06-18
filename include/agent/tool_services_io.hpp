#pragma once

#include "agent/messages.hpp"
#include "agent/tools.hpp"

namespace agent {

class BrowserRenderer;
class DocumentPreprocessorRegistry;
class DocumentRasterizerRegistry;
class MediaGenerationProviderRegistry;
class NativeWebCrawler;
class NativeWebPageFetcher;
class OCRProviderRegistry;
class WebSearchProviderRegistry;

inline const ToolServiceToken<WebSearchProviderRegistry> kToolServiceWebSearchRegistry{
    "web.search.registry"};
inline const ToolServiceToken<std::string> kToolServiceDefaultSearchProvider{
    "web.search.default_provider"};
inline const ToolServiceToken<NativeWebPageFetcher> kToolServiceWebFetcher{"web.fetcher"};
inline const ToolServiceToken<NativeWebCrawler> kToolServiceWebCrawler{"web.crawler"};
inline const ToolServiceToken<Value> kToolServiceDefaultCrawlerProfile{
    "web.crawler.default_profile"};
inline const ToolServiceToken<BrowserRenderer> kToolServiceBrowserRenderer{"browser.renderer"};
inline const ToolServiceToken<DefaultMediaResolver::ArtifactLookup> kToolServiceMediaArtifactLookup{
    "media.artifact.lookup"};
inline const ToolServiceToken<OCRProviderRegistry> kToolServiceOcrRegistry{"media.ocr.registry"};
inline const ToolServiceToken<std::string> kToolServiceDefaultOcrProvider{
    "media.ocr.default_provider"};
inline const ToolServiceToken<DocumentRasterizerRegistry> kToolServiceDocumentRasterizers{
    "document.rasterizers"};
inline const ToolServiceToken<DocumentPreprocessorRegistry> kToolServiceDocumentPreprocessors{
    "document.preprocessors"};
inline const ToolServiceToken<MediaGenerationProviderRegistry> kToolServiceMediaGenerationRegistry{
    "media.generation.registry"};
inline const ToolServiceToken<std::string> kToolServiceDefaultMediaGenerationProvider{
    "media.generation.default_provider"};

}  // namespace agent
