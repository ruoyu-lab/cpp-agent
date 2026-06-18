# C ABI Binding Examples

Minimal host-language bindings for the AgentCore C ABI v3. These examples use
the built-in echo runner so they do not require provider credentials.

| File | Host |
|---|---|
| [`c-minimal.c`](c-minimal.c) | Plain C99 |
| [`python-minimal.py`](python-minimal.py) | Python `ctypes` |
| [`rust-minimal.rs`](rust-minimal.rs) | Rust `extern "C"` |
| [`csharp-minimal.cs`](csharp-minimal.cs) | C# P/Invoke |

All returned `char*` values must be released with `agent_string_free`. Opaque
handles must be released with their matching `agent_*_release` function.
