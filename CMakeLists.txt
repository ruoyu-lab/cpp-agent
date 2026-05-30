cmake_minimum_required(VERSION 3.20)

project(native_agent_framework LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd / IDE LSPs know the include paths,
# C++ standard, and per-file flags. Without this, editors flag every
# `agent/*.hpp` include as "file not found" and every `std::string` as
# "type not found" even though the actual cmake build is clean.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(AGENT_NATIVE_ENABLE_LLAMA_CPP "Build the built-in llama.cpp native binding" OFF)
set(LLAMA_CPP_INCLUDE_DIR "" CACHE PATH "Directory containing llama.h")
set(GGML_INCLUDE_DIR "" CACHE PATH "Directory containing ggml-backend.h")

add_library(agent_native
  src/agent/autonomous.cpp
  src/agent/browser.cpp
  src/agent/builtins.cpp
  src/agent/cli.cpp
  src/agent/config.cpp
  src/agent/context.cpp
  src/agent/core.cpp
  src/agent/detail/helpers.cpp
  src/agent/execution.cpp
  src/agent/hooks.cpp
  src/agent/http.cpp
  src/agent/knowledge/strategies.cpp
  src/agent/mcp.cpp
  src/agent/media.cpp
  src/agent/memory.cpp
  src/agent/messages.cpp
  src/agent/model.cpp
  src/agent/observability.cpp
  src/agent/orchestration.cpp
  src/agent/process_hook.cpp
  src/agent/replay_eval.cpp
  src/agent/runtime.cpp
  src/agent/sandbox.cpp
  src/agent/scratch.cpp
  src/agent/server.cpp
  src/agent/server/routing.cpp
  src/agent/server/session_routes.cpp
  src/agent/skills.cpp
  src/agent/tasks.cpp
  src/agent/tool_client.cpp
  src/agent/tools.cpp
  src/agent/web.cpp
  src/agent/workflow.cpp
)

target_include_directories(agent_native
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(AGENT_NATIVE_ENABLE_LLAMA_CPP)
  target_sources(agent_native PRIVATE src/agent/llama_cpp_native_binding.cpp)
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
  target_include_directories(agent_native PRIVATE ${LLAMA_CPP_INCLUDE_DIR} ${GGML_INCLUDE_DIR})
  if(UNIX AND NOT APPLE)
    target_link_libraries(agent_native PRIVATE dl)
  endif()
  target_compile_definitions(agent_native PUBLIC AGENT_NATIVE_ENABLE_LLAMA_CPP=1)
else()
  target_sources(agent_native PRIVATE src/agent/llama_cpp_native_binding_stub.cpp)
endif()

if(MSVC)
  target_compile_options(agent_native PRIVATE /W4 /permissive-)
else()
  target_compile_options(agent_native PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(agent_capi STATIC
  src/agent_capi.cpp
)

target_link_libraries(agent_capi PUBLIC agent_native)
target_include_directories(agent_capi
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(agent_native_smoke
  tests/smoke.cpp
)

target_link_libraries(agent_native_smoke PRIVATE agent_native)

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
target_link_libraries(agent_capi_smoke PRIVATE agent_capi)

add_executable(agent_contract_tests
  tests/contracts.cpp
)

target_link_libraries(agent_contract_tests PRIVATE agent_native)

add_executable(native_agent_cli
  src/native_agent_cli.cpp
)

target_link_libraries(native_agent_cli PRIVATE agent_native)

# Examples — each one builds standalone, runs through CTest, and depends only
# on agent_native (no external providers).
set(AGENT_NATIVE_EXAMPLES
  runner_basic
  tools_and_permissions
  rag_basic
  workflow
  evals_basic
)
foreach(example IN LISTS AGENT_NATIVE_EXAMPLES)
  string(REPLACE "_" "-" example_source "${example}")
  add_executable(example_${example} examples/${example_source}.cpp)
  target_link_libraries(example_${example} PRIVATE agent_native)
endforeach()

enable_testing()
add_test(NAME agent_native_smoke COMMAND agent_native_smoke)
add_test(NAME agent_capi_smoke COMMAND agent_capi_smoke)
add_test(NAME agent_contract_tests
  COMMAND agent_contract_tests ${CMAKE_CURRENT_SOURCE_DIR}/contracts/observable
)
add_test(NAME agent_artifact_policy
  COMMAND ${CMAKE_COMMAND}
    -DAGENT_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/scripts/check-generated-artifacts.cmake
)
add_test(NAME native_agent_cli_help COMMAND native_agent_cli --help)
foreach(example IN LISTS AGENT_NATIVE_EXAMPLES)
  add_test(NAME example_${example} COMMAND example_${example})
endforeach()
