#pragma once

#include "internal.hpp"

namespace agent {

struct ModelStreamReductionState {
  AgentOutput response;
  bool saw_response = false;
};

class StreamEventReducer {
 public:
  explicit StreamEventReducer(EventBus* event_bus);

  void reduce_model_event(const ModelStreamEvent& event,
                          int iteration,
                          const TraceContext& model_trace,
                          const AgentLoopStreamEventHandler& emit_stream_event,
                          ModelStreamReductionState& state) const;

 private:
  EventBus* event_bus_;
};

}  // namespace agent
