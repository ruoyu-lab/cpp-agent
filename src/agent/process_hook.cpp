#include "agent/process_hook.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace agent {

#ifndef _WIN32

namespace {

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(other.release());
    }
    return *this;
  }
  ~UniqueFd() {
    reset();
  }

  [[nodiscard]] int get() const noexcept {
    return fd_;
  }

  [[nodiscard]] int release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
  }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      (void)::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

std::pair<UniqueFd, UniqueFd> make_pipe() {
  int fds[2] = {-1, -1};
  if (::pipe(fds) != 0) {
    throw ConfigurationError(std::string("Failed to create pipe: ") + std::strerror(errno));
  }
  return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

class SigpipeGuard {
 public:
  SigpipeGuard() {
    struct sigaction action {};
    action.sa_handler = SIG_IGN;
    sigemptyset(&action.sa_mask);
    changed_ = ::sigaction(SIGPIPE, &action, &previous_) == 0;
  }
  SigpipeGuard(const SigpipeGuard&) = delete;
  SigpipeGuard& operator=(const SigpipeGuard&) = delete;
  ~SigpipeGuard() {
    if (changed_) {
      (void)::sigaction(SIGPIPE, &previous_, nullptr);
    }
  }

 private:
  struct sigaction previous_ {};
  bool changed_ = false;
};

void write_all(int fd, const std::string& data) {
  SigpipeGuard sigpipe_guard;
  std::size_t written = 0;
  while (written < data.size()) {
    const auto n = ::write(fd, data.data() + written, data.size() - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    written += static_cast<std::size_t>(n);
  }
}

std::string read_all_until_deadline(int fd, std::chrono::steady_clock::time_point deadline) {
  std::string out;
  char buf[1024];
  ::fcntl(fd, F_SETFL, O_NONBLOCK);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) break;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    timeval tv;
    tv.tv_sec = remaining.count() / 1000;
    tv.tv_usec = static_cast<int>((remaining.count() % 1000) * 1000);
    const int ready = ::select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (ready <= 0) break;
    const auto n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  return out;
}

}  // namespace

ProcessHookResult run_process_hook(const ProcessHookConfig& config, const Value& payload) {
  ProcessHookResult result;
  if (config.executable.empty()) {
    throw ConfigurationError("ProcessHookConfig.executable is required.");
  }
  auto stdin_pipe = make_pipe();
  auto stdout_pipe = make_pipe();
  auto stderr_pipe = make_pipe();
  const auto deadline = std::chrono::steady_clock::now() + config.timeout;
  const pid_t pid = ::fork();
  if (pid < 0) {
    throw ConfigurationError(std::string("fork failed: ") + std::strerror(errno));
  }
  if (pid == 0) {
    // Child.
    if (::dup2(stdin_pipe.first.get(), 0) < 0 ||
        ::dup2(stdout_pipe.second.get(), 1) < 0 ||
        ::dup2(stderr_pipe.second.get(), 2) < 0) {
      std::_Exit(127);
    }
    stdin_pipe.first.reset();
    stdin_pipe.second.reset();
    stdout_pipe.first.reset();
    stdout_pipe.second.reset();
    stderr_pipe.first.reset();
    stderr_pipe.second.reset();

    std::vector<std::string> argv_storage;
    argv_storage.reserve(config.args.size() + 1);
    argv_storage.push_back(config.executable.string());
    for (const auto& a : config.args) argv_storage.push_back(a);
    std::vector<char*> argv;
    argv.reserve(argv_storage.size() + 1);
    for (auto& s : argv_storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    if (!config.env.empty()) {
      // Build a fresh environ. Each entry is "KEY=VAL".
      std::vector<std::string> env_storage;
      env_storage.reserve(config.env.size());
      for (const auto& [k, v] : config.env) env_storage.push_back(k + "=" + v);
      std::vector<char*> envp;
      envp.reserve(env_storage.size() + 1);
      for (auto& s : env_storage) envp.push_back(s.data());
      envp.push_back(nullptr);
      ::execve(config.executable.c_str(), argv.data(), envp.data());
    } else {
      ::execv(config.executable.c_str(), argv.data());
    }
    // execv failed.
    std::_Exit(127);
  }

  // Parent.
  stdin_pipe.first.reset();
  stdout_pipe.second.reset();
  stderr_pipe.second.reset();

  const std::string payload_text = payload.stringify(0);
  write_all(stdin_pipe.second.get(), payload_text);
  stdin_pipe.second.reset();

  std::string stdout_text;
  std::string stderr_text;
  std::thread stderr_thread([&] {
    stderr_text = read_all_until_deadline(stderr_pipe.first.get(), deadline);
  });
  stdout_text = read_all_until_deadline(stdout_pipe.first.get(), deadline);
  stderr_thread.join();

  stdout_pipe.first.reset();
  stderr_pipe.first.reset();

  int status = 0;
  bool reaped = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    if (r < 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!reaped) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
  }

  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  result.exit_code = exit_code;
  result.stdout_text = std::move(stdout_text);
  result.stderr_text = std::move(stderr_text);
  if (exit_code == 0) {
    result.decision = ProcessHookDecision::Allow;
  } else if (exit_code == 2) {
    result.decision = ProcessHookDecision::Block;
  } else {
    result.decision = ProcessHookDecision::Warn;
  }
  return result;
}

std::function<void(const ToolHookContext&)> make_process_tool_hook(ProcessHookConfig config) {
  return [config = std::move(config)](const ToolHookContext& context) {
    Value payload = Value::object({
        {"toolName", context.tool_name},
        {"toolCall", context.tool_call},
        {"input", context.input},
        {"traceId", context.trace_id},
        {"runId", context.run_id},
    });
    const auto result = run_process_hook(config, payload);
    if (result.decision == ProcessHookDecision::Block) {
      throw ConfigurationError(std::string("process hook blocked tool: ") + result.stderr_text);
    }
    // Warn: no-op other than propagating exit info to the trace via the
    // existing tool hook signature. (Callers can attach EventBus sinks for richer reporting.)
  };
}

#else  // _WIN32

ProcessHookResult run_process_hook(const ProcessHookConfig&, const Value&) {
  throw ConfigurationError("ProcessHook is not supported on Windows.");
}

std::function<void(const ToolHookContext&)> make_process_tool_hook(ProcessHookConfig) {
  return [](const ToolHookContext&) {
    throw ConfigurationError("ProcessHook is not supported on Windows.");
  };
}

#endif

}  // namespace agent
