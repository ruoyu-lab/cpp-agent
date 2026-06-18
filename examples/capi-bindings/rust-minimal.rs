use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int};
use std::ptr;

#[repr(C)]
struct AgentRunner {
    _private: [u8; 0],
}

#[link(name = "agent_capi_shared")]
extern "C" {
    fn agent_last_error() -> *const c_char;
    fn agent_string_free(value: *mut c_char);
    fn agent_capi_negotiate_abi_version(min_version: c_int, max_version: c_int, out_version: *mut c_int) -> c_int;
    fn agent_runner_create_with_echo_model(out_runner: *mut *mut AgentRunner) -> c_int;
    fn agent_runner_run(
        runner: *mut AgentRunner,
        input: *const c_char,
        session_id: *const c_char,
        out_result_json: *mut *mut c_char,
    ) -> c_int;
    fn agent_runner_release(runner: *mut AgentRunner);
}

unsafe fn last_error() -> String {
    let ptr = agent_last_error();
    if ptr.is_null() {
        String::new()
    } else {
        CStr::from_ptr(ptr).to_string_lossy().into_owned()
    }
}

unsafe fn require_ok(status: c_int, context: &str) {
    if status != 0 {
        panic!("{}: {}", context, last_error());
    }
}

fn main() {
    unsafe {
        let mut version: c_int = 0;
        require_ok(agent_capi_negotiate_abi_version(3, 3, &mut version), "negotiate");

        let mut runner: *mut AgentRunner = ptr::null_mut();
        require_ok(agent_runner_create_with_echo_model(&mut runner), "create");

        let input = CString::new("hello from Rust").unwrap();
        let session = CString::new("rust-example").unwrap();
        let mut result: *mut c_char = ptr::null_mut();
        require_ok(agent_runner_run(runner, input.as_ptr(), session.as_ptr(), &mut result), "run");

        println!("{}", CStr::from_ptr(result).to_string_lossy());
        agent_string_free(result);
        agent_runner_release(runner);
    }
}
