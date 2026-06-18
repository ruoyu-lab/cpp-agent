if(NOT DEFINED AGENT_SOURCE_DIR)
  message(FATAL_ERROR "AGENT_SOURCE_DIR is required.")
endif()
get_filename_component(AGENT_SOURCE_ABS_DIR "${AGENT_SOURCE_DIR}" ABSOLUTE)

function(require_file relative_path)
  if(NOT EXISTS "${AGENT_SOURCE_DIR}/${relative_path}")
    message(FATAL_ERROR "Required platform governance file is missing: ${relative_path}")
  endif()
endfunction()

function(count_file_lines absolute_path out_var)
  file(READ "${absolute_path}" content)
  set(newlines "")
  string(REGEX MATCHALL "\n" newlines "${content}")
  list(LENGTH newlines newline_count)
  if("${content}" STREQUAL "")
    set(line_count 0)
  else()
    math(EXPR line_count "${newline_count} + 1")
  endif()
  set(${out_var} "${line_count}" PARENT_SCOPE)
endfunction()

function(enforce_line_limit relative_path max_lines)
  set(path "${AGENT_SOURCE_DIR}/${relative_path}")
  require_file("${relative_path}")
  count_file_lines("${path}" line_count)
  if(line_count GREATER max_lines)
    message(FATAL_ERROR
      "Platform architecture policy failed: ${relative_path} has ${line_count} lines; limit is ${max_lines}.")
  endif()
endfunction()

function(enforce_glob_line_limit glob_pattern max_lines label)
  file(GLOB files "${AGENT_SOURCE_DIR}/${glob_pattern}")
  if(NOT files)
    message(FATAL_ERROR "Platform architecture policy failed: no files matched ${glob_pattern}.")
  endif()
  foreach(path IN LISTS files)
    count_file_lines("${path}" line_count)
    if(line_count GREATER max_lines)
      file(RELATIVE_PATH relative_path "${AGENT_SOURCE_DIR}" "${path}")
      message(FATAL_ERROR
        "Platform architecture policy failed: ${label} ${relative_path} has ${line_count} lines; limit is ${max_lines}.")
    endif()
  endforeach()
endfunction()

function(enforce_no_glob_matches glob_pattern label)
  file(GLOB files "${AGENT_SOURCE_DIR}/${glob_pattern}")
  if(files)
    foreach(path IN LISTS files)
      file(RELATIVE_PATH relative_path "${AGENT_SOURCE_DIR}" "${path}")
      message(FATAL_ERROR
        "Platform architecture policy failed: ${label} is not allowed at ${relative_path}.")
    endforeach()
  endif()
endfunction()

set(platform_token_allowed_files
  src/agent/builtins.cpp
  src/agent/http_native.cpp
  src/agent/llama_cpp_native_binding.cpp
  src/agent/mcp_native.cpp
  src/agent/process_hook.cpp
)

set(platform_tokens
  "_WIN32"
  "__APPLE__"
  "__linux__"
  "unistd\\.h"
  "sys/"
  "::fork\\("
  "::socket\\("
  "CreateProcess"
  "LoadLibrary"
  "GetProcAddress"
  "dlopen"
  "dlsym"
  "popen\\("
  "pclose\\("
  "WIFEXITED"
  "WEXITSTATUS"
)

file(GLOB_RECURSE platform_scan_files
  "${AGENT_SOURCE_ABS_DIR}/include/agent/*.h"
  "${AGENT_SOURCE_ABS_DIR}/include/agent/*.hpp"
  "${AGENT_SOURCE_ABS_DIR}/src/agent/*.cpp"
  "${AGENT_SOURCE_ABS_DIR}/src/agent/*.hpp"
  "${AGENT_SOURCE_ABS_DIR}/src/agent/*.inc"
)
foreach(path IN LISTS platform_scan_files)
  file(RELATIVE_PATH relative_path "${AGENT_SOURCE_ABS_DIR}" "${path}")
  list(FIND platform_token_allowed_files "${relative_path}" allowed_platform_file_index)
  file(READ "${path}" source_text)
  foreach(token IN LISTS platform_tokens)
    if(source_text MATCHES "${token}" AND allowed_platform_file_index EQUAL -1)
      message(FATAL_ERROR
        "Platform architecture policy failed: ${relative_path} contains platform token ${token}; move it to agent_platform/native opt-in code or add a deliberate allow-list entry.")
    endif()
  endforeach()
endforeach()

foreach(path IN ITEMS
  src/agent/knowledge.cpp
  src/agent/memory_retrieval.cpp
  src/agent/memory_session.cpp
  src/agent/memory_vector.cpp
  src/agent/model.cpp
  src/agent/model_providers.cpp
  src/agent/run_transcript_and_layered_memory.cpp
  src/agent/runtime.cpp
  src/agent/server.cpp
)
  enforce_line_limit("${path}" 120)
endforeach()

enforce_line_limit("src/agent/knowledge_runtime.cpp" 180)
enforce_line_limit("src/agent/http.cpp" 260)
enforce_line_limit("src/agent/http_native.cpp" 750)
enforce_line_limit("src/agent/config.cpp" 3000)
enforce_line_limit("src/agent/config_helpers.cpp" 160)
enforce_line_limit("src/agent/config_helpers.hpp" 80)
enforce_line_limit("src/agent/config_provider_registry.cpp" 1400)
enforce_line_limit("src/agent/media_native.cpp" 80)
enforce_line_limit("src/agent/mcp_native.cpp" 520)
enforce_line_limit("src/agent/process_hook.cpp" 320)
enforce_line_limit("src/agent/web_native.cpp" 100)

foreach(path IN ITEMS
  include/agent/http.hpp
  include/agent/http_native.hpp
  include/agent/media_native.hpp
  include/agent/memory.hpp
  include/agent/memory_retrieval.hpp
  include/agent/mcp_native.hpp
  include/agent/process_hook.hpp
  include/agent/run_transcript.hpp
  include/agent/web_native.hpp
  include/agent/memory_layered.hpp
)
  enforce_line_limit("${path}" 120)
endforeach()

enforce_line_limit("include/agent/knowledge_runtime.hpp" 180)

foreach(path IN ITEMS
  include/agent/memory_session.hpp
  include/agent/memory_vector.hpp
)
  enforce_line_limit("${path}" 420)
endforeach()

foreach(path IN ITEMS
  src/agent/memory/knowledge_helpers.inc
  src/agent/memory/session_memory.inc
  src/agent/memory/knowledge_sources.inc
  src/agent/memory/knowledge_chunking.inc
  src/agent/memory/knowledge_stores.inc
  src/agent/memory/knowledge_text_index.inc
  src/agent/memory/vector_memory.inc
  src/agent/memory/knowledge_vector_index.inc
  src/agent/memory/knowledge_rerankers.inc
  src/agent/memory/knowledge_base.inc
  src/agent/memory/knowledge_manager.inc
  src/agent/model/core_helpers.inc
  src/agent/model/embedding_core_helpers.inc
  src/agent/model/tagged_reasoning_helpers.inc
  src/agent/model/provider_helpers.inc
  src/agent/model/usage.inc
  src/agent/model/chat_core.inc
  src/agent/model/stream_framing.inc
  src/agent/model/provider_protocols.inc
  src/agent/model/stream_incremental.inc
  src/agent/model/provider_adapters.inc
  src/agent/model/embeddings_core.inc
  src/agent/model/provider_embeddings.inc
  src/agent/runtime/internal.hpp
  src/agent/runtime/agent_loop.cpp
  src/agent/runtime/agent_runner.cpp
  src/agent/runtime/compaction_planner.hpp
  src/agent/runtime/compaction_planner.cpp
  src/agent/runtime/errors.cpp
  src/agent/runtime/helpers.cpp
  src/agent/runtime/input_assembler.cpp
  src/agent/runtime/memory_writeback.hpp
  src/agent/runtime/memory_writeback.cpp
  src/agent/runtime/pipeline.cpp
  src/agent/runtime/prompt_context_builder.hpp
  src/agent/runtime/prompt_context_builder.cpp
  src/agent/runtime/retrieval_context.cpp
  src/agent/runtime/retrieval_options.cpp
  src/agent/runtime/run_state_codec.hpp
  src/agent/runtime/run_state_codec.cpp
  src/agent/runtime/runner_builder.cpp
  src/agent/runtime/runner_context_stats.cpp
  src/agent/runtime/runner_config.hpp
  src/agent/runtime/runner_config.cpp
  src/agent/runtime/runner_execution.cpp
  src/agent/runtime/runner_facades.cpp
  src/agent/runtime/runner_kernel.hpp
  src/agent/runtime/runner_kernel.cpp
  src/agent/runtime/runner_streaming.cpp
  src/agent/runtime/runtime_status.cpp
  src/agent/runtime/runtime_string_conversions.cpp
  src/agent/runtime/skill_preface.cpp
  src/agent/runtime/stream_event_reducer.hpp
  src/agent/runtime/stream_event_reducer.cpp
  src/agent/runtime/tool_batch_executor.hpp
  src/agent/runtime/tool_batch_executor.cpp
  src/agent/runtime/tool_call_orchestrator.hpp
  src/agent/runtime/tool_call_orchestrator.cpp
  src/agent/runtime/usage_aggregator.hpp
  src/agent/runtime/usage_aggregator.cpp
  src/agent/server/route_modules.inc
  src/agent/server/app_core.inc
  src/agent/server/approval_stores.inc
  src/agent/server/governance_policy.inc
  src/agent/server/task_routes.inc
  src/agent/server/access_control.inc
  src/agent/server/http_trace_helpers.inc
  src/agent/server/run_result_serialization.inc
  src/agent/server/autonomous_routes.inc
  src/agent/server/workflow_routes.inc
  src/agent/server/metrics.inc
  src/agent/server/chat_routes.inc
  src/agent/server/request_value_parsing.inc
  src/agent/server/governance_summary_values.inc
  src/agent/server/approval_codec.inc
  src/agent/server/value_serialization.inc
  src/agent/server/approval_routes.inc
  src/agent/server/query_filters.inc
  src/agent/server/audit_values.inc
  src/agent/server/session_access.inc
  src/agent/server/workflow_runtime_access.inc
)
  require_file("${path}")
endforeach()

enforce_glob_line_limit("src/agent/server/*.inc" 750 "server implementation fragment")
enforce_no_glob_matches("src/agent/runtime/*.inc" "runtime implementation fragment")
enforce_glob_line_limit("src/agent/runtime/*.cpp" 1500 "runtime internal source")
enforce_glob_line_limit("src/agent/model/*.inc" 2000 "model implementation fragment")
enforce_glob_line_limit("src/agent/memory/*.inc" 1600 "memory implementation fragment")

require_file("include/agent_capi.h")
file(READ "${AGENT_SOURCE_DIR}/include/agent_capi.h" capi_header)
if(NOT capi_header MATCHES "#define[ \t]+AGENT_CAPI_ABI_VERSION[ \t]+[0-9]+")
  message(FATAL_ERROR "agent_capi.h must define numeric AGENT_CAPI_ABI_VERSION.")
endif()
if(NOT capi_header MATCHES "agent_capi_abi_version")
  message(FATAL_ERROR "agent_capi.h must expose agent_capi_abi_version().")
endif()
if(NOT capi_header MATCHES "No C\\+\\+ exceptions propagate across the C boundary")
  message(FATAL_ERROR "agent_capi.h must document the no-exceptions C ABI contract.")
endif()
foreach(symbol IN ITEMS
  agent_runner_create_with_echo_model
  agent_capi_contract_json
  agent_runner_run
  agent_runner_run_json
  agent_runner_stream
  agent_runner_stream_json
)
  if(NOT capi_header MATCHES "${symbol}")
    message(FATAL_ERROR "agent_capi.h must expose ${symbol}().")
  endif()
endforeach()
foreach(symbol IN ITEMS
  agent_runner_create_from_config_json
  agent_runner_create_from_config_path
  agent_async_runtime_create
  agent_async_run_start_json
)
  if(capi_header MATCHES "${symbol}")
    message(FATAL_ERROR "agent_capi.h must not expose full-only ${symbol}(); use agent_capi_full.h.")
  endif()
endforeach()

require_file("include/agent_capi_full.h")
file(READ "${AGENT_SOURCE_DIR}/include/agent_capi_full.h" capi_full_header)
if(NOT capi_full_header MATCHES "agent_capi.h")
  message(FATAL_ERROR "agent_capi_full.h must extend agent_capi.h.")
endif()
foreach(symbol IN ITEMS
  agent_runner_create_from_config_json
  agent_runner_create_from_config_path
  agent_async_runtime_create
  agent_async_run_start_json
)
  if(NOT capi_full_header MATCHES "${symbol}")
    message(FATAL_ERROR "agent_capi_full.h must expose ${symbol}().")
  endif()
endforeach()

require_file("src/agent_capi.cpp")
file(READ "${AGENT_SOURCE_DIR}/src/agent_capi.cpp" capi_source)
if(capi_source MATCHES "__DATE__|__TIME__")
  message(FATAL_ERROR "agent_capi.cpp must not use build-date macros for release versioning.")
endif()
if(NOT capi_source MATCHES "AGENT_NATIVE_VERSION")
  message(FATAL_ERROR "agent_capi.cpp must use the configured AGENT_NATIVE_VERSION.")
endif()

require_file("tests/capi_smoke.c")
file(READ "${AGENT_SOURCE_DIR}/tests/capi_smoke.c" capi_smoke)
if(NOT capi_smoke MATCHES "agent_capi_abi_version\\(\\)")
  message(FATAL_ERROR "C ABI smoke test must check agent_capi_abi_version().")
endif()
foreach(symbol IN ITEMS
  agent_runner_create_from_config_json
  agent_runner_create_from_config_path
  agent_capi_contract_json
  agent_runner_run_json
  agent_runner_stream_json
)
  if(NOT capi_smoke MATCHES "${symbol}\\(")
    message(FATAL_ERROR "C ABI smoke test must exercise ${symbol}().")
  endif()
endforeach()

foreach(path IN ITEMS
  include/agent/knowledge_core.hpp
  include/agent/knowledge.hpp
  include/agent/knowledge_io.hpp
)
  require_file("${path}")
endforeach()
file(READ "${AGENT_SOURCE_DIR}/include/agent/knowledge.hpp" knowledge_header)
if(NOT knowledge_header MATCHES "agent/knowledge_core\\.hpp")
  message(FATAL_ERROR "include/agent/knowledge.hpp must expose agent/knowledge_core.hpp.")
endif()
file(READ "${AGENT_SOURCE_DIR}/include/agent/knowledge_io.hpp" knowledge_io_header)
if(NOT knowledge_io_header MATCHES "agent/knowledge_core\\.hpp")
  message(FATAL_ERROR "include/agent/knowledge_io.hpp must build on agent/knowledge_core.hpp.")
endif()
if(knowledge_io_header MATCHES "agent/memory\\.hpp")
  message(FATAL_ERROR "include/agent/knowledge_io.hpp must not depend on agent/memory.hpp.")
endif()

foreach(path IN ITEMS
  contracts/public-surface/capi-runtime-symbols.txt
  contracts/public-surface/capi-symbols.txt
  contracts/public-surface/public-headers-main.txt
  contracts/public-surface/public-headers-expert.txt
  contracts/public-surface/public-headers-full.txt
  contracts/observable/capi.json
  contracts/observable/config-defaults.json
  contracts/observable/events.json
  contracts/observable/messages.json
  contracts/observable/react.json
  contracts/observable/schemas.json
  contracts/observable/tool-results.json
)
  require_file("${path}")
endforeach()

require_file("tests/capi_ctypes_smoke.py")
require_file("tests/check_capi_symbols.py")
require_file("tests/check_doc_example_closure.py")

file(STRINGS "${AGENT_SOURCE_DIR}/contracts/public-surface/public-headers-main.txt" main_headers)
list(FIND main_headers "include/agent/knowledge_runtime.hpp" knowledge_runtime_header_index)
if(knowledge_runtime_header_index EQUAL -1)
  message(FATAL_ERROR
    "contracts/public-surface/public-headers-main.txt must list include/agent/knowledge_runtime.hpp.")
endif()
foreach(required_main_header IN ITEMS
  include/agent/memory_retrieval.hpp
  include/agent/memory_session.hpp
)
  list(FIND main_headers "${required_main_header}" required_main_header_index)
  if(required_main_header_index EQUAL -1)
    message(FATAL_ERROR
      "contracts/public-surface/public-headers-main.txt must list ${required_main_header}.")
  endif()
endforeach()
foreach(path IN LISTS main_headers)
  require_file("${path}")
endforeach()

foreach(forbidden_main_header IN ITEMS
  include/agent/app_api.hpp
  include/agent/server_api.hpp
  include/agent/full.hpp
  include/agent/http_native.hpp
  include/agent/knowledge.hpp
  include/agent/knowledge_core.hpp
  include/agent/media_native.hpp
  include/agent/mcp_native.hpp
  include/agent/web_native.hpp
  include/agent/memory.hpp
  include/agent/memory_layered.hpp
  include/agent/memory_vector.hpp
  include/agent/run_transcript.hpp
  include/agent/async.hpp
  include/agent/autonomous.hpp
  include/agent/orchestration.hpp
  include/agent/plan.hpp
  include/agent/presets.hpp
  include/agent/process_hook.hpp
  include/agent/react.hpp
  include/agent/realtime.hpp
  include/agent/server.hpp
  include/agent/skills.hpp
  include/agent/tasks.hpp
  include/agent/workflow.hpp
  include/agent_capi_full.h
)
  list(FIND main_headers "${forbidden_main_header}" forbidden_main_header_index)
  if(NOT forbidden_main_header_index EQUAL -1)
    message(FATAL_ERROR
      "contracts/public-surface/public-headers-main.txt must stay embeddable; ${forbidden_main_header} belongs in the expert/full surface.")
  endif()
endforeach()

file(STRINGS "${AGENT_SOURCE_DIR}/contracts/public-surface/public-headers-expert.txt" expert_headers)
foreach(path IN LISTS expert_headers)
  require_file("${path}")
endforeach()

file(STRINGS "${AGENT_SOURCE_DIR}/contracts/public-surface/public-headers-full.txt" full_headers)
foreach(path IN LISTS full_headers)
  require_file("${path}")
endforeach()

foreach(required_full_header IN ITEMS
  include/agent/app_api.hpp
  include/agent/server_api.hpp
  include/agent/full.hpp
  include/agent/http_native.hpp
  include/agent/memory.hpp
  include/agent/memory_layered.hpp
  include/agent/memory_vector.hpp
  include/agent/model_providers.hpp
  include/agent/media_native.hpp
  include/agent/mcp_native.hpp
  include/agent/process_hook.hpp
  include/agent/tool_services_io.hpp
  include/agent/tool_services_modules.hpp
  include/agent/web_native.hpp
  include/agent_capi_full.h
)
  list(FIND full_headers "${required_full_header}" required_full_header_index)
  if(required_full_header_index EQUAL -1)
    message(FATAL_ERROR
      "contracts/public-surface/public-headers-full.txt must list ${required_full_header}.")
  endif()
endforeach()

file(READ "${AGENT_SOURCE_DIR}/include/agent/agent.hpp" aggregate_header)
if(aggregate_header MATCHES "agent/react\\.hpp")
  message(FATAL_ERROR
    "include/agent/agent.hpp must not include agent/react.hpp; ReAct expert APIs must stay behind explicit expert headers.")
endif()

foreach(path IN LISTS main_headers)
  if(path MATCHES "react\\.hpp$")
    message(FATAL_ERROR
      "contracts/public-surface/public-headers-main.txt must not list ReAct expert headers.")
  endif()
endforeach()

require_file("docs/release-governance.md")
file(READ "${AGENT_SOURCE_DIR}/docs/release-governance.md" release_governance)
if(NOT release_governance MATCHES "ABI")
  message(FATAL_ERROR "docs/release-governance.md must describe ABI governance.")
endif()
if(NOT release_governance MATCHES "Release Checklist")
  message(FATAL_ERROR "docs/release-governance.md must include a Release Checklist.")
endif()

message(STATUS "platform governance policy OK")
