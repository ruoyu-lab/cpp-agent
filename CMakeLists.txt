cmake_minimum_required(VERSION 3.20)

project(native_agent_framework VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / IDE LSPs know the include paths,
# C++ standard, and per-file flags. Without this, editors flag every
# `agent/*.hpp` include as "file not found" and every `std::string` as
# "type not found" even though the actual cmake build is clean.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(CTest)

option(AGENT_NATIVE_ENABLE_LLAMA_CPP "Build the built-in llama.cpp native binding" OFF)
option(AGENT_NATIVE_BUILD_CLI "Build the native CLI executable" ON)
option(AGENT_NATIVE_BUILD_EXAMPLES "Build example executables" ON)
set(LLAMA_CPP_INCLUDE_DIR "" CACHE PATH "Directory containing llama.h")
set(GGML_INCLUDE_DIR "" CACHE PATH "Directory containing ggml-backend.h")

add_library(agent_core
  src/agent/core.cpp
  src/agent/detail/helpers.cpp
  src/agent/execution.cpp
  src/agent/hooks.cpp
  src/agent/http.cpp
  src/agent/messages.cpp
  src/agent/observability.cpp
)
target_include_directories(agent_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_library(agent_platform
  src/agent/http_native.cpp
  src/agent/process_hook.cpp
)
target_link_libraries(agent_platform PUBLIC agent_core)

add_library(agent_model
  src/agent/model.cpp
)
target_link_libraries(agent_model PUBLIC agent_core)

add_library(agent_retrieval
  src/agent/knowledge_runtime.cpp
  src/agent/memory_retrieval.cpp
)
target_link_libraries(agent_retrieval PUBLIC agent_model)

add_library(agent_model_providers
  src/agent/model_providers.cpp
  src/agent/providers/native.cpp
  src/agent/providers/openai_compatible.cpp
  src/agent/providers/reasoning.cpp
)
target_link_libraries(agent_model_providers PUBLIC agent_model)

add_library(agent_tools
  src/agent/sandbox.cpp
  src/agent/scratch.cpp
  src/agent/security_governance.cpp
  src/agent/shell_classifier.cpp
  src/agent/tool_runs.cpp
  src/agent/tools.cpp
)
target_link_libraries(agent_tools PUBLIC agent_model)

add_library(agent_mcp
  src/agent/mcp.cpp
)
target_link_libraries(agent_mcp PUBLIC agent_tools)

add_library(agent_mcp_native
  src/agent/mcp_native.cpp
)
target_link_libraries(agent_mcp_native PUBLIC agent_mcp agent_platform)

add_library(agent_runtime
  src/agent/context.cpp
  src/agent/context_stats.cpp
  src/agent/knowledge/strategies.cpp
  src/agent/memory_session.cpp
  src/agent/react.cpp
  src/agent/runtime.cpp
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
  src/agent/runtime/usage_aggregator.cpp
  src/agent/skills.cpp
)
target_link_libraries(agent_runtime PUBLIC agent_tools agent_retrieval)

add_library(agent_memory
  src/agent/memory_vector.cpp
  src/agent/run_transcript_and_layered_memory.cpp
)
target_link_libraries(agent_memory PUBLIC agent_retrieval)

add_library(agent_knowledge
  src/agent/knowledge.cpp
)
target_link_libraries(agent_knowledge PUBLIC agent_retrieval)

add_library(agent_runtime_io
  src/agent/browser.cpp
  src/agent/knowledge_io.cpp
  src/agent/media.cpp
  src/agent/skills_io.cpp
  src/agent/web.cpp
)
target_link_libraries(agent_runtime_io PUBLIC agent_runtime agent_knowledge)

add_library(agent_runtime_io_native
  src/agent/media_native.cpp
  src/agent/web_native.cpp
)
target_link_libraries(agent_runtime_io_native PUBLIC agent_runtime_io agent_platform)

add_library(agent_runtime_modules
  src/agent/async.cpp
  src/agent/autonomous.cpp
  src/agent/orchestration.cpp
  src/agent/plan.cpp
  src/agent/realtime.cpp
  src/agent/tasks.cpp
  src/agent/workflow.cpp
)
target_link_libraries(agent_runtime_modules PUBLIC agent_runtime agent_memory)

add_library(agent_app
  src/agent/builtins.cpp
  src/agent/cli.cpp
  src/agent/config.cpp
  src/agent/config_helpers.cpp
  src/agent/config_provider_registry.cpp
  src/agent/media_generation.cpp
  src/agent/presets.cpp
  src/agent/replay_eval.cpp
  src/agent/tool_client.cpp
)
target_link_libraries(agent_app PUBLIC agent_model_providers agent_runtime_modules agent_runtime_io_native agent_mcp_native agent_memory agent_knowledge)

add_library(agent_server
  src/agent/server.cpp
  src/agent/server/routing.cpp
  src/agent/server/session_routes.cpp
)
target_link_libraries(agent_server PUBLIC agent_app)

add_library(agent_full INTERFACE)
target_link_libraries(agent_full INTERFACE agent_server)

add_library(agent_native INTERFACE)
target_link_libraries(agent_native INTERFACE agent_runtime)

if(AGENT_NATIVE_ENABLE_LLAMA_CPP)
  target_sources(agent_model_providers PRIVATE src/agent/llama_cpp_native_binding.cpp)
  if(NOT LLAMA_CPP_INCLUDE_DIR AND DEFINED ENV{LLAMA_CPP_INCLUDE_DIR})
    set(LLAMA_CPP_INCLUDE_DIR "$ENV{LLAMA_CPP_INCLUDE_DIR}")
  endif()
  if(NOT GGML_INCLUDE_DIR AND DEFINED ENV{GGML_INCLUDE_DIR})
    set(GGML_INCLUDE_DIR "$ENV{GGML_INCLUDE_DIR}")
  endif()
  if(NOT LLAMA_CPP_INCLUDE_DIR)
    message(FATAL_ERROR "AGENT_NATIVE_ENABLE_LLAMA_CPP=ON requires LLAMA_CPP_INCLUDE_DIR.")
  endif()
  if(NOT GGML_INCLUDE_DIR)
    message(FATAL_ERROR "AGENT_NATIVE_ENABLE_LLAMA_CPP=ON requires GGML_INCLUDE_DIR.")
  endif()
  target_include_directories(agent_model_providers PRIVATE ${LLAMA_CPP_INCLUDE_DIR} ${GGML_INCLUDE_DIR})
  if(UNIX AND NOT APPLE)
    target_link_libraries(agent_model_providers PRIVATE dl)
  endif()
  target_compile_definitions(agent_model_providers PUBLIC AGENT_NATIVE_ENABLE_LLAMA_CPP=1)
else()
  target_sources(agent_model_providers PRIVATE src/agent/llama_cpp_native_binding_stub.cpp)
endif()

foreach(agent_target IN ITEMS agent_core agent_platform agent_model agent_retrieval agent_model_providers agent_tools agent_mcp agent_mcp_native agent_runtime agent_memory agent_knowledge agent_runtime_io agent_runtime_io_native agent_runtime_modules agent_app agent_server)
  if(MSVC)
    target_compile_options(${agent_target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(${agent_target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  set_target_properties(${agent_target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
endforeach()

add_library(agent_capi STATIC
  src/agent_capi.cpp
)

target_link_libraries(agent_capi PUBLIC agent_runtime)
target_include_directories(agent_capi
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(agent_capi PRIVATE AGENT_NATIVE_VERSION="${PROJECT_VERSION}")
set_target_properties(agent_capi PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(agent_capi_shared SHARED
  src/agent_capi.cpp
)
target_link_libraries(agent_capi_shared PUBLIC agent_runtime)
target_include_directories(agent_capi_shared
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(agent_capi_shared PRIVATE AGENT_NATIVE_VERSION="${PROJECT_VERSION}")
set_target_properties(agent_capi_shared PROPERTIES
  OUTPUT_NAME agent_capi_shared
  POSITION_INDEPENDENT_CODE ON
)
if(WIN32)
  set_target_properties(agent_capi_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

add_library(agent_capi_full STATIC
  src/agent_capi.cpp
)
target_link_libraries(agent_capi_full PUBLIC agent_app)
target_include_directories(agent_capi_full
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(agent_capi_full PRIVATE
  AGENT_NATIVE_VERSION="${PROJECT_VERSION}"
  AGENT_CAPI_ENABLE_FULL=1
)
set_target_properties(agent_capi_full PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(agent_capi_full_shared SHARED
  src/agent_capi.cpp
)
target_link_libraries(agent_capi_full_shared PUBLIC agent_app)
target_include_directories(agent_capi_full_shared
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_definitions(agent_capi_full_shared PRIVATE
  AGENT_NATIVE_VERSION="${PROJECT_VERSION}"
  AGENT_CAPI_ENABLE_FULL=1
)
set_target_properties(agent_capi_full_shared PROPERTIES
  OUTPUT_NAME agent_capi_full_shared
  POSITION_INDEPENDENT_CODE ON
)
if(WIN32)
  set_target_properties(agent_capi_full_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

set(AGENT_NATIVE_EXAMPLES
  runner_basic
  tools_and_permissions
  rag_basic
  workflow
  evals_basic
)

if(AGENT_NATIVE_BUILD_CLI)
  add_executable(native_agent_cli
    src/native_agent_cli.cpp
  )
  target_link_libraries(native_agent_cli PRIVATE agent_app)
endif()

if(AGENT_NATIVE_BUILD_EXAMPLES)
  foreach(example IN LISTS AGENT_NATIVE_EXAMPLES)
    string(REPLACE "_" "-" example_source "${example}")
    add_executable(example_${example} examples/${example_source}.cpp)
    if(example STREQUAL "evals_basic")
      target_link_libraries(example_${example} PRIVATE agent_app)
    elseif(example STREQUAL "workflow")
      target_link_libraries(example_${example} PRIVATE agent_runtime_modules)
    elseif(example STREQUAL "rag_basic")
      target_link_libraries(example_${example} PRIVATE agent_full)
    else()
      target_link_libraries(example_${example} PRIVATE agent_native)
    endif()
  endforeach()

  add_executable(example_capi_c examples/capi-bindings/c-minimal.c)
  set_target_properties(example_capi_c PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    LINKER_LANGUAGE CXX
  )
  target_link_libraries(example_capi_c PRIVATE agent_capi)
endif()

if(BUILD_TESTING)
  add_executable(agent_native_embedded_smoke
    tests/native_embedded_smoke.cpp
  )
  target_link_libraries(agent_native_embedded_smoke PRIVATE agent_native)

  add_executable(agent_native_smoke
    tests/smoke.cpp
  )
  target_link_libraries(agent_native_smoke PRIVATE agent_full)

  # C-language smoke test for the agent_capi shim. Compiling as C99 proves the
  # header is genuinely C-compatible (no C++ leaks into the FFI surface).
  add_executable(agent_capi_smoke
    tests/capi_smoke.c
  )
  set_target_properties(agent_capi_smoke PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF
    LINKER_LANGUAGE CXX
  )
  target_link_libraries(agent_capi_smoke PRIVATE agent_capi_full)

  add_executable(agent_contract_tests
    tests/contracts.cpp
  )
  target_link_libraries(agent_contract_tests PRIVATE agent_capi_full agent_full)

  add_executable(agent_plan_execute_tests
    tests/plan_execute.cpp
  )
  target_link_libraries(agent_plan_execute_tests PRIVATE agent_runtime_modules)

  add_test(NAME agent_native_embedded_smoke COMMAND agent_native_embedded_smoke)
  add_test(NAME agent_native_smoke COMMAND agent_native_smoke)
  add_test(NAME agent_capi_smoke COMMAND agent_capi_smoke)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    set(AGENT_PYTHON_TEST_COMMAND ${Python3_EXECUTABLE})
    if(APPLE AND EXISTS "/usr/bin/arch" AND EXISTS "/usr/bin/python3")
      if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(AGENT_PYTHON_TEST_COMMAND /usr/bin/arch -x86_64 /usr/bin/python3)
      elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64")
        set(AGENT_PYTHON_TEST_COMMAND /usr/bin/arch -arm64 /usr/bin/python3)
      endif()
    endif()
    add_test(NAME agent_python_ctypes_smoke
      COMMAND ${CMAKE_COMMAND} -E env
        PYTHONUTF8=1
        AGENT_CAPI_LIBRARY=$<TARGET_FILE:agent_capi_full_shared>
        ${AGENT_PYTHON_TEST_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/tests/capi_ctypes_smoke.py
    )
    add_test(NAME agent_capi_surface_manifest
      COMMAND ${CMAKE_COMMAND} -E env
        PYTHONUTF8=1
        AGENT_CAPI_LIBRARY=$<TARGET_FILE:agent_capi_shared>
        AGENT_CAPI_SYMBOL_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/contracts/public-surface/capi-runtime-symbols.txt
        ${AGENT_PYTHON_TEST_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_capi_symbols.py
    )
    add_test(NAME agent_capi_full_surface_manifest
      COMMAND ${CMAKE_COMMAND} -E env
        PYTHONUTF8=1
        AGENT_CAPI_LIBRARY=$<TARGET_FILE:agent_capi_full_shared>
        AGENT_CAPI_SYMBOL_MANIFEST=${CMAKE_CURRENT_SOURCE_DIR}/contracts/public-surface/capi-symbols.txt
        ${AGENT_PYTHON_TEST_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_capi_symbols.py
    )
    add_test(NAME agent_doc_example_closure
      COMMAND ${AGENT_PYTHON_TEST_COMMAND} ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_doc_example_closure.py
    )
  endif()
  add_test(NAME agent_contract_tests
    COMMAND agent_contract_tests ${CMAKE_CURRENT_SOURCE_DIR}/contracts/observable
  )
  add_test(NAME agent_plan_execute_tests COMMAND agent_plan_execute_tests)
  add_test(NAME agent_artifact_policy
    COMMAND ${CMAKE_COMMAND}
      -DAGENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-generated-artifacts.cmake
  )
  add_test(NAME agent_platform_governance_policy
    COMMAND ${CMAKE_COMMAND}
      -DAGENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-platform-governance.cmake
  )
  add_test(NAME agent_native_boundary_policy
    COMMAND ${CMAKE_COMMAND}
      -DAGENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-native-boundaries.cmake
  )
  if(AGENT_NATIVE_BUILD_CLI)
    add_test(NAME native_agent_cli_help COMMAND native_agent_cli --help)
  endif()
  if(AGENT_NATIVE_BUILD_EXAMPLES)
    foreach(example IN LISTS AGENT_NATIVE_EXAMPLES)
      add_test(NAME example_${example} COMMAND example_${example})
    endforeach()
  endif()
endif()
