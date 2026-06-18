if(NOT DEFINED AGENT_SOURCE_DIR)
  message(FATAL_ERROR "AGENT_SOURCE_DIR is required.")
endif()

file(READ "${AGENT_SOURCE_DIR}/CMakeLists.txt" cmake_lists)
string(REGEX MATCH "target_link_libraries\\(agent_native[ \t\r\n]+INTERFACE[^)]*\\)" agent_native_link_block "${cmake_lists}")
string(REGEX REPLACE "[() \t\r\n]+" ";" agent_native_link_tokens "${agent_native_link_block}")
list(FIND agent_native_link_tokens "agent_runtime" agent_native_runtime_index)
if(agent_native_runtime_index EQUAL -1)
  message(FATAL_ERROR "agent_native must stay linked to agent_runtime, not the full app/server stack.")
endif()

foreach(forbidden_target IN ITEMS agent_platform agent_model_providers agent_memory agent_knowledge agent_runtime_io agent_runtime_io_native agent_runtime_modules agent_mcp agent_mcp_native agent_app agent_server agent_full)
  list(FIND agent_native_link_tokens "${forbidden_target}" forbidden_target_index)
  if(NOT forbidden_target_index EQUAL -1)
    message(FATAL_ERROR
      "agent_native must not link provider/full-stack targets.")
  endif()
endforeach()

if(NOT cmake_lists MATCHES "add_library\\(agent_full INTERFACE\\)")
  message(FATAL_ERROR "agent_full aggregate target is required for explicit full-stack opt-in.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_platform")
  message(FATAL_ERROR "agent_platform target is required for optional native platform transports.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_mcp[ \t\r\n]")
  message(FATAL_ERROR "agent_mcp target is required for optional MCP protocol integration.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_mcp_native[ \t\r\n]")
  message(FATAL_ERROR "agent_mcp_native target is required for optional native MCP transports.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_runtime_modules")
  message(FATAL_ERROR "agent_runtime_modules target is required for explicit high-level runtime modules.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_runtime_io[ \t\r\n]")
  message(FATAL_ERROR "agent_runtime_io target is required for explicit host I/O runtime modules.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_runtime_io_native[ \t\r\n]")
  message(FATAL_ERROR "agent_runtime_io_native target is required for optional native host I/O transports.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_model_providers")
  message(FATAL_ERROR "agent_model_providers target is required for explicit built-in provider opt-in.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_retrieval")
  message(FATAL_ERROR "agent_retrieval target is required for lightweight memory/knowledge runtime contracts.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_memory")
  message(FATAL_ERROR "agent_memory target is required for optional concrete memory implementations.")
endif()

if(NOT cmake_lists MATCHES "add_library\\(agent_knowledge")
  message(FATAL_ERROR "agent_knowledge target is required for optional concrete knowledge implementations.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_runtime PUBLIC agent_tools agent_retrieval\\)")
  message(FATAL_ERROR "agent_runtime must depend on agent_retrieval contracts without concrete memory/knowledge implementations.")
endif()

string(REGEX MATCH "add_library\\(agent_runtime[ \t\r\n]+[^)]*\\)" agent_runtime_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/runtime/agent_loop.cpp
    src/agent/runtime/agent_runner.cpp
    src/agent/runtime/compaction_planner.cpp
    src/agent/runtime/errors.cpp
    src/agent/runtime/helpers.cpp
    src/agent/runtime/input_assembler.cpp
    src/agent/runtime/memory_writeback.cpp
    src/agent/runtime/pipeline.cpp
    src/agent/runtime/prompt_context_builder.cpp
    src/agent/runtime/retrieval_context.cpp
    src/agent/runtime/retrieval_options.cpp
    src/agent/runtime/run_state_codec.cpp
    src/agent/runtime/runner_builder.cpp
    src/agent/runtime/runner_context_stats.cpp
    src/agent/runtime/runner_config.cpp
    src/agent/runtime/runner_execution.cpp
    src/agent/runtime/runner_facades.cpp
    src/agent/runtime/runner_kernel.cpp
    src/agent/runtime/runner_streaming.cpp
    src/agent/runtime/runtime_status.cpp
    src/agent/runtime/runtime_string_conversions.cpp
    src/agent/runtime/skill_preface.cpp
    src/agent/runtime/stream_event_reducer.cpp
    src/agent/runtime/tool_batch_executor.cpp
    src/agent/runtime/tool_call_orchestrator.cpp
    src/agent/runtime/usage_aggregator.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(NOT agent_runtime_block MATCHES "${source_regex}")
    message(FATAL_ERROR
      "agent_runtime must compile ${source} directly; runtime internals must not hide behind runtime.cpp include-splits.")
  endif()
endforeach()
file(GLOB runtime_fragments "${AGENT_SOURCE_DIR}/src/agent/runtime/*.inc")
if(runtime_fragments)
  foreach(fragment IN LISTS runtime_fragments)
    file(RELATIVE_PATH relative_fragment "${AGENT_SOURCE_DIR}" "${fragment}")
    message(FATAL_ERROR
      "${relative_fragment} must stay deleted; use focused runtime/*.cpp modules instead of runtime implementation fragments.")
  endforeach()
endif()
file(READ "${AGENT_SOURCE_DIR}/src/agent/runtime.cpp" runtime_source)
if(runtime_source MATCHES "#include[ \t]+\"runtime/")
  message(FATAL_ERROR "src/agent/runtime.cpp must not include runtime implementation fragments.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_memory PUBLIC agent_retrieval\\)")
  message(FATAL_ERROR "agent_memory must build concrete memory implementations on agent_retrieval.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_knowledge PUBLIC agent_retrieval\\)")
  message(FATAL_ERROR "agent_knowledge must build concrete knowledge implementations on agent_retrieval.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_runtime_io PUBLIC agent_runtime agent_knowledge\\)")
  message(FATAL_ERROR "agent_runtime_io must explicitly opt into agent_knowledge for knowledge I/O helpers.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_runtime_io_native PUBLIC agent_runtime_io agent_platform\\)")
  message(FATAL_ERROR "agent_runtime_io_native must explicitly opt into agent_runtime_io and agent_platform.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_runtime_modules PUBLIC agent_runtime agent_memory\\)")
  message(FATAL_ERROR "agent_runtime_modules must explicitly opt into agent_memory for transcript-backed modules.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_model_providers PUBLIC agent_model\\)")
  message(FATAL_ERROR "agent_model_providers must build on top of provider-neutral agent_model.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_mcp PUBLIC agent_tools\\)")
  message(FATAL_ERROR "agent_mcp must build on top of agent_tools.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_mcp_native PUBLIC agent_mcp agent_platform\\)")
  message(FATAL_ERROR "agent_mcp_native must build on top of MCP protocol contracts and agent_platform.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_app PUBLIC agent_model_providers agent_runtime_modules agent_runtime_io_native agent_mcp_native agent_memory agent_knowledge\\)")
  message(FATAL_ERROR "agent_app must explicitly opt into provider, module, native host I/O, native MCP, memory, and knowledge layers.")
endif()

string(REGEX MATCH "target_link_libraries\\(agent_capi[ \t\r\n]+PUBLIC[^)]*\\)" agent_capi_link_block "${cmake_lists}")
string(REGEX REPLACE "[() \t\r\n]+" ";" agent_capi_link_tokens "${agent_capi_link_block}")
list(FIND agent_capi_link_tokens "agent_runtime" agent_capi_runtime_index)
if(agent_capi_runtime_index EQUAL -1)
  message(FATAL_ERROR "agent_capi must link the embeddable agent_runtime target.")
endif()
foreach(forbidden_target IN ITEMS agent_platform agent_model_providers agent_memory agent_knowledge agent_runtime_io agent_runtime_io_native agent_runtime_modules agent_mcp agent_mcp_native agent_app agent_server agent_full)
  list(FIND agent_capi_link_tokens "${forbidden_target}" forbidden_capi_target_index)
  if(NOT forbidden_capi_target_index EQUAL -1)
    message(FATAL_ERROR
      "agent_capi must stay embeddable; use agent_capi_full for provider/app/runtime_io/runtime_modules.")
  endif()
endforeach()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_capi_full PUBLIC agent_app\\)")
  message(FATAL_ERROR "agent_capi_full must opt into agent_app for full C ABI features.")
endif()

if(NOT cmake_lists MATCHES "target_link_libraries\\(agent_native_embedded_smoke PRIVATE agent_native\\)")
  message(FATAL_ERROR
    "agent_native_embedded_smoke must link agent_native so the embeddable default surface is tested directly.")
endif()

string(REGEX MATCH "add_library\\(agent_model[ \t\r\n]+[^)]*\\)" agent_model_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/model_providers.cpp
    src/agent/providers/native.cpp
    src/agent/providers/openai_compatible.cpp
    src/agent/providers/reasoning.cpp
    src/agent/llama_cpp_native_binding.cpp
    src/agent/llama_cpp_native_binding_stub.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(agent_model_block MATCHES "${source_regex}")
    message(FATAL_ERROR
      "agent_model must stay provider-neutral; put ${source} in agent_model_providers.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_core[ \t\r\n]+[^)]*\\)" agent_core_block "${cmake_lists}")
foreach(platform_source IN ITEMS
    src/agent/http_native.cpp
    src/agent/process_hook.cpp
    src/agent/mcp_native.cpp
    src/agent/media_native.cpp
    src/agent/web_native.cpp)
  string(REPLACE "." "\\." platform_source_regex "${platform_source}")
  if(agent_core_block MATCHES "${platform_source_regex}")
    message(FATAL_ERROR "agent_core must not compile native platform source ${platform_source}.")
  endif()
endforeach()

foreach(core_source IN ITEMS
    src/agent/core.cpp
    src/agent/detail/helpers.cpp)
  file(READ "${AGENT_SOURCE_DIR}/${core_source}" core_source_text)
  foreach(platform_token IN ITEMS
      "_WIN32"
      "__APPLE__"
      "__linux__"
      "unistd.h"
      "sys/"
      "fork\\("
      "socket\\("
      "CreateProcess"
      "LoadLibrary"
      "dlopen")
    if(core_source_text MATCHES "${platform_token}")
      message(FATAL_ERROR
        "${core_source} must stay platform-neutral; ${platform_token} belongs in agent_platform or a native opt-in target.")
    endif()
  endforeach()
endforeach()

string(REGEX MATCH "add_library\\(agent_platform[ \t\r\n]+[^)]*\\)" agent_platform_block "${cmake_lists}")
foreach(platform_source IN ITEMS
    src/agent/http_native.cpp
    src/agent/process_hook.cpp)
  string(REPLACE "." "\\." platform_source_regex "${platform_source}")
  if(NOT agent_platform_block MATCHES "${platform_source_regex}")
    message(FATAL_ERROR "agent_platform must compile ${platform_source}.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_app[ \t\r\n]+[^)]*\\)" agent_app_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/config.cpp
    src/agent/config_helpers.cpp
    src/agent/config_provider_registry.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(NOT agent_app_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_app must compile ${source}.")
  endif()
endforeach()
if(agent_app_block MATCHES "src/agent/process_hook\\.cpp")
  message(FATAL_ERROR "agent_app must use agent_platform for process hooks instead of compiling src/agent/process_hook.cpp.")
endif()
foreach(source IN ITEMS
    src/agent/mcp.cpp
    src/agent/mcp_native.cpp
    src/agent/media_native.cpp
    src/agent/web_native.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(agent_app_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_app must use agent_mcp/agent_mcp_native instead of compiling ${source}.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/src/agent/config.cpp" config_source)
foreach(token IN ITEMS
    "std::map<std::string, NativeChatProviderDescriptor>& native_chat_provider_descriptors"
    "std::map<std::string, NativeTextEmbeddingProviderDescriptor>& native_text_embedding_provider_descriptors"
    "void ensure_builtin_native_chat_provider_descriptors"
    "void ensure_builtin_native_embedding_provider_descriptors"
    "struct OpenAICompatibleProviderProfile")
  if(config_source MATCHES "${token}")
    message(FATAL_ERROR
      "src/agent/config.cpp must stay focused on config validation/app assembly; provider descriptor registry belongs in src/agent/config_provider_registry.cpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/src/agent/config_provider_registry.cpp" config_provider_registry_source)
foreach(token IN ITEMS
    "native_chat_provider_descriptors"
    "ensure_builtin_native_chat_provider_descriptors"
    "native_text_embedding_provider_descriptors"
    "ensure_builtin_native_embedding_provider_descriptors")
  if(NOT config_provider_registry_source MATCHES "${token}")
    message(FATAL_ERROR
      "src/agent/config_provider_registry.cpp must own provider descriptor registration and lookup.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_mcp[ \t\r\n]+[^)]*\\)" agent_mcp_block "${cmake_lists}")
if(NOT agent_mcp_block MATCHES "src/agent/mcp\\.cpp")
  message(FATAL_ERROR "agent_mcp must compile src/agent/mcp.cpp.")
endif()
if(agent_mcp_block MATCHES "src/agent/mcp_native\\.cpp")
  message(FATAL_ERROR "agent_mcp must stay platform-neutral; native transports belong in agent_mcp_native.")
endif()

string(REGEX MATCH "add_library\\(agent_mcp_native[ \t\r\n]+[^)]*\\)" agent_mcp_native_block "${cmake_lists}")
if(NOT agent_mcp_native_block MATCHES "src/agent/mcp_native\\.cpp")
  message(FATAL_ERROR "agent_mcp_native must compile src/agent/mcp_native.cpp.")
endif()
if(agent_mcp_native_block MATCHES "src/agent/mcp\\.cpp")
  message(FATAL_ERROR "agent_mcp_native must not compile the platform-neutral MCP protocol implementation.")
endif()

string(REGEX MATCH "add_library\\(agent_runtime_io[ \t\r\n]+[^)]*\\)" agent_runtime_io_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/media_native.cpp
    src/agent/web_native.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(agent_runtime_io_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_runtime_io must stay platform-neutral; native I/O transports belong in agent_runtime_io_native.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_runtime_io_native[ \t\r\n]+[^)]*\\)" agent_runtime_io_native_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/media_native.cpp
    src/agent/web_native.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(NOT agent_runtime_io_native_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_runtime_io_native must compile ${source}.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_model_providers[ \t\r\n]+[^)]*\\)" agent_model_providers_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/model_providers.cpp
    src/agent/providers/native.cpp
    src/agent/providers/openai_compatible.cpp
    src/agent/providers/reasoning.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(NOT agent_model_providers_block MATCHES "${source_regex}")
    message(FATAL_ERROR
      "agent_model_providers must compile ${source}.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/src/agent/providers/reasoning.cpp" provider_reasoning_source)
foreach(module IN ITEMS common mappers registry serialization profile_policy apply)
  if(NOT provider_reasoning_source MATCHES "reasoning/${module}\\.inc")
    message(FATAL_ERROR
      "src/agent/providers/reasoning.cpp must stay a focused aggregator and include reasoning/${module}.inc.")
  endif()
endforeach()
foreach(token IN ITEMS "class .*ReasoningMapper" "default_reasoning_mode_for_profile" "reasoning_disable_strategy_for_mode")
  if(provider_reasoning_source MATCHES "${token}")
    message(FATAL_ERROR
      "src/agent/providers/reasoning.cpp must not contain mapper/profile policy implementations; keep them in providers/reasoning/*.inc.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/http.hpp" http_header)
foreach(symbol IN ITEMS NativeHttpClientConfig create_native_http_transport create_native_http_streaming_transport)
  if(http_header MATCHES "${symbol}")
    message(FATAL_ERROR
      "agent/http.hpp must stay platform-neutral; ${symbol} belongs in agent/http_native.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/http_native.hpp" http_native_header)
if(NOT http_native_header MATCHES "agent/http\\.hpp")
  message(FATAL_ERROR "agent/http_native.hpp must build on the platform-neutral HTTP contract.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/hooks.hpp" hooks_header)
foreach(symbol IN ITEMS ProcessHookConfig ProcessHookDecision ProcessHookResult run_process_hook make_process_tool_hook)
  if(hooks_header MATCHES "${symbol}")
    message(FATAL_ERROR
      "agent/hooks.hpp must stay platform-neutral; ${symbol} belongs in agent/process_hook.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/process_hook.hpp" process_hook_header)
if(NOT process_hook_header MATCHES "agent/hooks\\.hpp")
  message(FATAL_ERROR "agent/process_hook.hpp must build on the platform-neutral hook contracts.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/mcp.hpp" mcp_header)
if(mcp_header MATCHES "agent/http_native\\.hpp")
  message(FATAL_ERROR "agent/mcp.hpp must stay transport-neutral; native HTTP belongs in agent/mcp_native.hpp.")
endif()
foreach(symbol IN ITEMS MCPNativeHttpTransport MCPStdioTransport create_native_mcp_transport NativeHttpClientConfig)
  if(mcp_header MATCHES "${symbol}")
    message(FATAL_ERROR
      "agent/mcp.hpp must stay transport-neutral; ${symbol} belongs in agent/mcp_native.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/mcp_native.hpp" mcp_native_header)
if(NOT mcp_native_header MATCHES "agent/mcp\\.hpp")
  message(FATAL_ERROR "agent/mcp_native.hpp must build on the platform-neutral MCP contract.")
endif()
if(NOT mcp_native_header MATCHES "agent/http_native\\.hpp")
  message(FATAL_ERROR "agent/mcp_native.hpp must explicitly opt into the native HTTP transport contract.")
endif()

foreach(header IN ITEMS media web)
  file(READ "${AGENT_SOURCE_DIR}/include/agent/${header}.hpp" io_header)
  if(io_header MATCHES "agent/http_native\\.hpp")
    message(FATAL_ERROR "agent/${header}.hpp must depend on agent/http.hpp, not agent/http_native.hpp.")
  endif()
  foreach(symbol IN ITEMS NativeHttpClientConfig create_native_http_transport create_native_http_streaming_transport)
    if(io_header MATCHES "${symbol}")
      message(FATAL_ERROR
        "agent/${header}.hpp must stay host-I/O neutral; ${symbol} belongs in agent/${header}_native.hpp.")
    endif()
  endforeach()
endforeach()

foreach(header IN ITEMS media web)
  file(READ "${AGENT_SOURCE_DIR}/include/agent/${header}_native.hpp" io_native_header)
  if(NOT io_native_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR "agent/${header}_native.hpp must build on agent/${header}.hpp.")
  endif()
  if(NOT io_native_header MATCHES "agent/http_native\\.hpp")
    message(FATAL_ERROR "agent/${header}_native.hpp must explicitly opt into agent/http_native.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/src/agent/mcp.cpp" mcp_source)
foreach(token IN ITEMS
    "agent/app_api.hpp"
    "agent/mcp_native.hpp"
    "agent/http_native.hpp"
    "MCPNativeHttpTransport"
    "MCPStdioTransport"
    "create_native_mcp_transport"
    "#ifndef _WIN32"
    "unistd.h"
    "sys/wait.h")
  if(mcp_source MATCHES "${token}")
    message(FATAL_ERROR
      "src/agent/mcp.cpp must stay platform-neutral; ${token} belongs in src/agent/mcp_native.cpp.")
  endif()
endforeach()

foreach(source IN ITEMS media web)
  file(READ "${AGENT_SOURCE_DIR}/src/agent/${source}.cpp" io_source)
  foreach(token IN ITEMS
      "agent/${source}_native.hpp"
      "agent/http_native.hpp"
      "NativeHttpClientConfig"
      "create_native_http_transport")
    if(io_source MATCHES "${token}")
      message(FATAL_ERROR
        "src/agent/${source}.cpp must stay host-I/O neutral; ${token} belongs in src/agent/${source}_native.cpp.")
    endif()
  endforeach()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/src/agent/memory/knowledge_vector_index.inc" knowledge_vector_index_source)
if(knowledge_vector_index_source MATCHES "create_native_http_transport")
  message(FATAL_ERROR
    "Runtime knowledge vector indexes must require an injected HTTP transport instead of creating native platform HTTP.")
endif()

string(REGEX MATCH "add_library\\(agent_runtime[ \t\r\n]+[^)]*\\)" agent_runtime_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/browser.cpp
    src/agent/async.cpp
    src/agent/autonomous.cpp
    src/agent/knowledge.cpp
    src/agent/knowledge_io.cpp
    src/agent/media.cpp
    src/agent/memory.cpp
    src/agent/memory_vector.cpp
    src/agent/orchestration.cpp
    src/agent/plan.cpp
    src/agent/realtime.cpp
    src/agent/run_transcript_and_layered_memory.cpp
    src/agent/skills_io.cpp
    src/agent/tasks.cpp
    src/agent/web.cpp
    src/agent/workflow.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(agent_runtime_block MATCHES "${source_regex}")
    message(FATAL_ERROR
      "agent_runtime must not compile ${source}; put high-level runtime modules in agent_runtime_modules.")
  endif()
endforeach()

foreach(required_runtime_source IN ITEMS
    src/agent/memory_session.cpp)
  string(REPLACE "." "\\." source_regex "${required_runtime_source}")
  if(NOT agent_runtime_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_runtime must compile ${required_runtime_source}.")
  endif()
endforeach()

foreach(runtime_contract_source IN ITEMS
    src/agent/knowledge_runtime.cpp
    src/agent/memory_retrieval.cpp)
  string(REPLACE "." "\\." source_regex "${runtime_contract_source}")
  if(agent_runtime_block MATCHES "${source_regex}")
    message(FATAL_ERROR
      "agent_runtime must consume ${runtime_contract_source} through agent_retrieval, not compile it directly.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_retrieval[ \t\r\n]+[^)]*\\)" agent_retrieval_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/knowledge_runtime.cpp
    src/agent/memory_retrieval.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(NOT agent_retrieval_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_retrieval must compile ${source}.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_memory[ \t\r\n]+[^)]*\\)" agent_memory_block "${cmake_lists}")
foreach(source IN ITEMS
    src/agent/memory_vector.cpp
    src/agent/run_transcript_and_layered_memory.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(NOT agent_memory_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_memory must compile ${source}.")
  endif()
endforeach()
foreach(source IN ITEMS
    src/agent/memory_session.cpp
    src/agent/knowledge.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(agent_memory_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_memory must not compile ${source}.")
  endif()
endforeach()

string(REGEX MATCH "add_library\\(agent_knowledge[ \t\r\n]+[^)]*\\)" agent_knowledge_block "${cmake_lists}")
if(NOT agent_knowledge_block MATCHES "src/agent/knowledge\\.cpp")
  message(FATAL_ERROR "agent_knowledge must compile src/agent/knowledge.cpp.")
endif()
foreach(source IN ITEMS
    src/agent/memory.cpp
    src/agent/memory_session.cpp
    src/agent/memory_vector.cpp
    src/agent/run_transcript_and_layered_memory.cpp)
  string(REPLACE "." "\\." source_regex "${source}")
  if(agent_knowledge_block MATCHES "${source_regex}")
    message(FATAL_ERROR "agent_knowledge must not compile ${source}.")
  endif()
endforeach()

if(NOT agent_runtime_io_block MATCHES "src/agent/skills_io\\.cpp")
  message(FATAL_ERROR "agent_runtime_io must compile src/agent/skills_io.cpp for opt-in skill file loading.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/agent.hpp" agent_header)
if(agent_header MATCHES "agent/(app_api|server_api)\\.hpp")
  message(FATAL_ERROR "agent/agent.hpp must not include app_api.hpp or server_api.hpp.")
endif()
if(NOT agent_header MATCHES "agent/runtime_api\\.hpp")
  message(FATAL_ERROR "agent/agent.hpp must include runtime_api.hpp.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime_api.hpp" runtime_api_header)
if(runtime_api_header MATCHES "agent/runtime\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_api.hpp must include focused runtime headers, not the runtime.hpp umbrella.")
endif()
if(NOT runtime_api_header MATCHES "agent/runtime_runner\\.hpp")
  message(FATAL_ERROR "agent/runtime_api.hpp must expose the embeddable runner API through agent/runtime_runner.hpp.")
endif()
foreach(header IN ITEMS async autonomous orchestration plan react realtime tasks workflow)
  if(runtime_api_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/runtime_api.hpp must stay limited to the embeddable runner surface; include agent/${header}.hpp explicitly or use agent/full.hpp.")
  endif()
endforeach()
foreach(header IN ITEMS browser web media knowledge_io)
  if(runtime_api_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/runtime_api.hpp must not include agent/${header}.hpp; host I/O modules require agent_runtime_io opt-in.")
  endif()
endforeach()
if(runtime_api_header MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_api.hpp must not include the full memory umbrella; use focused memory headers through runtime_runner/runtime_loop.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/model_api.hpp" model_api_header)
foreach(header IN ITEMS
    model_providers
    providers/native
    providers/openai_compatible
    providers/reasoning)
  if(model_api_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/model_api.hpp must expose provider-neutral model contracts only; include agent/${header}.hpp explicitly from app/full/provider code.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/model.hpp" model_header)
foreach(header IN ITEMS
    providers/native
    providers/openai_compatible
    providers/reasoning
    providers/reasoning_types)
  if(model_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/model.hpp must stay provider-neutral; provider adapters belong in agent/model_providers.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/src/agent/model.cpp" model_source)
foreach(token IN ITEMS
    "agent/model_providers.hpp"
    "agent/providers/native.hpp"
    "agent/providers/openai_compatible.hpp"
    "agent/providers/reasoning.hpp"
    "model/provider_protocols.inc"
    "model/stream_framing.inc"
    "model/stream_incremental.inc"
    "model/provider_adapters.inc"
    "model/provider_helpers.inc"
    "model/provider_embeddings.inc")
  if(model_source MATCHES "${token}")
    message(FATAL_ERROR
      "src/agent/model.cpp must stay provider-neutral; ${token} belongs in src/agent/model_providers.cpp.")
  endif()
endforeach()
foreach(symbol IN ITEMS
    NativeProviderTransport
    NativeProviderRequest
    ProviderRequestProtocol
    ProviderReasoningMode
    OpenAICompatibleChatModelAdapter
    QwenChatModelAdapter
    MiMoChatModelAdapter
    AnthropicChatModelAdapter
    DeepSeekChatModelAdapter
    OllamaChatModelAdapter
    GeminiChatModelAdapter
    LlamaCppNativeChatModelAdapter
    OpenAIEmbeddingAdapter
    QwenEmbeddingAdapter
    OllamaEmbeddingAdapter
    GeminiEmbeddingAdapter
    LlamaCppNativeTextEmbeddingAdapter)
  if(model_header MATCHES "${symbol}")
    message(FATAL_ERROR
      "agent/model.hpp must stay provider-neutral; ${symbol} belongs in agent/model_providers.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime_runner.hpp" runtime_runner_header)
file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime_runner_config.hpp" runtime_runner_config_header)
file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime_runner_api.hpp" runtime_runner_api_header)
file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime_runner_facades.hpp" runtime_runner_facades_header)
set(runtime_runner_surface "${runtime_runner_header}\n${runtime_runner_config_header}\n${runtime_runner_api_header}\n${runtime_runner_facades_header}")
if(runtime_runner_header MATCHES "struct[ \t]+AgentRunnerResolvedConfig[ \t\r\n]*\\{")
  message(FATAL_ERROR
    "agent/runtime_runner.hpp must not expose AgentRunnerResolvedConfig fields; resolved runner config is an implementation detail.")
endif()
foreach(header IN ITEMS
    runtime_runner_types
    runtime_runner_durable
    runtime_runner_stream
    runtime_runner_config
    runtime_runner_facades
    runtime_runner_api
    runtime_builder)
  if(NOT runtime_runner_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/runtime_runner.hpp must aggregate focused agent/${header}.hpp.")
  endif()
endforeach()
string(FIND "${runtime_runner_api_header}" "class AgentRunner" agent_runner_class_index)
if(agent_runner_class_index EQUAL -1)
  message(FATAL_ERROR "agent/runtime_runner_api.hpp must declare AgentRunner.")
endif()
string(FIND "${runtime_runner_api_header}" " private:" agent_runner_private_index)
if(agent_runner_private_index EQUAL -1)
  message(FATAL_ERROR "AgentRunner must keep private implementation state out of the public block.")
endif()
math(EXPR agent_runner_public_length "${agent_runner_private_index} - ${agent_runner_class_index}")
string(SUBSTRING "${runtime_runner_api_header}" ${agent_runner_class_index} ${agent_runner_public_length} agent_runner_public_block)
foreach(symbol IN ITEMS
    "register_tool\\("
    "register_context\\("
    "register_event_sink\\("
    "unregister_event_sink\\("
    "get_session\\("
    "compact_session\\("
    "session_store\\("
    "scratch_store\\("
    "adapter\\("
    "thinking_adapter\\("
    "critique_adapter\\("
    "event_bus\\("
    "estimate_context_stats\\("
    "last_context_stats\\("
    "run\\("
    "stream\\("
    "stream_events\\(")
  if(agent_runner_public_block MATCHES "${symbol}")
    message(FATAL_ERROR
      "AgentRunner public API must stay facade-based; ${symbol} belongs behind Runner* facades.")
  endif()
endforeach()
foreach(facade IN ITEMS RunnerTools RunnerContexts RunnerEvents RunnerSessions RunnerModels RunnerContextStats RunnerExecution RunnerStreaming)
  if(NOT runtime_runner_facades_header MATCHES "class[ \t]+${facade}")
    message(FATAL_ERROR "agent/runtime_runner_facades.hpp must declare ${facade}.")
  endif()
endforeach()
if(NOT runtime_runner_facades_header MATCHES "Native C\\+\\+ SDK surface")
  message(FATAL_ERROR
    "agent/runtime_runner_facades.hpp must document that STL-based C++ facades are native SDK APIs, not the cross-language ABI.")
endif()
file(READ "${AGENT_SOURCE_DIR}/docs/bindings.md" bindings_doc)
foreach(required_text IN ITEMS
    "thin **C ABI shim**"
    "None of them share a stable"
    "C++ types like `std::string`")
  string(FIND "${bindings_doc}" "${required_text}" bindings_doc_required_index)
  if(bindings_doc_required_index EQUAL -1)
    message(FATAL_ERROR
      "docs/bindings.md must keep the C ABI as the documented cross-language boundary.")
  endif()
endforeach()
if(runtime_runner_surface MATCHES "agent/skills\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_runner focused headers must depend on agent/skills_core.hpp, not the full skill loader API.")
endif()
if(runtime_runner_surface MATCHES "agent/knowledge_core\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_runner focused headers must depend on agent/knowledge_runtime.hpp, not full agent/knowledge_core.hpp.")
endif()
if(NOT runtime_runner_config_header MATCHES "agent/knowledge_runtime\\.hpp")
  message(FATAL_ERROR "agent/runtime_runner_config.hpp must include agent/knowledge_runtime.hpp.")
endif()
if(NOT runtime_runner_config_header MATCHES "agent/skills_core\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_runner_config.hpp must expose runner skill contracts through agent/skills_core.hpp.")
endif()
if(runtime_runner_surface MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_runner focused headers must depend on focused memory_retrieval.hpp/memory_session.hpp, not the full memory umbrella.")
endif()
foreach(header IN ITEMS memory_retrieval memory_session)
  if(NOT runtime_runner_config_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/runtime_runner_config.hpp must include agent/${header}.hpp for the embeddable runner memory surface.")
  endif()
endforeach()
foreach(header IN ITEMS memory_vector memory_layered run_transcript)
  if(runtime_runner_surface MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/runtime_runner focused headers must not include agent/${header}.hpp; optional memory implementations require explicit headers.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime_loop.hpp" runtime_loop_header)
if(runtime_loop_header MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR
    "agent/runtime_loop.hpp must depend on focused memory_session.hpp, not the full memory umbrella.")
endif()
if(NOT runtime_loop_header MATCHES "agent/memory_session\\.hpp")
  message(FATAL_ERROR "agent/runtime_loop.hpp must include agent/memory_session.hpp.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/context_stats.hpp" context_stats_header)
if(context_stats_header MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR
    "agent/context_stats.hpp must depend on focused memory_session.hpp, not the full memory umbrella.")
endif()
if(NOT context_stats_header MATCHES "agent/memory_session\\.hpp")
  message(FATAL_ERROR "agent/context_stats.hpp must include agent/memory_session.hpp.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/context.hpp" context_header)
if(context_header MATCHES "agent/knowledge_core\\.hpp")
  message(FATAL_ERROR
    "agent/context.hpp must depend on agent/knowledge_runtime.hpp, not full agent/knowledge_core.hpp.")
endif()
if(NOT context_header MATCHES "agent/knowledge_runtime\\.hpp")
  message(FATAL_ERROR "agent/context.hpp must include agent/knowledge_runtime.hpp.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/memory.hpp" memory_header)
foreach(header IN ITEMS memory_layered memory_retrieval memory_session memory_vector run_transcript)
  if(NOT memory_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR "agent/memory.hpp must remain a thin umbrella over agent/${header}.hpp.")
  endif()
endforeach()
foreach(symbol IN ITEMS
    SessionMemory
    LongTermMemory
    FileLayeredMemoryStore
    RunTranscript)
  if(memory_header MATCHES "class[ \t]+${symbol}|struct[ \t]+${symbol}")
    message(FATAL_ERROR
      "agent/memory.hpp must stay a thin umbrella; concrete declarations belong in focused memory headers.")
  endif()
endforeach()
foreach(io_symbol IN ITEMS
    RepositoryKnowledgeSourceLoader
    WebKnowledgeSourceLoader
    WebsiteKnowledgeSourceLoader
    SitemapKnowledgeSourceLoader
    create_web_enabled_knowledge_loader
    create_web_enabled_knowledge_loader_registry)
  if(memory_header MATCHES "${io_symbol}")
    message(FATAL_ERROR
      "agent/memory.hpp must stay core-safe; ${io_symbol} belongs in agent/knowledge_io.hpp.")
  endif()
endforeach()
foreach(knowledge_symbol IN ITEMS
    KnowledgeAssetType
    LoadedKnowledgeDocument
    KnowledgeSourceLoader
    KnowledgeLoaderRegistry
    KnowledgeSearchHit
    KnowledgeStore
    KnowledgeTextIndex
    KnowledgeVectorIndex
    KnowledgeReranker
    KnowledgeBase
    KnowledgeBaseManager)
  if(memory_header MATCHES "${knowledge_symbol}")
    message(FATAL_ERROR
      "agent/memory.hpp must stay memory-only; ${knowledge_symbol} belongs in agent/knowledge_core.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/knowledge.hpp" knowledge_header)
if(NOT knowledge_header MATCHES "agent/knowledge_core\\.hpp")
  message(FATAL_ERROR "agent/knowledge.hpp must expose the core knowledge API through agent/knowledge_core.hpp.")
endif()
file(READ "${AGENT_SOURCE_DIR}/include/agent/knowledge_io.hpp" knowledge_io_header)
if(NOT knowledge_io_header MATCHES "agent/knowledge_core\\.hpp")
  message(FATAL_ERROR "agent/knowledge_io.hpp must build on agent/knowledge_core.hpp.")
endif()
if(knowledge_io_header MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR "agent/knowledge_io.hpp must not depend on agent/memory.hpp.")
endif()

file(READ "${AGENT_SOURCE_DIR}/include/agent/runtime.hpp" runtime_header)
if(NOT runtime_header MATCHES "agent/runtime_loop\\.hpp")
  message(FATAL_ERROR "agent/runtime.hpp must aggregate agent/runtime_loop.hpp.")
endif()
if(NOT runtime_header MATCHES "agent/runtime_runner\\.hpp")
  message(FATAL_ERROR "agent/runtime.hpp must aggregate agent/runtime_runner.hpp.")
endif()
foreach(symbol IN ITEMS AgentRunner AgentRunnerConfig AgentLoopStreamEvent AgentLoopDurableState)
  if(runtime_header MATCHES "${symbol}")
    message(FATAL_ERROR
      "agent/runtime.hpp must stay an umbrella header; concrete runtime declarations belong in focused headers.")
  endif()
endforeach()
foreach(header IN ITEMS async autonomous orchestration plan realtime tasks workflow)
  if(runtime_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/runtime.hpp must not include agent/${header}.hpp; high-level runtime modules require explicit headers.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/tools.hpp" tools_header)
foreach(symbol IN ITEMS
    kToolServiceWorkflowEngine
    kToolServiceWorkflowDefinition
    kToolServiceWebSearchRegistry
    kToolServiceDefaultSearchProvider
    kToolServiceWebFetcher
    kToolServiceWebCrawler
    kToolServiceDefaultCrawlerProfile
    kToolServiceBrowserRenderer
    kToolServiceMediaArtifactLookup
    kToolServiceOcrRegistry
    kToolServiceDefaultOcrProvider
    kToolServiceDocumentRasterizers
    kToolServiceDocumentPreprocessors
    kToolServiceMediaGenerationRegistry
    kToolServiceDefaultMediaGenerationProvider)
  if(tools_header MATCHES "${symbol}")
    message(FATAL_ERROR
      "agent/tools.hpp must expose only core/runtime service tokens; ${symbol} belongs in tool_services_io.hpp or tool_services_modules.hpp.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/builtins.hpp" builtins_header)
foreach(header IN ITEMS tool_services_io tool_services_modules)
  if(NOT builtins_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR
      "agent/builtins.hpp must explicitly include agent/${header}.hpp for optional builtin tool services.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/full.hpp" full_header)
if(NOT full_header MATCHES "agent/server_api\\.hpp")
  message(FATAL_ERROR "agent/full.hpp must include server_api.hpp.")
endif()
if(NOT full_header MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR "agent/full.hpp must include agent/memory.hpp for explicit full-stack memory APIs.")
endif()
foreach(header IN ITEMS async autonomous orchestration plan react realtime tasks workflow)
  if(NOT full_header MATCHES "agent/${header}\\.hpp")
    message(FATAL_ERROR "agent/full.hpp must include agent/${header}.hpp for explicit full-stack opt-in.")
  endif()
endforeach()

message(STATUS "native target boundary policy OK")
