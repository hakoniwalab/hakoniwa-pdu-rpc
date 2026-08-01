#!/usr/bin/env python3
import os
import sys
from pathlib import Path

from cffi import FFI

ffi = FFI()
ffi.cdef(
    """
    typedef struct hako_pdu_rpc_client_handle hako_pdu_rpc_client_handle_t;
    typedef struct hako_pdu_rpc_server_handle hako_pdu_rpc_server_handle_t;

    typedef enum {
        HAKO_PDU_RPC_OK = 0,
        HAKO_PDU_RPC_ERROR_INVALID_ARGUMENT = 1,
        HAKO_PDU_RPC_ERROR_INITIALIZE = 2,
        HAKO_PDU_RPC_ERROR_START = 3,
        HAKO_PDU_RPC_ERROR_NOT_RUNNING = 4,
        HAKO_PDU_RPC_ERROR_CALL = 5,
        HAKO_PDU_RPC_ERROR_BUFFER_TOO_SMALL = 6,
        HAKO_PDU_RPC_ERROR_NOT_FOUND = 7,
        HAKO_PDU_RPC_ERROR_INTERNAL = 8
    } hako_pdu_rpc_error_t;

    typedef enum {
        HAKO_PDU_RPC_CLIENT_EVENT_NONE = 0,
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_IN = 1,
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_CANCEL = 2,
        HAKO_PDU_RPC_CLIENT_EVENT_RESPONSE_TIMEOUT = 3
    } hako_pdu_rpc_client_event_t;

    typedef enum {
        HAKO_PDU_RPC_SERVER_EVENT_NONE = 0,
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_IN = 1,
        HAKO_PDU_RPC_SERVER_EVENT_REQUEST_CANCEL = 2
    } hako_pdu_rpc_server_event_t;

    typedef struct {
        char service_name[128];
        size_t pdu_size;
    } hako_pdu_rpc_response_info_t;

    typedef struct {
        uint64_t request_token;
        char service_name[128];
        char client_name[128];
        size_t pdu_size;
    } hako_pdu_rpc_request_info_t;

    hako_pdu_rpc_client_handle_t* hako_pdu_rpc_client_create(
        const char*, const char*, const char*, const char*, uint64_t, const char*);
    void hako_pdu_rpc_client_destroy(hako_pdu_rpc_client_handle_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_client_start(hako_pdu_rpc_client_handle_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_client_stop(hako_pdu_rpc_client_handle_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_client_create_request_buffer(
        hako_pdu_rpc_client_handle_t*, const char*, uint8_t*, size_t, size_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_client_call(
        hako_pdu_rpc_client_handle_t*, const char*, const uint8_t*, size_t, uint64_t);
    hako_pdu_rpc_client_event_t hako_pdu_rpc_client_poll(
        hako_pdu_rpc_client_handle_t*, hako_pdu_rpc_response_info_t*, uint8_t*, size_t,
        size_t*, hako_pdu_rpc_error_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_client_cancel(
        hako_pdu_rpc_client_handle_t*, const char*);

    hako_pdu_rpc_server_handle_t* hako_pdu_rpc_server_create(
        const char*, const char*, const char*, uint64_t, const char*);
    void hako_pdu_rpc_server_destroy(hako_pdu_rpc_server_handle_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_server_start(hako_pdu_rpc_server_handle_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_server_stop(hako_pdu_rpc_server_handle_t*);
    hako_pdu_rpc_server_event_t hako_pdu_rpc_server_poll(
        hako_pdu_rpc_server_handle_t*, hako_pdu_rpc_request_info_t*, uint8_t*, size_t,
        size_t*, hako_pdu_rpc_error_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_server_create_reply_buffer(
        hako_pdu_rpc_server_handle_t*, uint64_t, uint8_t, int32_t, uint8_t*, size_t, size_t*);
    hako_pdu_rpc_error_t hako_pdu_rpc_server_send_reply(
        hako_pdu_rpc_server_handle_t*, uint64_t, const uint8_t*, size_t);
    hako_pdu_rpc_error_t hako_pdu_rpc_server_send_cancel_reply(
        hako_pdu_rpc_server_handle_t*, uint64_t, const uint8_t*, size_t);
    """
)


def configure_ffi():
    repo_root = Path(__file__).resolve().parents[2]
    include_dir = repo_root / "include"
    build_dir = repo_root / "build"
    python_build_root = Path(
        os.environ.get("HAKO_PDU_RPC_PYTHON_BUILD_DIR", build_dir / "python")
    ).expanduser().resolve()

    library_dirs = []
    libraries = ["hakoniwa_pdu_rpc"]
    extra_link_args = []
    extra_compile_args = []

    if sys.platform == "win32":
        extra_compile_args.append("/utf-8")

    env_lib_dir = os.environ.get("HAKO_PDU_RPC_LIB_DIR")
    env_shared_lib = os.environ.get("HAKO_PDU_RPC_SHARED_LIB")
    if env_lib_dir:
        library_dirs.append(str(Path(env_lib_dir).expanduser().resolve()))
    elif env_shared_lib:
        shared_lib = Path(env_shared_lib).expanduser().resolve()
        library_dirs.append(str(shared_lib.parent))
        if sys.platform == "win32":
            import_lib = shared_lib.with_suffix(".lib")
            if import_lib.exists():
                extra_link_args.append(str(import_lib))
        else:
            extra_link_args.append(str(shared_lib))
            if sys.platform == "darwin":
                extra_link_args.append("-Wl,-rpath,@loader_path")
            elif sys.platform.startswith("linux"):
                extra_link_args.append("-Wl,-rpath,$ORIGIN")
    else:
        for candidate in (build_dir / "src", build_dir / "src" / "Release"):
            if candidate.exists():
                library_dirs.append(str(candidate.resolve()))

    ffi.set_source(
        "hakoniwa_pdu_rpc._c_rpc_ffi",
        '#include "hakoniwa/pdu/rpc/c_rpc.h"',
        include_dirs=[str(include_dir)],
        libraries=libraries,
        library_dirs=library_dirs,
        extra_compile_args=extra_compile_args,
        extra_link_args=extra_link_args,
    )
    return python_build_root


if __name__ == "__main__":
    target_dir = configure_ffi()
    target_dir.mkdir(parents=True, exist_ok=True)
    ffi.compile(tmpdir=str(target_dir), verbose=True)
