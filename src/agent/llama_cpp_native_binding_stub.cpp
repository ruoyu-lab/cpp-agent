#include "agent/model_providers.hpp"

namespace agent {

std::shared_ptr<LlamaCppNativeBinding> create_llama_cpp_native_binding() {
  auto binding = std::make_shared<LlamaCppNativeBinding>();
  binding->generate_chat = [](const LlamaCppNativeRuntimeConfig&,
                              const LlamaCppNativeChatRequest&,
                              LlamaCppNativeBinding::ChatDeltaHandler) {
    throw ConfigurationError(
        "Built-in llama.cpp native binding is not enabled. Reconfigure with "
        "-DAGENT_NATIVE_ENABLE_LLAMA_CPP=ON and provide LLAMA_CPP_INCLUDE_DIR and "
        "GGML_INCLUDE_DIR, or inject LlamaCppNativeBinding yourself.");
    return LlamaCppNativeChatResult{};
  };
  binding->embed_texts = [](const LlamaCppNativeRuntimeConfig&,
                            const LlamaCppNativeEmbeddingRequest&) {
    throw ConfigurationError(
        "Built-in llama.cpp native binding is not enabled. Reconfigure with "
        "-DAGENT_NATIVE_ENABLE_LLAMA_CPP=ON and provide LLAMA_CPP_INCLUDE_DIR and "
        "GGML_INCLUDE_DIR, or inject LlamaCppNativeBinding yourself.");
    return LlamaCppNativeEmbeddingResult{};
  };
  binding->cancel = [](const std::string&) {};
  return binding;
}

}  // namespace agent
