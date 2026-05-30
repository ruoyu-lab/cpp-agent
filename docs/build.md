# Build

`agent_native` builds with CMake (≥ 3.20) and any C++20 compiler. The default
build links only the C++ standard library.

## Targets

| Target | Type | Purpose |
|---|---|---|
| `agent_native` | static library | The framework. Public include root is `include/`. |
| `native_agent_cli` | executable | The reference CLI binary. |
| `agent_native_smoke` | executable | Smoke test exercised by CTest and direct repeated runs. |

## Standard Build

```bash
cd C++
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

This produces `build/libagent_native.a` (or `agent_native.lib` on MSVC) and
the `native_agent_cli` binary in `build/`.

The CMake target sets `-Wall -Wextra -Wpedantic` for GCC/Clang and `/W4
/permissive-` for MSVC. Treat new warnings as porting regressions.

## Smoke Test Loop

The porting audit standard is to run the smoke binary repeatedly:

```bash
for i in $(seq 1 10); do ./build/agent_native_smoke || break; done
```

The smoke binary exercises registries, runtime, serialization, and store paths.
Consecutive failures indicate concurrency or state-leak regressions.

## Optional llama.cpp Native Binding

The framework ships with a stub llama.cpp adapter so the default build stays
zero-dependency. To enable the built-in dynamic-loader binding:

```bash
cmake -S . -B build-llama \
  -DAGENT_NATIVE_ENABLE_LLAMA_CPP=ON \
  -DLLAMA_CPP_INCLUDE_DIR=/path/to/llama.cpp/include \
  -DGGML_INCLUDE_DIR=/path/to/llama.cpp/ggml/include
cmake --build build-llama
```

When `AGENT_NATIVE_ENABLE_LLAMA_CPP=ON`:

- `src/agent/llama_cpp_native_binding.cpp` replaces the stub.
- The macro `AGENT_NATIVE_ENABLE_LLAMA_CPP=1` is propagated to consumers.
- On Linux the framework links `dl` for `dlopen`-based runtime loading.

Runtime configuration still requires the host to provide:

- `modelPath` — GGUF model file.
- `libraryPath` or `libraryDir` — location of `libllama` / `libggml`.
- `mmprojPath` plus `mtmdLibraryPath`/`mtmdLibraryDir` if image input is used
  (requires a llama.cpp build that ships `libmtmd`).

The relevant environment-variable names (`LLAMA_CPP_INCLUDE_DIR`,
`GGML_INCLUDE_DIR`, `modelPathEnv`, `libraryPathEnv`, `libraryDirEnv`,
`mmprojPathEnv`, `mtmdLibraryPathEnv`, `mtmdLibraryDirEnv`) are collected by
the config validator for env-key auditing — see [Config](config.md).

## Zero-Dependency Verification

Periodically rerun the dependency scan that the porting audits use to confirm
the build still has no transitive third-party links. The expectation is that
`libagent_native.a` references only standard library symbols (plus `dl` and
`pthread` on Linux when relevant, and `mtmd`/`llama`/`ggml` symbols loaded
dynamically only when llama.cpp is enabled).

A quick local check:

```bash
nm -gC build/libagent_native.a | grep -E ' U ' | sort -u
```

Anything beyond the standard library, libc, libdl, libpthread, and (when
llama.cpp is enabled) llama/ggml/mtmd entry points indicates a new transitive
dependency. Investigate before merging.

## Embedding `agent_native` Into Another Build

```cmake
add_subdirectory(third_party/agent_native/C++)
target_link_libraries(your_target PRIVATE agent_native)
target_include_directories(your_target PRIVATE third_party/agent_native/C++/include)
```

There is no install step required. The CMake target exports its include
directory publicly.

## Building the CLI Standalone

The CLI binary is the simplest way to validate a build end-to-end:

```bash
./build/native_agent_cli --help
./build/native_agent_cli --version
```

CLI command reference is in [CLI](cli.md).

## Platform Notes

- **Linux**: GCC 11+ or Clang 14+. `dl` is linked when llama.cpp is enabled.
- **macOS**: Apple Clang 14+ or Homebrew Clang 15+. `dlopen` is provided by
  the system, no extra link flag needed.
- **Windows / MSVC**: VS 2022 (MSVC 19.34+). The framework builds cleanly on
  Windows. The server module ships as a transport-independent app
  (`AgentServerApp`); HTTP transport is provided by the host on every
  platform, so there is no platform-specific listener to maintain.
