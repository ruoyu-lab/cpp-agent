#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace agent {

inline constexpr int kAgentStreamEventSchemaVersion = 1;
inline constexpr std::size_t kDefaultStreamQueueCapacity = 64;

struct StreamQueueOptions {
  std::size_t capacity = kDefaultStreamQueueCapacity;
};

class StreamQueueClosed : public std::runtime_error {
 public:
  StreamQueueClosed() : std::runtime_error("Stream queue is closed.") {}
};

template <typename Event>
class BoundedStreamQueue {
 public:
  explicit BoundedStreamQueue(StreamQueueOptions options = {})
      : state_(std::make_shared<State>()) {
    state_->capacity = options.capacity == 0 ? 1 : options.capacity;
  }

  void push(Event event) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->not_full.wait(lock, [&] {
      return state_->closed || state_->failed || state_->events.size() < state_->capacity;
    });
    if (state_->failed) {
      std::rethrow_exception(state_->failed);
    }
    if (state_->closed) {
      throw StreamQueueClosed();
    }
    state_->events.push_back(std::move(event));
    lock.unlock();
    state_->not_empty.notify_one();
  }

  bool next(Event& event) {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->not_empty.wait(lock, [&] {
      return state_->failed || state_->closed || !state_->events.empty();
    });
    if (!state_->events.empty()) {
      event = std::move(state_->events.front());
      state_->events.pop_front();
      lock.unlock();
      state_->not_full.notify_one();
      return true;
    }
    if (state_->failed) {
      std::rethrow_exception(state_->failed);
    }
    return false;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->closed = true;
    }
    state_->not_empty.notify_all();
    state_->not_full.notify_all();
  }

  void fail(std::exception_ptr error) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->failed = error ? std::move(error) : std::make_exception_ptr(StreamQueueClosed());
    }
    state_->not_empty.notify_all();
    state_->not_full.notify_all();
  }

  [[nodiscard]] bool closed() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closed;
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    std::deque<Event> events;
    std::size_t capacity = kDefaultStreamQueueCapacity;
    bool closed = false;
    std::exception_ptr failed;
  };

  std::shared_ptr<State> state_;
};

}  // namespace agent
