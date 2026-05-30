#include "agent/agent.hpp"

#include <ggml-backend.h>
#include <llama.h>

#include <atomic>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace agent {
namespace {

struct NativeError {
  bool has_error = false;
  std::string message;

  void set(std::string value) {
    if (!has_error) {
      has_error = true;
      message = std::move(value);
    }
  }

  void throw_if_failed() const {
    if (has_error) {
      throw AdapterError(message);
    }
  }
};

struct CancelToken {
  std::atomic<bool> cancelled{false};
};

std::mutex g_cancel_mutex;
std::unordered_map<std::string, std::shared_ptr<CancelToken>> g_cancel_tokens;

std::shared_ptr<CancelToken> register_cancel_token(const std::string& request_id) {
  auto token = std::make_shared<CancelToken>();
  std::lock_guard<std::mutex> lock(g_cancel_mutex);
  g_cancel_tokens[request_id] = token;
  return token;
}

void unregister_cancel_token(const std::string& request_id) {
  std::lock_guard<std::mutex> lock(g_cancel_mutex);
  g_cancel_tokens.erase(request_id);
}

void cancel_request(const std::string& request_id) {
  std::lock_guard<std::mutex> lock(g_cancel_mutex);
  const auto found = g_cancel_tokens.find(request_id);
  if (found != g_cancel_tokens.end() && found->second) {
    found->second->cancelled.store(true);
  }
}

struct CancelRegistration {
  std::string request_id;
  std::shared_ptr<CancelToken> token;

  explicit CancelRegistration(std::string id)
      : request_id(std::move(id)),
        token(register_cancel_token(request_id)) {}

  ~CancelRegistration() {
    unregister_cancel_token(request_id);
  }
};

bool llama_abort_callback(void* data) {
  auto* token = static_cast<CancelToken*>(data);
  return token != nullptr && token->cancelled.load();
}

void quiet_llama_log(enum ggml_log_level level, const char* text, void*) {
  if (level >= GGML_LOG_LEVEL_ERROR && text) {
    std::fputs(text, stderr);
  }
}

struct mtmd_context;
struct mtmd_bitmap;
struct mtmd_input_chunks;

struct mtmd_input_text {
  const char* text;
  bool add_special;
  bool parse_special;
};

struct mtmd_context_params {
  bool use_gpu;
  bool print_timings;
  int n_threads;
  const char* image_marker;
  const char* media_marker;
  enum llama_flash_attn_type flash_attn_type;
  bool warmup;
  int image_min_tokens;
  int image_max_tokens;
  ggml_backend_sched_eval_callback cb_eval;
  void* cb_eval_user_data;
};

#if defined(_WIN32)
using LibraryHandle = HMODULE;

LibraryHandle open_library(const std::filesystem::path& path, bool required, NativeError& error) {
  auto handle = LoadLibraryW(path.wstring().c_str());
  if (!handle && required) {
    error.set("Failed to load library: " + path.string());
  }
  return handle;
}

void* symbol(LibraryHandle handle, const char* name) {
  return reinterpret_cast<void*>(GetProcAddress(handle, name));
}
#else
using LibraryHandle = void*;

LibraryHandle open_library(const std::filesystem::path& path, bool required, NativeError& error) {
  auto handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle && required) {
    const char* detail = dlerror();
    error.set("Failed to load library: " + path.string() +
              (detail ? std::string(" ") + detail : ""));
  }
  return handle;
}

void* symbol(LibraryHandle handle, const char* name) {
  return dlsym(handle, name);
}
#endif

template <typename T>
void load_symbol(LibraryHandle handle, const char* name, T& target, NativeError& error) {
  target = reinterpret_cast<T>(symbol(handle, name));
  if (!target) {
    error.set(std::string("Missing llama.cpp symbol: ") + name);
  }
}

template <typename T>
void load_optional_symbol(LibraryHandle handle, const char* name, T& target) {
  target = reinterpret_cast<T>(symbol(handle, name));
}

struct LlamaApi {
  bool initialized = false;
  std::filesystem::path library_dir;
  std::string library_path_key;
  std::string mtmd_library_path_key;
  std::vector<LibraryHandle> handles;
  LibraryHandle llama_handle = nullptr;
  LibraryHandle ggml_handle = nullptr;
  LibraryHandle mtmd_handle = nullptr;

  decltype(&ggml_backend_load_all_from_path) ggml_backend_load_all_from_path_ptr = nullptr;
  decltype(&llama_backend_init) llama_backend_init_ptr = nullptr;
  decltype(&llama_log_set) llama_log_set_ptr = nullptr;
  decltype(&llama_model_default_params) llama_model_default_params_ptr = nullptr;
  decltype(&llama_context_default_params) llama_context_default_params_ptr = nullptr;
  decltype(&llama_sampler_chain_default_params) llama_sampler_chain_default_params_ptr = nullptr;
  decltype(&llama_model_load_from_file) llama_model_load_from_file_ptr = nullptr;
  decltype(&llama_model_free) llama_model_free_ptr = nullptr;
  decltype(&llama_model_get_vocab) llama_model_get_vocab_ptr = nullptr;
  decltype(&llama_model_chat_template) llama_model_chat_template_ptr = nullptr;
  decltype(&llama_model_n_embd_out) llama_model_n_embd_out_ptr = nullptr;
  decltype(&llama_init_from_model) llama_init_from_model_ptr = nullptr;
  decltype(&llama_free) llama_free_ptr = nullptr;
  decltype(&llama_tokenize) llama_tokenize_ptr = nullptr;
  decltype(&llama_token_to_piece) llama_token_to_piece_ptr = nullptr;
  decltype(&llama_chat_apply_template) llama_chat_apply_template_ptr = nullptr;
  decltype(&llama_batch_get_one) llama_batch_get_one_ptr = nullptr;
  decltype(&llama_decode) llama_decode_ptr = nullptr;
  decltype(&llama_memory_clear) llama_memory_clear_ptr = nullptr;
  decltype(&llama_get_memory) llama_get_memory_ptr = nullptr;
  decltype(&llama_get_embeddings_seq) llama_get_embeddings_seq_ptr = nullptr;
  decltype(&llama_get_embeddings_ith) llama_get_embeddings_ith_ptr = nullptr;
  decltype(&llama_vocab_is_eog) llama_vocab_is_eog_ptr = nullptr;
  decltype(&llama_sampler_chain_init) llama_sampler_chain_init_ptr = nullptr;
  decltype(&llama_sampler_chain_add) llama_sampler_chain_add_ptr = nullptr;
  decltype(&llama_sampler_free) llama_sampler_free_ptr = nullptr;
  decltype(&llama_sampler_sample) llama_sampler_sample_ptr = nullptr;
  decltype(&llama_sampler_init_grammar) llama_sampler_init_grammar_ptr = nullptr;
  decltype(&llama_sampler_init_greedy) llama_sampler_init_greedy_ptr = nullptr;
  decltype(&llama_sampler_init_dist) llama_sampler_init_dist_ptr = nullptr;
  decltype(&llama_sampler_init_top_k) llama_sampler_init_top_k_ptr = nullptr;
  decltype(&llama_sampler_init_top_p) llama_sampler_init_top_p_ptr = nullptr;
  decltype(&llama_sampler_init_min_p) llama_sampler_init_min_p_ptr = nullptr;
  decltype(&llama_sampler_init_temp) llama_sampler_init_temp_ptr = nullptr;
  decltype(&llama_sampler_init_penalties) llama_sampler_init_penalties_ptr = nullptr;
  decltype(&llama_adapter_lora_init) llama_adapter_lora_init_ptr = nullptr;
  decltype(&llama_adapter_lora_free) llama_adapter_lora_free_ptr = nullptr;
  decltype(&llama_set_adapters_lora) llama_set_adapters_lora_ptr = nullptr;
  decltype(&llama_memory_seq_rm) llama_memory_seq_rm_ptr = nullptr;

  mtmd_context_params (*mtmd_context_params_default_ptr)() = nullptr;
  mtmd_context* (*mtmd_init_from_file_ptr)(const char*, const llama_model*, mtmd_context_params) = nullptr;
  void (*mtmd_free_ptr)(mtmd_context*) = nullptr;
  void (*mtmd_log_set_ptr)(ggml_log_callback, void*) = nullptr;
  void (*mtmd_helper_log_set_ptr)(ggml_log_callback, void*) = nullptr;
  mtmd_bitmap* (*mtmd_helper_bitmap_init_from_buf_ptr)(mtmd_context*, const unsigned char*, size_t) = nullptr;
  void (*mtmd_bitmap_free_ptr)(mtmd_bitmap*) = nullptr;
  void (*mtmd_bitmap_set_id_ptr)(mtmd_bitmap*, const char*) = nullptr;
  mtmd_input_chunks* (*mtmd_input_chunks_init_ptr)() = nullptr;
  void (*mtmd_input_chunks_free_ptr)(mtmd_input_chunks*) = nullptr;
  int32_t (*mtmd_tokenize_ptr)(mtmd_context*, mtmd_input_chunks*, const mtmd_input_text*,
                               const mtmd_bitmap**, size_t) = nullptr;
  llama_pos (*mtmd_helper_get_n_pos_ptr)(const mtmd_input_chunks*) = nullptr;
  int32_t (*mtmd_helper_eval_chunks_ptr)(mtmd_context*, llama_context*, const mtmd_input_chunks*,
                                         llama_pos, llama_seq_id, int32_t, bool, llama_pos*) = nullptr;
};

std::mutex g_api_mutex;
LlamaApi g_api;

std::filesystem::path library_name_for_platform(const std::string& stem) {
#if defined(_WIN32)
  return stem + ".dll";
#elif defined(__APPLE__)
  return "lib" + stem + ".dylib";
#else
  return "lib" + stem + ".so";
#endif
}

std::filesystem::path resolve_llama_library_path(const std::string& library_path,
                                                 const std::string& library_dir) {
  if (!library_path.empty()) return library_path;
  if (!library_dir.empty()) return std::filesystem::path(library_dir) / library_name_for_platform("llama");
  return library_name_for_platform("llama");
}

struct RuntimeConfig;
std::filesystem::path resolve_mtmd_library_path(const RuntimeConfig& config);

std::string path_key(const std::filesystem::path& path) {
  try {
    return std::filesystem::weakly_canonical(path).string();
  } catch (...) {
    return path.lexically_normal().string();
  }
}

void preload_known_libraries(const std::filesystem::path& dir, NativeError& error) {
  if (dir.empty()) return;
  const std::vector<std::string> names{
      "ggml-base", "ggml", "ggml-cpu", "ggml-blas", "ggml-metal", "ggml-cuda", "llama"};
  for (const auto& name : names) {
    const auto path = dir / library_name_for_platform(name);
    if (std::filesystem::exists(path)) {
      auto handle = open_library(path, name == "llama", error);
      if (handle) {
        g_api.handles.push_back(handle);
      }
    }
  }
}

void ensure_api_loaded(const std::string& library_path,
                       const std::string& library_dir,
                       NativeError& error) {
  const auto llama_path = resolve_llama_library_path(library_path, library_dir);
  const auto dir = !library_dir.empty()
                       ? std::filesystem::path(library_dir)
                       : llama_path.has_parent_path() ? llama_path.parent_path() : std::filesystem::path();
  const auto resolved_library_path_key = path_key(llama_path);

  std::lock_guard<std::mutex> lock(g_api_mutex);
  if (g_api.initialized) {
    if (g_api.library_path_key != resolved_library_path_key) {
      error.set("llama.cpp native provider can only load one libllama per process.");
    }
    return;
  }

  g_api.library_dir = dir;
  g_api.library_path_key = resolved_library_path_key;
  preload_known_libraries(dir, error);
  if (error.has_error) return;

  g_api.llama_handle = open_library(llama_path, true, error);
  if (!g_api.llama_handle) return;
  g_api.handles.push_back(g_api.llama_handle);

  const auto ggml_path = dir.empty() ? library_name_for_platform("ggml") : dir / library_name_for_platform("ggml");
  g_api.ggml_handle = open_library(ggml_path, false, error);
  if (g_api.ggml_handle) {
    g_api.handles.push_back(g_api.ggml_handle);
  }

  load_symbol(g_api.ggml_handle ? g_api.ggml_handle : g_api.llama_handle,
              "ggml_backend_load_all_from_path",
              g_api.ggml_backend_load_all_from_path_ptr,
              error);
  load_symbol(g_api.llama_handle, "llama_backend_init", g_api.llama_backend_init_ptr, error);
  load_symbol(g_api.llama_handle, "llama_log_set", g_api.llama_log_set_ptr, error);
  load_symbol(g_api.llama_handle, "llama_model_default_params", g_api.llama_model_default_params_ptr, error);
  load_symbol(g_api.llama_handle, "llama_context_default_params", g_api.llama_context_default_params_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_chain_default_params",
              g_api.llama_sampler_chain_default_params_ptr, error);
  load_symbol(g_api.llama_handle, "llama_model_load_from_file", g_api.llama_model_load_from_file_ptr, error);
  load_symbol(g_api.llama_handle, "llama_model_free", g_api.llama_model_free_ptr, error);
  load_symbol(g_api.llama_handle, "llama_model_get_vocab", g_api.llama_model_get_vocab_ptr, error);
  load_symbol(g_api.llama_handle, "llama_model_chat_template", g_api.llama_model_chat_template_ptr, error);
  load_symbol(g_api.llama_handle, "llama_model_n_embd_out", g_api.llama_model_n_embd_out_ptr, error);
  load_symbol(g_api.llama_handle, "llama_init_from_model", g_api.llama_init_from_model_ptr, error);
  load_symbol(g_api.llama_handle, "llama_free", g_api.llama_free_ptr, error);
  load_symbol(g_api.llama_handle, "llama_tokenize", g_api.llama_tokenize_ptr, error);
  load_symbol(g_api.llama_handle, "llama_token_to_piece", g_api.llama_token_to_piece_ptr, error);
  load_symbol(g_api.llama_handle, "llama_chat_apply_template", g_api.llama_chat_apply_template_ptr, error);
  load_symbol(g_api.llama_handle, "llama_batch_get_one", g_api.llama_batch_get_one_ptr, error);
  load_symbol(g_api.llama_handle, "llama_decode", g_api.llama_decode_ptr, error);
  load_symbol(g_api.llama_handle, "llama_memory_clear", g_api.llama_memory_clear_ptr, error);
  load_symbol(g_api.llama_handle, "llama_get_memory", g_api.llama_get_memory_ptr, error);
  load_symbol(g_api.llama_handle, "llama_get_embeddings_seq", g_api.llama_get_embeddings_seq_ptr, error);
  load_symbol(g_api.llama_handle, "llama_get_embeddings_ith", g_api.llama_get_embeddings_ith_ptr, error);
  load_symbol(g_api.llama_handle, "llama_vocab_is_eog", g_api.llama_vocab_is_eog_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_chain_init", g_api.llama_sampler_chain_init_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_chain_add", g_api.llama_sampler_chain_add_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_free", g_api.llama_sampler_free_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_sample", g_api.llama_sampler_sample_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_greedy", g_api.llama_sampler_init_greedy_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_dist", g_api.llama_sampler_init_dist_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_top_k", g_api.llama_sampler_init_top_k_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_top_p", g_api.llama_sampler_init_top_p_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_min_p", g_api.llama_sampler_init_min_p_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_temp", g_api.llama_sampler_init_temp_ptr, error);
  load_symbol(g_api.llama_handle, "llama_sampler_init_penalties", g_api.llama_sampler_init_penalties_ptr, error);
  load_optional_symbol(g_api.llama_handle, "llama_sampler_init_grammar", g_api.llama_sampler_init_grammar_ptr);
  load_optional_symbol(g_api.llama_handle, "llama_adapter_lora_init", g_api.llama_adapter_lora_init_ptr);
  load_optional_symbol(g_api.llama_handle, "llama_adapter_lora_free", g_api.llama_adapter_lora_free_ptr);
  load_optional_symbol(g_api.llama_handle, "llama_set_adapters_lora", g_api.llama_set_adapters_lora_ptr);
  load_optional_symbol(g_api.llama_handle, "llama_memory_seq_rm", g_api.llama_memory_seq_rm_ptr);
  if (error.has_error) return;

  g_api.ggml_backend_load_all_from_path_ptr(dir.empty() ? nullptr : dir.string().c_str());
  g_api.llama_log_set_ptr(quiet_llama_log, nullptr);
  g_api.llama_backend_init_ptr();
  g_api.initialized = true;
}

struct RuntimeConfig {
  std::string model_path;
  std::string library_path;
  std::string library_dir;
  uint32_t context_size = 2048;
  uint32_t batch_size = 512;
  int32_t threads = 0;
  int32_t batch_threads = 0;
  int32_t gpu_layers = 0;
  bool mmap = true;
  bool mlock = false;
  std::string mmproj_path;
  std::string mtmd_library_path;
  std::string mtmd_library_dir;
  bool mmproj_use_gpu = true;
  int32_t mmproj_threads = 0;
  int32_t mmproj_image_min_tokens = 0;
  int32_t mmproj_image_max_tokens = 0;
  std::string media_marker = "<__media__>";
  std::vector<LlamaCppNativeLoraAdapter> lora_adapters;
};

RuntimeConfig runtime_config_from_agent_config(const LlamaCppNativeRuntimeConfig& input) {
  RuntimeConfig config;
  config.model_path = input.model_path;
  config.library_path = input.library_path;
  config.library_dir = input.library_dir;
  config.context_size = static_cast<uint32_t>(std::max(1, input.context_size.value_or(2048)));
  config.batch_size = static_cast<uint32_t>(std::max(1, input.batch_size.value_or(512)));
  config.threads = input.threads.value_or(0);
  config.batch_threads = input.batch_threads.value_or(0);
  config.gpu_layers = input.gpu_layers.value_or(0);
  config.mmap = input.mmap.value_or(true);
  config.mlock = input.mlock.value_or(false);
  config.mmproj_path = input.mmproj_path;
  config.mtmd_library_path = input.mtmd_library_path;
  config.mtmd_library_dir = input.mtmd_library_dir;
  config.mmproj_use_gpu = input.mmproj_use_gpu.value_or(true);
  config.mmproj_threads = input.mmproj_threads.value_or(0);
  config.mmproj_image_min_tokens = input.mmproj_image_min_tokens.value_or(0);
  config.mmproj_image_max_tokens = input.mmproj_image_max_tokens.value_or(0);
  config.media_marker = input.media_marker.empty() ? "<__media__>" : input.media_marker;
  config.lora_adapters = input.lora_adapters;
  return config;
}

std::filesystem::path resolve_mtmd_library_path(const RuntimeConfig& config) {
  if (!config.mtmd_library_path.empty()) return config.mtmd_library_path;
  if (!config.mtmd_library_dir.empty()) {
    return std::filesystem::path(config.mtmd_library_dir) / library_name_for_platform("mtmd");
  }
  if (!config.library_dir.empty()) {
    return std::filesystem::path(config.library_dir) / library_name_for_platform("mtmd");
  }
  if (!g_api.library_dir.empty()) {
    return g_api.library_dir / library_name_for_platform("mtmd");
  }
  return library_name_for_platform("mtmd");
}

struct ModelHandle {
  llama_model* model = nullptr;
  const llama_vocab* vocab = nullptr;
  std::vector<llama_adapter_lora*> lora_adapters;
  std::vector<float> lora_scales;

  ~ModelHandle() {
    if (g_api.llama_adapter_lora_free_ptr) {
      for (auto* adapter : lora_adapters) {
        if (adapter) g_api.llama_adapter_lora_free_ptr(adapter);
      }
    }
    if (model && g_api.llama_model_free_ptr) {
      g_api.llama_model_free_ptr(model);
    }
  }
};

std::mutex g_model_mutex;
std::unordered_map<std::string, std::weak_ptr<ModelHandle>> g_model_cache;

std::string model_cache_key(const RuntimeConfig& config) {
  std::string key = config.library_path + "|" + config.library_dir + "|" + config.model_path + "|" +
                    std::to_string(config.gpu_layers) + "|" + (config.mmap ? "mmap" : "nommap") +
                    "|" + (config.mlock ? "mlock" : "nomlock");
  for (const auto& adapter : config.lora_adapters) {
    key += "|lora:" + adapter.path + "@" + std::to_string(adapter.scale);
  }
  return key;
}

std::shared_ptr<ModelHandle> load_model(const RuntimeConfig& config, NativeError& error) {
  ensure_api_loaded(config.library_path, config.library_dir, error);
  if (error.has_error) return nullptr;

  const auto key = model_cache_key(config);
  {
    std::lock_guard<std::mutex> lock(g_model_mutex);
    const auto found = g_model_cache.find(key);
    if (found != g_model_cache.end()) {
      if (auto existing = found->second.lock()) return existing;
    }
  }

  auto model_params = g_api.llama_model_default_params_ptr();
  model_params.n_gpu_layers = config.gpu_layers;
  model_params.use_mmap = config.mmap;
  model_params.use_mlock = config.mlock;

  llama_model* model = g_api.llama_model_load_from_file_ptr(config.model_path.c_str(), model_params);
  if (!model) {
    error.set("Failed to load llama.cpp model: " + config.model_path);
    return nullptr;
  }

  auto handle = std::make_shared<ModelHandle>();
  handle->model = model;
  handle->vocab = g_api.llama_model_get_vocab_ptr(model);

  if (!config.lora_adapters.empty()) {
    if (!g_api.llama_adapter_lora_init_ptr || !g_api.llama_set_adapters_lora_ptr) {
      error.set("The loaded llama.cpp library does not support LoRA adapters.");
      return nullptr;
    }
    for (const auto& adapter_config : config.lora_adapters) {
      llama_adapter_lora* adapter = g_api.llama_adapter_lora_init_ptr(model, adapter_config.path.c_str());
      if (!adapter) {
        error.set("Failed to load llama.cpp LoRA adapter: " + adapter_config.path);
        return nullptr;
      }
      handle->lora_adapters.push_back(adapter);
      handle->lora_scales.push_back(static_cast<float>(adapter_config.scale));
    }
  }

  std::lock_guard<std::mutex> lock(g_model_mutex);
  g_model_cache[key] = handle;
  return handle;
}

void ensure_mtmd_loaded(const RuntimeConfig& config, NativeError& error) {
  ensure_api_loaded(config.library_path, config.library_dir, error);
  if (error.has_error) return;

  const auto mtmd_path = resolve_mtmd_library_path(config);
  const auto resolved_key = path_key(mtmd_path);

  std::lock_guard<std::mutex> lock(g_api_mutex);
  if (g_api.mtmd_handle) {
    if (g_api.mtmd_library_path_key != resolved_key) {
      error.set("llama.cpp native provider can only load one libmtmd per process.");
    }
    return;
  }

  g_api.mtmd_library_path_key = resolved_key;
  g_api.mtmd_handle = open_library(mtmd_path, true, error);
  if (!g_api.mtmd_handle) return;
  g_api.handles.push_back(g_api.mtmd_handle);

  load_symbol(g_api.mtmd_handle, "mtmd_context_params_default", g_api.mtmd_context_params_default_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_init_from_file", g_api.mtmd_init_from_file_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_free", g_api.mtmd_free_ptr, error);
  load_optional_symbol(g_api.mtmd_handle, "mtmd_log_set", g_api.mtmd_log_set_ptr);
  load_optional_symbol(g_api.mtmd_handle, "mtmd_helper_log_set", g_api.mtmd_helper_log_set_ptr);
  load_symbol(g_api.mtmd_handle, "mtmd_helper_bitmap_init_from_buf",
              g_api.mtmd_helper_bitmap_init_from_buf_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_bitmap_free", g_api.mtmd_bitmap_free_ptr, error);
  load_optional_symbol(g_api.mtmd_handle, "mtmd_bitmap_set_id", g_api.mtmd_bitmap_set_id_ptr);
  load_symbol(g_api.mtmd_handle, "mtmd_input_chunks_init", g_api.mtmd_input_chunks_init_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_input_chunks_free", g_api.mtmd_input_chunks_free_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_tokenize", g_api.mtmd_tokenize_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_helper_get_n_pos", g_api.mtmd_helper_get_n_pos_ptr, error);
  load_symbol(g_api.mtmd_handle, "mtmd_helper_eval_chunks", g_api.mtmd_helper_eval_chunks_ptr, error);
  if (error.has_error) return;

  if (g_api.mtmd_helper_log_set_ptr) {
    g_api.mtmd_helper_log_set_ptr(quiet_llama_log, nullptr);
  } else if (g_api.mtmd_log_set_ptr) {
    g_api.mtmd_log_set_ptr(quiet_llama_log, nullptr);
  }
}

struct MultimodalHandle {
  mtmd_context* ctx = nullptr;
  std::string media_marker;
  std::mutex mutex;

  ~MultimodalHandle() {
    if (ctx && g_api.mtmd_free_ptr) {
      g_api.mtmd_free_ptr(ctx);
    }
  }
};

std::mutex g_multimodal_mutex;
std::unordered_map<std::string, std::weak_ptr<MultimodalHandle>> g_multimodal_cache;

std::string multimodal_cache_key(const RuntimeConfig& config) {
  return model_cache_key(config) + "|mmproj:" + config.mmproj_path + "|" + config.mtmd_library_path +
         "|" + config.mtmd_library_dir + "|" + (config.mmproj_use_gpu ? "gpu" : "cpu") + "|" +
         std::to_string(config.mmproj_threads) + "|" +
         std::to_string(config.mmproj_image_min_tokens) + "|" +
         std::to_string(config.mmproj_image_max_tokens) + "|" + config.media_marker;
}

std::shared_ptr<MultimodalHandle> load_multimodal_context(const RuntimeConfig& config,
                                                          const std::shared_ptr<ModelHandle>& model,
                                                          NativeError& error) {
  if (config.mmproj_path.empty()) {
    error.set("llama.cpp native image inputs require mmprojPath.");
    return nullptr;
  }
  ensure_mtmd_loaded(config, error);
  if (error.has_error) return nullptr;

  const auto key = multimodal_cache_key(config);
  {
    std::lock_guard<std::mutex> lock(g_multimodal_mutex);
    const auto found = g_multimodal_cache.find(key);
    if (found != g_multimodal_cache.end()) {
      if (auto existing = found->second.lock()) return existing;
    }
  }

  auto handle = std::make_shared<MultimodalHandle>();
  handle->media_marker = config.media_marker;
  auto params = g_api.mtmd_context_params_default_ptr();
  params.use_gpu = config.mmproj_use_gpu;
  params.n_threads = config.mmproj_threads;
  params.print_timings = false;
  params.warmup = false;
  if (!handle->media_marker.empty()) {
    params.media_marker = handle->media_marker.c_str();
  }
  if (config.mmproj_image_min_tokens > 0) {
    params.image_min_tokens = config.mmproj_image_min_tokens;
  }
  if (config.mmproj_image_max_tokens > 0) {
    params.image_max_tokens = config.mmproj_image_max_tokens;
  }

  mtmd_context* ctx = g_api.mtmd_init_from_file_ptr(config.mmproj_path.c_str(), model->model, params);
  if (!ctx) {
    error.set("Failed to load llama.cpp mmproj: " + config.mmproj_path);
    return nullptr;
  }

  handle->ctx = ctx;
  std::lock_guard<std::mutex> lock(g_multimodal_mutex);
  g_multimodal_cache[key] = handle;
  return handle;
}

std::vector<llama_token> tokenize(const std::shared_ptr<ModelHandle>& model,
                                  const std::string& text,
                                  bool add_special,
                                  NativeError& error) {
  int32_t n_tokens = g_api.llama_tokenize_ptr(model->vocab, text.c_str(),
                                              static_cast<int32_t>(text.size()),
                                              nullptr, 0, add_special, true);
  if (n_tokens < 0) n_tokens = -n_tokens;
  if (n_tokens <= 0) {
    error.set("llama.cpp tokenization returned no tokens.");
    return {};
  }
  std::vector<llama_token> tokens(static_cast<std::size_t>(n_tokens));
  const int32_t actual = g_api.llama_tokenize_ptr(model->vocab, text.c_str(),
                                                  static_cast<int32_t>(text.size()),
                                                  tokens.data(), n_tokens, add_special, true);
  if (actual < 0) {
    error.set("Failed to tokenize llama.cpp prompt.");
    return {};
  }
  tokens.resize(static_cast<std::size_t>(actual));
  return tokens;
}

std::string token_to_piece(const std::shared_ptr<ModelHandle>& model,
                           llama_token token,
                           NativeError& error) {
  char small[256];
  int32_t n = g_api.llama_token_to_piece_ptr(model->vocab, token, small, sizeof(small), 0, true);
  if (n >= 0) {
    return std::string(small, static_cast<std::size_t>(n));
  }
  std::vector<char> buffer(static_cast<std::size_t>(-n));
  n = g_api.llama_token_to_piece_ptr(model->vocab, token, buffer.data(),
                                     static_cast<int32_t>(buffer.size()), 0, true);
  if (n < 0) {
    error.set("Failed to convert llama.cpp token to text.");
    return {};
  }
  return std::string(buffer.data(), static_cast<std::size_t>(n));
}

std::string fallback_prompt(const std::vector<LlamaCppNativeChatMessage>& messages) {
  std::string prompt;
  for (const auto& message : messages) {
    prompt += message.role;
    prompt += ": ";
    prompt += message.content;
    prompt += "\n";
  }
  prompt += "assistant: ";
  return prompt;
}

std::string apply_chat_template(const std::shared_ptr<ModelHandle>& model,
                                const std::vector<LlamaCppNativeChatMessage>& messages,
                                const std::string& chat_template,
                                bool strict_template,
                                NativeError& error) {
  const char* tmpl = chat_template.empty()
                         ? g_api.llama_model_chat_template_ptr(model->model, nullptr)
                         : chat_template.c_str();
  if (!tmpl) {
    if (strict_template) {
      error.set("llama.cpp model does not provide a chat template.");
      return {};
    }
    return fallback_prompt(messages);
  }

  std::vector<llama_chat_message> llama_messages;
  llama_messages.reserve(messages.size());
  for (const auto& message : messages) {
    llama_messages.push_back({message.role.c_str(), message.content.c_str()});
  }

  int32_t size = g_api.llama_chat_apply_template_ptr(tmpl, llama_messages.data(),
                                                     llama_messages.size(), true, nullptr, 0);
  if (size < 0) {
    if (strict_template) {
      error.set("Failed to apply llama.cpp chat template.");
      return {};
    }
    return fallback_prompt(messages);
  }
  std::vector<char> buffer(static_cast<std::size_t>(size));
  int32_t actual = g_api.llama_chat_apply_template_ptr(tmpl, llama_messages.data(),
                                                       llama_messages.size(), true,
                                                       buffer.data(), size);
  if (actual < 0) {
    error.set("Failed to apply llama.cpp chat template.");
    return {};
  }
  return std::string(buffer.data(), static_cast<std::size_t>(actual));
}

void apply_lora_adapters(llama_context* ctx,
                         const std::shared_ptr<ModelHandle>& model,
                         NativeError& error) {
  if (model->lora_adapters.empty()) return;
  if (!g_api.llama_set_adapters_lora_ptr) {
    error.set("The loaded llama.cpp library does not support context LoRA adapters.");
    return;
  }
  if (g_api.llama_set_adapters_lora_ptr(ctx,
                                        model->lora_adapters.data(),
                                        model->lora_adapters.size(),
                                        model->lora_scales.data()) != 0) {
    error.set("Failed to apply llama.cpp LoRA adapters to context.");
  }
}

struct SessionAbortState {
  std::atomic<CancelToken*> current{nullptr};
};

bool session_abort_callback(void* data) {
  auto* state = static_cast<SessionAbortState*>(data);
  if (!state) return false;
  auto* token = state->current.load();
  return token != nullptr && token->cancelled.load();
}

struct ContextHandle {
  std::shared_ptr<ModelHandle> model;
  llama_context* ctx = nullptr;
  std::vector<llama_token> tokens;
  std::mutex mutex;
  SessionAbortState abort_state;

  ~ContextHandle() {
    if (ctx && g_api.llama_free_ptr) {
      g_api.llama_free_ptr(ctx);
    }
  }
};

struct SessionAbortScope {
  std::shared_ptr<ContextHandle> context;
  ~SessionAbortScope() {
    if (context) {
      context->abort_state.current.store(nullptr);
    }
  }
};

llama_context* create_context(const RuntimeConfig& config,
                              const std::shared_ptr<ModelHandle>& model,
                              NativeError& error,
                              ggml_abort_callback abort_callback,
                              void* abort_data,
                              bool embeddings = false,
                              enum llama_pooling_type pooling_type = LLAMA_POOLING_TYPE_UNSPECIFIED) {
  auto ctx_params = g_api.llama_context_default_params_ptr();
  ctx_params.n_ctx = config.context_size;
  ctx_params.n_batch = config.batch_size;
  ctx_params.n_threads = config.threads;
  ctx_params.n_threads_batch = config.batch_threads;
  ctx_params.abort_callback = abort_callback;
  ctx_params.abort_callback_data = abort_data;
  ctx_params.embeddings = embeddings;
  ctx_params.pooling_type = pooling_type;

  llama_context* ctx = g_api.llama_init_from_model_ptr(model->model, ctx_params);
  if (!ctx) {
    error.set(embeddings ? "Failed to create llama.cpp embedding context."
                         : "Failed to create llama.cpp context.");
    return nullptr;
  }
  apply_lora_adapters(ctx, model, error);
  if (error.has_error) {
    g_api.llama_free_ptr(ctx);
    return nullptr;
  }
  return ctx;
}

std::mutex g_session_mutex;
std::unordered_map<std::string, std::shared_ptr<ContextHandle>> g_session_contexts;

std::string session_cache_key(const RuntimeConfig& config, const std::string& session_id) {
  return model_cache_key(config) + "|ctx:" + std::to_string(config.context_size) + "|" +
         std::to_string(config.batch_size) + "|" + std::to_string(config.threads) + "|" +
         std::to_string(config.batch_threads) + "|session:" + session_id;
}

std::shared_ptr<ContextHandle> get_session_context(const RuntimeConfig& config,
                                                   const std::shared_ptr<ModelHandle>& model,
                                                   const std::string& session_id,
                                                   NativeError& error) {
  const auto key = session_cache_key(config, session_id);
  {
    std::lock_guard<std::mutex> lock(g_session_mutex);
    const auto found = g_session_contexts.find(key);
    if (found != g_session_contexts.end()) return found->second;
  }

  auto handle = std::make_shared<ContextHandle>();
  handle->model = model;
  handle->ctx = create_context(config, model, error, session_abort_callback, &handle->abort_state);
  if (error.has_error) return nullptr;

  std::lock_guard<std::mutex> lock(g_session_mutex);
  auto [it, inserted] = g_session_contexts.emplace(key, handle);
  return inserted ? handle : it->second;
}

std::size_t common_prefix_length(const std::vector<llama_token>& left,
                                 const std::vector<llama_token>& right) {
  std::size_t index = 0;
  const std::size_t max = std::min(left.size(), right.size());
  while (index < max && left[index] == right[index]) {
    ++index;
  }
  return index;
}

bool remove_session_suffix(ContextHandle& handle, std::size_t start) {
  if (start >= handle.tokens.size()) return true;
  if (g_api.llama_memory_seq_rm_ptr) {
    if (!g_api.llama_memory_seq_rm_ptr(g_api.llama_get_memory_ptr(handle.ctx),
                                       0, static_cast<llama_pos>(start), -1)) {
      return false;
    }
  } else {
    g_api.llama_memory_clear_ptr(g_api.llama_get_memory_ptr(handle.ctx), true);
    start = 0;
  }
  handle.tokens.resize(start);
  return true;
}

bool decode_token_range(llama_context* ctx,
                        const std::vector<llama_token>& tokens,
                        std::size_t start,
                        NativeError& error) {
  if (start >= tokens.size()) return true;
  std::vector<llama_token> pending(tokens.begin() + static_cast<std::ptrdiff_t>(start), tokens.end());
  llama_batch batch = g_api.llama_batch_get_one_ptr(pending.data(), static_cast<int32_t>(pending.size()));
  if (g_api.llama_decode_ptr(ctx, batch) != 0) {
    error.set("llama.cpp failed to decode prompt.");
    return false;
  }
  return true;
}

bool eval_multimodal_prompt(const RuntimeConfig& config,
                            const std::shared_ptr<ModelHandle>& model,
                            llama_context* ctx,
                            const LlamaCppNativeChatRequest& request,
                            const std::string& prompt,
                            const std::shared_ptr<CancelToken>& cancel_token,
                            NativeError& error) {
  auto multimodal = load_multimodal_context(config, model, error);
  if (error.has_error) return false;

  std::unique_lock<std::mutex> lock(multimodal->mutex);
  std::vector<mtmd_bitmap*> bitmaps;
  mtmd_input_chunks* chunks = nullptr;
  auto cleanup = [&]() {
    if (chunks && g_api.mtmd_input_chunks_free_ptr) {
      g_api.mtmd_input_chunks_free_ptr(chunks);
    }
    if (g_api.mtmd_bitmap_free_ptr) {
      for (auto* bitmap : bitmaps) {
        if (bitmap) g_api.mtmd_bitmap_free_ptr(bitmap);
      }
    }
  };

  bitmaps.reserve(request.media.size());
  for (const auto& media : request.media) {
    if (cancel_token->cancelled.load()) {
      error.set("Operation aborted.");
      cleanup();
      return false;
    }
    mtmd_bitmap* bitmap = g_api.mtmd_helper_bitmap_init_from_buf_ptr(
        multimodal->ctx, media.bytes.data(), media.bytes.size());
    if (!bitmap) {
      error.set("Failed to decode multimodal image input" +
                (media.filename.empty() ? std::string(".") : ": " + media.filename));
      cleanup();
      return false;
    }
    if (!media.id.empty() && g_api.mtmd_bitmap_set_id_ptr) {
      g_api.mtmd_bitmap_set_id_ptr(bitmap, media.id.c_str());
    }
    bitmaps.push_back(bitmap);
  }

  chunks = g_api.mtmd_input_chunks_init_ptr();
  if (!chunks) {
    error.set("Failed to allocate llama.cpp multimodal input chunks.");
    cleanup();
    return false;
  }

  std::vector<const mtmd_bitmap*> bitmap_ptrs;
  bitmap_ptrs.reserve(bitmaps.size());
  for (auto* bitmap : bitmaps) {
    bitmap_ptrs.push_back(bitmap);
  }

  mtmd_input_text text{prompt.c_str(), true, true};
  const int32_t tokenized = g_api.mtmd_tokenize_ptr(multimodal->ctx, chunks, &text,
                                                    bitmap_ptrs.data(), bitmap_ptrs.size());
  if (tokenized != 0) {
    error.set("llama.cpp failed to tokenize multimodal prompt.");
    cleanup();
    return false;
  }

  const llama_pos n_pos = g_api.mtmd_helper_get_n_pos_ptr(chunks);
  if (n_pos > static_cast<llama_pos>(config.context_size)) {
    error.set("llama.cpp multimodal prompt exceeds context size.");
    cleanup();
    return false;
  }

  llama_pos n_past = 0;
  const int32_t decoded = g_api.mtmd_helper_eval_chunks_ptr(
      multimodal->ctx, ctx, chunks, 0, 0, static_cast<int32_t>(config.batch_size), true, &n_past);
  if (decoded != 0) {
    error.set("llama.cpp failed to evaluate multimodal prompt.");
    cleanup();
    return false;
  }

  cleanup();
  return true;
}

llama_sampler* create_sampler(const std::shared_ptr<ModelHandle>& model,
                              const LlamaCppNativeChatRequest& request,
                              NativeError& error) {
  auto sampler_params = g_api.llama_sampler_chain_default_params_ptr();
  llama_sampler* sampler = g_api.llama_sampler_chain_init_ptr(sampler_params);
  if (!sampler) {
    error.set("Failed to initialize llama.cpp sampler.");
    return nullptr;
  }

  if (!request.grammar.empty()) {
    if (!g_api.llama_sampler_init_grammar_ptr) {
      error.set("The loaded llama.cpp library does not support grammar sampling.");
      return sampler;
    }
    llama_sampler* grammar_sampler = g_api.llama_sampler_init_grammar_ptr(
        model->vocab,
        request.grammar.c_str(),
        request.grammar_root.empty() ? "root" : request.grammar_root.c_str());
    if (!grammar_sampler) {
      error.set("Failed to initialize llama.cpp grammar sampler.");
      return sampler;
    }
    g_api.llama_sampler_chain_add_ptr(sampler, grammar_sampler);
  }

  const float repeat_penalty = static_cast<float>(request.repeat_penalty.value_or(1.0));
  if (repeat_penalty != 1.0f) {
    g_api.llama_sampler_chain_add_ptr(sampler,
                                      g_api.llama_sampler_init_penalties_ptr(-1, repeat_penalty, 0.0f, 0.0f));
  }

  const float temperature = static_cast<float>(request.temperature.value_or(0.2));
  if (temperature <= 0.0f) {
    g_api.llama_sampler_chain_add_ptr(sampler, g_api.llama_sampler_init_greedy_ptr());
  } else {
    g_api.llama_sampler_chain_add_ptr(sampler, g_api.llama_sampler_init_top_k_ptr(request.top_k.value_or(40)));
    g_api.llama_sampler_chain_add_ptr(
        sampler, g_api.llama_sampler_init_top_p_ptr(static_cast<float>(request.top_p.value_or(0.95)), 1));
    if (request.min_p.value_or(0.0) > 0.0) {
      g_api.llama_sampler_chain_add_ptr(
          sampler, g_api.llama_sampler_init_min_p_ptr(static_cast<float>(request.min_p.value_or(0.0)), 1));
    }
    g_api.llama_sampler_chain_add_ptr(sampler, g_api.llama_sampler_init_temp_ptr(temperature));
    g_api.llama_sampler_chain_add_ptr(
        sampler, g_api.llama_sampler_init_dist_ptr(static_cast<uint32_t>(request.seed.value_or(LLAMA_DEFAULT_SEED))));
  }
  return sampler;
}

LlamaCppNativeChatResult generate_chat_impl(const LlamaCppNativeRuntimeConfig& public_config,
                                            const LlamaCppNativeChatRequest& request,
                                            LlamaCppNativeBinding::ChatDeltaHandler on_delta) {
  CancelRegistration cancel_registration(request.request_id);
  auto cancel_token = cancel_registration.token;
  NativeError error;
  const RuntimeConfig config = runtime_config_from_agent_config(public_config);
  auto model = load_model(config, error);
  error.throw_if_failed();

  const std::string prompt = apply_chat_template(model, request.messages, request.chat_template,
                                                 request.strict_template, error);
  error.throw_if_failed();

  const bool has_media = !request.media.empty();
  if (has_media && !request.session_id.empty()) {
    throw AdapterError("llama.cpp native provider does not support sessionId with multimodal image inputs.");
  }

  std::vector<llama_token> tokens;
  if (!has_media) {
    tokens = tokenize(model, prompt, true, error);
    error.throw_if_failed();
  }

  std::shared_ptr<ContextHandle> session_context;
  std::unique_ptr<ContextHandle> local_context;
  std::unique_lock<std::mutex> session_lock;
  SessionAbortScope abort_scope;
  ContextHandle* active_context = nullptr;

  if (!has_media && !request.session_id.empty()) {
    session_context = get_session_context(config, model, request.session_id, error);
    error.throw_if_failed();
    session_lock = std::unique_lock<std::mutex>(session_context->mutex);
    session_context->abort_state.current.store(cancel_token.get());
    abort_scope.context = session_context;
    active_context = session_context.get();
  } else {
    local_context = std::make_unique<ContextHandle>();
    local_context->model = model;
    local_context->ctx = create_context(config, model, error, llama_abort_callback, cancel_token.get());
    error.throw_if_failed();
    active_context = local_context.get();
  }

  llama_sampler* sampler = create_sampler(model, request, error);
  if (error.has_error) {
    if (sampler) g_api.llama_sampler_free_ptr(sampler);
    error.throw_if_failed();
  }

  if (has_media) {
    eval_multimodal_prompt(config, model, active_context->ctx, request, prompt, cancel_token, error);
    if (error.has_error) {
      g_api.llama_sampler_free_ptr(sampler);
      error.throw_if_failed();
    }
  } else {
    std::size_t decode_start = 0;
    if (session_context) {
      std::size_t common = common_prefix_length(active_context->tokens, tokens);
      decode_start = common;
      if (!tokens.empty() && decode_start >= tokens.size()) {
        decode_start = tokens.size() - 1;
      }
      if (!remove_session_suffix(*active_context, decode_start)) {
        g_api.llama_memory_clear_ptr(g_api.llama_get_memory_ptr(active_context->ctx), true);
        active_context->tokens.clear();
        decode_start = 0;
      }
    }
    decode_token_range(active_context->ctx, tokens, decode_start, error);
    if (error.has_error) {
      g_api.llama_sampler_free_ptr(sampler);
      error.throw_if_failed();
    }
    if (session_context) {
      active_context->tokens = tokens;
    }
  }

  const int32_t max_tokens = std::max(1, request.max_output_tokens.value_or(1024));
  int32_t produced = 0;
  std::vector<llama_token> generated_tokens;
  std::string text;
  std::string finish_reason = "stop";

  while (!error.has_error && produced < max_tokens) {
    if (cancel_token->cancelled.load()) {
      error.set("Operation aborted.");
      break;
    }

    llama_token token = g_api.llama_sampler_sample_ptr(sampler, active_context->ctx, -1);
    if (g_api.llama_vocab_is_eog_ptr(model->vocab, token)) {
      finish_reason = "stop";
      break;
    }

    std::string piece = token_to_piece(model, token, error);
    if (error.has_error) break;
    text += piece;
    if (on_delta && !piece.empty()) {
      on_delta(LlamaCppNativeChatDelta{.type = "text", .delta = piece});
    }
    ++produced;
    generated_tokens.push_back(token);

    llama_batch batch = g_api.llama_batch_get_one_ptr(&token, 1);
    if (g_api.llama_decode_ptr(active_context->ctx, batch) != 0) {
      error.set("llama.cpp failed while generating text.");
      break;
    }
  }

  if (!error.has_error && produced >= max_tokens) {
    finish_reason = "length";
  }
  g_api.llama_sampler_free_ptr(sampler);
  if (session_context) {
    active_context->tokens.insert(active_context->tokens.end(),
                                  generated_tokens.begin(),
                                  generated_tokens.end());
  }
  error.throw_if_failed();

  return LlamaCppNativeChatResult{
      .id = request.request_id,
      .model = request.model,
      .text = text,
      .finish_reason = finish_reason,
      .raw = Value::object({
          {"id", request.request_id},
          {"model", request.model},
          {"text", text},
          {"finishReason", finish_reason},
      }),
  };
}

void normalize_float_vector(std::vector<float>& values) {
  double sum = 0.0;
  for (float value : values) {
    sum += static_cast<double>(value) * static_cast<double>(value);
  }
  if (sum <= 0.0) return;
  const float scale = static_cast<float>(1.0 / std::sqrt(sum));
  for (float& value : values) {
    value *= scale;
  }
}

enum llama_pooling_type pooling_type_from_string(const std::string& value) {
  if (value == "cls") return LLAMA_POOLING_TYPE_CLS;
  if (value == "last") return LLAMA_POOLING_TYPE_LAST;
  return LLAMA_POOLING_TYPE_MEAN;
}

LlamaCppNativeEmbeddingResult embed_texts_impl(const LlamaCppNativeRuntimeConfig& public_config,
                                               const LlamaCppNativeEmbeddingRequest& request) {
  CancelRegistration cancel_registration(request.request_id);
  auto cancel_token = cancel_registration.token;
  NativeError error;
  const RuntimeConfig config = runtime_config_from_agent_config(public_config);
  auto model = load_model(config, error);
  error.throw_if_failed();

  const auto pooling_type = pooling_type_from_string(request.pooling);
  llama_context* ctx = create_context(config, model, error, llama_abort_callback,
                                      cancel_token.get(), true, pooling_type);
  error.throw_if_failed();

  const int32_t dimensions = g_api.llama_model_n_embd_out_ptr(model->model);
  std::vector<EmbeddingVector> embeddings;
  embeddings.reserve(request.texts.size());

  for (const auto& text : request.texts) {
    if (cancel_token->cancelled.load()) {
      error.set("Operation aborted.");
      break;
    }
    auto tokens = tokenize(model, text, true, error);
    if (error.has_error) break;
    if (tokens.size() > config.batch_size || tokens.size() > config.context_size) {
      error.set("llama.cpp embedding input exceeds context or batch size.");
      break;
    }

    g_api.llama_memory_clear_ptr(g_api.llama_get_memory_ptr(ctx), true);
    llama_batch batch = g_api.llama_batch_get_one_ptr(tokens.data(), static_cast<int32_t>(tokens.size()));
    if (g_api.llama_decode_ptr(ctx, batch) < 0) {
      error.set("llama.cpp failed to compute embeddings.");
      break;
    }

    const float* raw = g_api.llama_get_embeddings_seq_ptr(ctx, 0);
    if (!raw) {
      raw = g_api.llama_get_embeddings_ith_ptr(ctx, -1);
    }
    if (!raw) {
      error.set("llama.cpp did not return embeddings.");
      break;
    }

    std::vector<float> values(raw, raw + dimensions);
    if (request.normalize) {
      normalize_float_vector(values);
    }
    EmbeddingVector embedding;
    embedding.reserve(values.size());
    for (float value : values) {
      embedding.push_back(value);
    }
    embeddings.push_back(std::move(embedding));
  }

  g_api.llama_free_ptr(ctx);
  error.throw_if_failed();

  return LlamaCppNativeEmbeddingResult{
      .model = request.model,
      .embeddings = std::move(embeddings),
      .dimensions = dimensions,
  };
}

}  // namespace

std::shared_ptr<LlamaCppNativeBinding> create_llama_cpp_native_binding() {
  auto binding = std::make_shared<LlamaCppNativeBinding>();
  binding->generate_chat = [](const LlamaCppNativeRuntimeConfig& config,
                              const LlamaCppNativeChatRequest& request,
                              LlamaCppNativeBinding::ChatDeltaHandler on_delta) {
    return generate_chat_impl(config, request, std::move(on_delta));
  };
  binding->embed_texts = [](const LlamaCppNativeRuntimeConfig& config,
                            const LlamaCppNativeEmbeddingRequest& request) {
    return embed_texts_impl(config, request);
  };
  binding->cancel = [](const std::string& request_id) {
    cancel_request(request_id);
  };
  return binding;
}

}  // namespace agent
