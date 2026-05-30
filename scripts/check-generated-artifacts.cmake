if(NOT DEFINED AGENT_SOURCE_DIR)
  message(FATAL_ERROR "AGENT_SOURCE_DIR is required.")
endif()

execute_process(
  COMMAND git ls-files
    "build/**"
    "build-release/**"
    "build-debug/**"
    "build-*/**"
    "compile_commands.json"
    "*.a"
    "*.o"
    "*.so"
    "*.dylib"
    "*.dll"
    "*.exe"
  WORKING_DIRECTORY "${AGENT_SOURCE_DIR}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE tracked_artifacts
  ERROR_VARIABLE git_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "git ls-files failed while checking generated artifacts: ${git_error}")
endif()

if(NOT "${tracked_artifacts}" STREQUAL "")
  message(FATAL_ERROR "Generated artifacts are tracked in git:\n${tracked_artifacts}")
endif()

message(STATUS "generated artifact policy OK")
