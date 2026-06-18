#!/usr/bin/env python3

import ctypes
import os


lib = ctypes.CDLL(os.environ["AGENT_CAPI_LIBRARY"])

Runner = ctypes.c_void_p
StringOut = ctypes.c_void_p

lib.agent_last_error.restype = ctypes.c_char_p
lib.agent_capi_negotiate_abi_version.argtypes = [
    ctypes.c_int32,
    ctypes.c_int32,
    ctypes.POINTER(ctypes.c_int32),
]
lib.agent_capi_negotiate_abi_version.restype = ctypes.c_int32
lib.agent_runner_create_with_echo_model.argtypes = [ctypes.POINTER(Runner)]
lib.agent_runner_create_with_echo_model.restype = ctypes.c_int32
lib.agent_runner_run.argtypes = [Runner, ctypes.c_char_p, ctypes.c_char_p, ctypes.POINTER(StringOut)]
lib.agent_runner_run.restype = ctypes.c_int32
lib.agent_runner_release.argtypes = [Runner]
lib.agent_string_free.argtypes = [ctypes.c_void_p]


def require_ok(status: int, context: str) -> None:
    if status == 0:
        return
    error = lib.agent_last_error()
    raise RuntimeError(f"{context}: {error.decode('utf-8') if error else ''}")


version = ctypes.c_int32()
require_ok(lib.agent_capi_negotiate_abi_version(3, 3, ctypes.byref(version)), "negotiate")

runner = Runner()
require_ok(lib.agent_runner_create_with_echo_model(ctypes.byref(runner)), "create")

result = StringOut()
try:
    require_ok(lib.agent_runner_run(runner, b"hello from Python", b"python-example", ctypes.byref(result)), "run")
    print(ctypes.cast(result, ctypes.c_char_p).value.decode("utf-8"))
finally:
    if result:
        lib.agent_string_free(result)
    lib.agent_runner_release(runner)
