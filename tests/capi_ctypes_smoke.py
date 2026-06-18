#!/usr/bin/env python3

import ctypes
import json
import os
import pathlib
import tempfile


LIB_PATH = os.environ.get("AGENT_CAPI_LIBRARY")
if not LIB_PATH:
    raise SystemExit("AGENT_CAPI_LIBRARY is required")


def load_library(path: str) -> ctypes.CDLL:
    library_dir = str(pathlib.Path(path).resolve().parent)
    if os.name == "nt" and hasattr(os, "add_dll_directory"):
        os.add_dll_directory(library_dir)
    return ctypes.CDLL(path)


lib = load_library(LIB_PATH)

RunnerHandle = ctypes.c_void_p
StreamHandle = ctypes.c_void_p
ErrorHandle = ctypes.c_void_p
CancellationHandle = ctypes.c_void_p
RunHandle = ctypes.c_void_p
HostRuntimeHandle = ctypes.c_void_p
StringOut = ctypes.c_void_p
StreamCallback = ctypes.CFUNCTYPE(ctypes.c_int32, ctypes.c_char_p, ctypes.c_void_p)
HostModelCallback = ctypes.CFUNCTYPE(
    ctypes.c_int32,
    ctypes.c_char_p,
    ctypes.POINTER(StringOut),
    ctypes.c_void_p,
)


class HostVTable(ctypes.Structure):
    _fields_ = [
        ("size", ctypes.c_size_t),
        ("user_data", ctypes.c_void_p),
        ("model_generate_json", HostModelCallback),
        ("cancelled", ctypes.c_void_p),
    ]

lib.agent_last_error.restype = ctypes.c_char_p
lib.agent_version.restype = ctypes.c_char_p
lib.agent_capi_abi_version.restype = ctypes.c_int32
lib.agent_capi_contract_json.restype = ctypes.c_char_p
lib.agent_string_free.argtypes = [ctypes.c_void_p]
lib.agent_string_clone.argtypes = [ctypes.c_char_p]
lib.agent_string_clone.restype = ctypes.c_void_p
lib.agent_capi_negotiate_abi_version.argtypes = [
    ctypes.c_int32,
    ctypes.c_int32,
    ctypes.POINTER(ctypes.c_int32),
]
lib.agent_capi_negotiate_abi_version.restype = ctypes.c_int32
lib.agent_capi_version_info_json.argtypes = [ctypes.POINTER(StringOut)]
lib.agent_capi_version_info_json.restype = ctypes.c_int32
lib.agent_last_error_object.argtypes = [ctypes.POINTER(ErrorHandle)]
lib.agent_last_error_object.restype = ctypes.c_int32
lib.agent_error_release.argtypes = [ErrorHandle]
lib.agent_error_code.argtypes = [ErrorHandle]
lib.agent_error_code.restype = ctypes.c_int32
lib.agent_error_type.argtypes = [ErrorHandle]
lib.agent_error_type.restype = ctypes.c_char_p
lib.agent_error_message.argtypes = [ErrorHandle]
lib.agent_error_message.restype = ctypes.c_char_p

lib.agent_runner_create_with_echo_model.argtypes = [ctypes.POINTER(RunnerHandle)]
lib.agent_runner_create_with_echo_model.restype = ctypes.c_int32
lib.agent_runner_create_from_config_json.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(RunnerHandle)]
lib.agent_runner_create_from_config_json.restype = ctypes.c_int32
lib.agent_runner_create_from_config_path.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(RunnerHandle)]
lib.agent_runner_create_from_config_path.restype = ctypes.c_int32
lib.agent_runner_release.argtypes = [RunnerHandle]

lib.agent_runner_run.argtypes = [RunnerHandle, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(StringOut)]
lib.agent_runner_run.restype = ctypes.c_int32
lib.agent_runner_run_json.argtypes = [RunnerHandle, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(StringOut)]
lib.agent_runner_run_json.restype = ctypes.c_int32
lib.agent_runner_run_json_async.argtypes = [
    RunnerHandle,
    ctypes.c_char_p,
    ctypes.c_char_p,
    CancellationHandle,
    ctypes.POINTER(RunHandle),
]
lib.agent_runner_run_json_async.restype = ctypes.c_int32
lib.agent_run_wait_json.argtypes = [RunHandle, ctypes.POINTER(StringOut)]
lib.agent_run_wait_json.restype = ctypes.c_int32
lib.agent_run_release.argtypes = [RunHandle]
lib.agent_run_release.restype = None
lib.agent_host_runtime_create.argtypes = [ctypes.POINTER(HostVTable), ctypes.POINTER(HostRuntimeHandle)]
lib.agent_host_runtime_create.restype = ctypes.c_int32
lib.agent_host_runtime_release.argtypes = [HostRuntimeHandle]
lib.agent_host_runtime_release.restype = None
lib.agent_host_runtime_describe_json.argtypes = [HostRuntimeHandle, ctypes.POINTER(StringOut)]
lib.agent_host_runtime_describe_json.restype = ctypes.c_int32
lib.agent_runner_create_with_host_model.argtypes = [
    HostRuntimeHandle,
    ctypes.c_char_p,
    ctypes.POINTER(RunnerHandle),
]
lib.agent_runner_create_with_host_model.restype = ctypes.c_int32
lib.agent_runner_stream_json.argtypes = [RunnerHandle, ctypes.c_char_p, ctypes.c_char_p, StreamCallback, ctypes.c_void_p]
lib.agent_runner_stream_json.restype = ctypes.c_int32
lib.agent_runner_stream_events_json.argtypes = [
    RunnerHandle,
    ctypes.c_char_p,
    ctypes.c_char_p,
    ctypes.c_size_t,
    ctypes.POINTER(StreamHandle),
]
lib.agent_runner_stream_events_json.restype = ctypes.c_int32
lib.agent_runner_event_stream_next_json.argtypes = [
    StreamHandle,
    ctypes.POINTER(StringOut),
    ctypes.POINTER(ctypes.c_int32),
]
lib.agent_runner_event_stream_next_json.restype = ctypes.c_int32
lib.agent_runner_event_stream_cancel.argtypes = [StreamHandle, ctypes.c_char_p]
lib.agent_runner_event_stream_cancel.restype = None
lib.agent_runner_event_stream_close.argtypes = [StreamHandle]
lib.agent_runner_event_stream_close.restype = None
lib.agent_runner_event_stream_release.argtypes = [StreamHandle]
lib.agent_runner_event_stream_release.restype = None


def last_error() -> str:
    value = lib.agent_last_error()
    return value.decode("utf-8") if value else ""


def require_ok(status: int, context: str) -> None:
    if status != 0:
        raise AssertionError(f"{context}: {last_error()}")


def decode_owned_json(pointer: ctypes.c_void_p) -> object:
    if not pointer:
        raise AssertionError("expected non-null owned string pointer")
    try:
        raw = ctypes.cast(pointer, ctypes.c_char_p).value
        if raw is None:
            raise AssertionError("owned string pointer had null value")
        return json.loads(raw.decode("utf-8"))
    finally:
        lib.agent_string_free(pointer)


def main() -> None:
    assert lib.agent_capi_abi_version() == 4
    negotiated = ctypes.c_int32()
    require_ok(
        lib.agent_capi_negotiate_abi_version(4, 4, ctypes.byref(negotiated)),
        "agent_capi_negotiate_abi_version",
    )
    assert negotiated.value == 4
    assert lib.agent_capi_negotiate_abi_version(1, 3, ctypes.byref(negotiated)) != 0
    error = ErrorHandle()
    require_ok(lib.agent_last_error_object(ctypes.byref(error)), "agent_last_error_object")
    try:
        assert lib.agent_error_code(error) != 0
        assert lib.agent_error_type(error)
        assert lib.agent_error_message(error)
    finally:
        lib.agent_error_release(error)

    version = lib.agent_version()
    assert version is not None and version.decode("utf-8").startswith("agent_native ")
    version_info_ptr = StringOut()
    require_ok(lib.agent_capi_version_info_json(ctypes.byref(version_info_ptr)), "agent_capi_version_info_json")
    version_info = decode_owned_json(version_info_ptr)
    assert version_info["abiVersion"] == 4
    assert "host-model-vtable" in version_info["features"]

    contract = lib.agent_capi_contract_json()
    assert contract is not None
    contract_json = json.loads(contract.decode("utf-8"))
    assert contract_json["abiVersion"] == 4
    assert contract_json["constructors"]["configPath"] == "agent_runner_create_from_config_path"
    assert contract_json["constructors"]["hostModel"] == "agent_runner_create_with_host_model"
    assert contract_json["errorObject"]["last"] == "agent_last_error_object"
    assert contract_json["asyncRunHandle"]["startText"] == "agent_runner_run_async"
    assert contract_json["asyncIterator"]["next"] == "agent_runner_event_stream_next_json"
    assert contract_json["asyncIterator"]["cancel"] == "agent_runner_event_stream_cancel"

    runner = RunnerHandle()
    config_json = json.dumps({
        "agents": {
            "ffi": {
                "model": {"provider": "echo", "model": "echo"}
            }
        }
    }).encode("utf-8")
    require_ok(
        lib.agent_runner_create_from_config_json(config_json, b"ffi", ctypes.byref(runner)),
        "agent_runner_create_from_config_json",
    )

    result_ptr = StringOut()
    require_ok(
        lib.agent_runner_run_json(runner, b"\"hello from ctypes\"", b"ctypes-json-session", ctypes.byref(result_ptr)),
        "agent_runner_run_json",
    )
    result = decode_owned_json(result_ptr)
    assert result["sessionId"] == "ctypes-json-session"
    assert "hello from ctypes" in result["text"]
    assert isinstance(result["usage"], dict)

    async_run = RunHandle()
    require_ok(
        lib.agent_runner_run_json_async(
            runner,
            b"\"ctypes async handle\"",
            b"ctypes-async-handle-session",
            None,
            ctypes.byref(async_run),
        ),
        "agent_runner_run_json_async",
    )
    async_result_ptr = StringOut()
    try:
        require_ok(lib.agent_run_wait_json(async_run, ctypes.byref(async_result_ptr)), "agent_run_wait_json")
        async_result = decode_owned_json(async_result_ptr)
        assert async_result["sessionId"] == "ctypes-async-handle-session"
        assert "ctypes async handle" in async_result["text"]
        assert isinstance(async_result["usage"], dict)
    finally:
        lib.agent_run_release(async_run)

    @HostModelCallback
    def host_model(request_json, out_response_json, _user_data):
        request = json.loads(request_json.decode("utf-8"))
        serialized = json.dumps({
            "text": "Thought: answer through Python host.\nFinal Answer: ctypes host ok",
            "usage": {
                "inputTokens": 4,
                "outputTokens": 5,
                "totalTokens": 9,
                "inputTokensSource": "provider",
                "outputTokensSource": "provider",
                "totalTokensSource": "provider",
                "quality": "provider",
            },
            "metadata": {"sawMessages": len(request["messages"])},
        }).encode("utf-8")
        out_response_json[0] = lib.agent_string_clone(serialized)
        return 0 if out_response_json[0] else 2

    host_vtable = HostVTable()
    host_vtable.size = ctypes.sizeof(HostVTable)
    host_vtable.user_data = None
    host_vtable.model_generate_json = host_model
    host_runtime = HostRuntimeHandle()
    require_ok(
        lib.agent_host_runtime_create(ctypes.byref(host_vtable), ctypes.byref(host_runtime)),
        "agent_host_runtime_create",
    )
    host_description_ptr = StringOut()
    require_ok(
        lib.agent_host_runtime_describe_json(host_runtime, ctypes.byref(host_description_ptr)),
        "agent_host_runtime_describe_json",
    )
    host_description = decode_owned_json(host_description_ptr)
    assert host_description["model"] is True

    host_runner = RunnerHandle()
    require_ok(
        lib.agent_runner_create_with_host_model(
            host_runtime,
            json.dumps({
                "provider": "ctypes-host",
                "model": "ctypes-host-model",
                "capabilities": ["input.text"],
            }).encode("utf-8"),
            ctypes.byref(host_runner),
        ),
        "agent_runner_create_with_host_model",
    )
    lib.agent_host_runtime_release(host_runtime)
    host_result_ptr = StringOut()
    try:
        require_ok(
            lib.agent_runner_run(host_runner, b"ctypes host hello", b"ctypes-host-session", ctypes.byref(host_result_ptr)),
            "agent_runner_run host",
        )
        host_result = decode_owned_json(host_result_ptr)
        assert host_result["sessionId"] == "ctypes-host-session"
        assert "ctypes host ok" in host_result["text"]
        assert isinstance(host_result["usage"], dict)
    finally:
        lib.agent_runner_release(host_runner)

    events = []

    @StreamCallback
    def on_event(event_json, _user_data):
        events.append(json.loads(event_json.decode("utf-8")))
        return 0

    require_ok(
        lib.agent_runner_stream_json(
            runner,
            b"[{\"type\":\"text\",\"text\":\"stream from ctypes\"}]",
            b"ctypes-stream-session",
            on_event,
            None,
        ),
        "agent_runner_stream_json",
    )
    assert events
    assert events[-1]["type"] == "done"
    assert events[-1]["result"]["sessionId"] == "ctypes-stream-session"

    pull_stream = StreamHandle()
    require_ok(
        lib.agent_runner_stream_events_json(
            runner,
            b"[{\"type\":\"text\",\"text\":\"pull stream from ctypes\"}]",
            b"ctypes-pull-stream-session",
            1,
            ctypes.byref(pull_stream),
        ),
        "agent_runner_stream_events_json",
    )
    pull_events = []
    try:
        while True:
            event_ptr = StringOut()
            has_event = ctypes.c_int32()
            require_ok(
                lib.agent_runner_event_stream_next_json(
                    pull_stream,
                    ctypes.byref(event_ptr),
                    ctypes.byref(has_event),
                ),
                "agent_runner_event_stream_next_json",
            )
            if not has_event.value:
                break
            pull_events.append(decode_owned_json(event_ptr))
    finally:
        lib.agent_runner_event_stream_release(pull_stream)
    assert pull_events
    assert pull_events[0]["schemaVersion"] == 1
    assert pull_events[0]["sequence"] == 1
    assert pull_events[-1]["type"] == "done"
    assert pull_events[-1]["result"]["sessionId"] == "ctypes-pull-stream-session"

    cancel_stream = StreamHandle()
    require_ok(
        lib.agent_runner_stream_events_json(
            runner,
            json.dumps("ctypes cancel pull stream").encode("utf-8"),
            b"ctypes-pull-cancel",
            1,
            ctypes.byref(cancel_stream),
        ),
        "agent_runner_stream_events_json cancel",
    )
    lib.agent_runner_event_stream_cancel(cancel_stream, b"ctypes cancelled")
    lib.agent_runner_event_stream_release(cancel_stream)

    lib.agent_runner_release(runner)

    with tempfile.TemporaryDirectory(prefix="agent-capi-ctypes-") as tmpdir:
        config_path = pathlib.Path(tmpdir) / "agent.json"
        config_path.write_text(json.dumps({
            "agents": {
                "ffi-path": {
                    "model": {"provider": "echo", "model": "echo"}
                }
            }
        }), encoding="utf-8")

        path_runner = RunnerHandle()
        require_ok(
            lib.agent_runner_create_from_config_path(
                str(config_path).encode("utf-8"),
                b"ffi-path",
                ctypes.byref(path_runner),
            ),
            "agent_runner_create_from_config_path",
        )

        path_result_ptr = StringOut()
        require_ok(
            lib.agent_runner_run(
                path_runner,
                b"path hello",
                b"ctypes-path-session",
                ctypes.byref(path_result_ptr),
            ),
            "agent_runner_run",
        )
        path_result = decode_owned_json(path_result_ptr)
        assert path_result["sessionId"] == "ctypes-path-session"
        assert "path hello" in path_result["text"]
        lib.agent_runner_release(path_runner)

    invalid_runner = RunnerHandle()
    status = lib.agent_runner_create_from_config_path(b"/definitely/missing/agent.json", None, ctypes.byref(invalid_runner))
    assert status != 0
    assert last_error()


if __name__ == "__main__":
    main()
