#include "agent/runtime.hpp"
#include "agent/react.hpp"
#include "detail/helpers.hpp"
#include "function_calling_loop.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>

namespace agent {

AgentRunnerEventStream::AgentRunnerEventStream(BoundedStreamQueue<AgentRunnerStreamEvent> queue,
                                               std::thread producer,
                                               std::shared_ptr<CancellationToken> owned_cancellation)
    : queue_(std::move(queue)),
      producer_(std::move(producer)),
      owned_cancellation_(std::move(owned_cancellation)),
      active_(true) {}

AgentRunnerEventStream::AgentRunnerEventStream(AgentRunnerEventStream&& other) noexcept
    : queue_(std::move(other.queue_)),
      producer_(std::move(other.producer_)),
      owned_cancellation_(std::move(other.owned_cancellation_)),
      active_(other.active_) {
  other.active_ = false;
}

AgentRunnerEventStream& AgentRunnerEventStream::operator=(AgentRunnerEventStream&& other) noexcept {
  if (this != &other) {
    if (active_) {
      close();
      join();
    }
    queue_ = std::move(other.queue_);
    producer_ = std::move(other.producer_);
    owned_cancellation_ = std::move(other.owned_cancellation_);
    active_ = other.active_;
    other.active_ = false;
  }
  return *this;
}

AgentRunnerEventStream::~AgentRunnerEventStream() {
  if (active_) {
    close();
    join();
  }
}

bool AgentRunnerEventStream::next(AgentRunnerStreamEvent& event) {
  if (!active_) {
    return false;
  }
  return queue_.next(event);
}

void AgentRunnerEventStream::close() {
  cancel("Stream closed.");
}

void AgentRunnerEventStream::cancel(std::string reason) {
  if (active_) {
    queue_.close();
    if (owned_cancellation_) {
      owned_cancellation_->cancel(reason.empty() ? "Stream cancelled." : std::move(reason));
    }
  }
}

void AgentRunnerEventStream::join() {
  if (producer_.joinable()) {
    producer_.join();
  }
}


}  // namespace agent
