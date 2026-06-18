#include "internal.hpp"

#include <utility>

namespace agent {

std::string to_string(AgentLoopTerminationReason reason) {
  switch (reason) {
    case AgentLoopTerminationReason::Completed:
      return "completed";
    case AgentLoopTerminationReason::MaxIterations:
      return "max-iterations";
    case AgentLoopTerminationReason::IncompleteResponse:
      return "incomplete-response";
  }
  return "completed";
}

std::string to_string(AgentLoopStreamEventType type) {
  switch (type) {
    case AgentLoopStreamEventType::IterationStart:
      return "iteration-start";
    case AgentLoopStreamEventType::ModelStart:
      return "model-start";
    case AgentLoopStreamEventType::ModelTextDelta:
      return "model-text-delta";
    case AgentLoopStreamEventType::UserVisibleDelta:
      return "user-visible-delta";
    case AgentLoopStreamEventType::ModelReasoningDelta:
      return "model-reasoning-delta";
    case AgentLoopStreamEventType::ModelReasoningCompleted:
      return "model-reasoning-completed";
    case AgentLoopStreamEventType::AgentOutput:
      return "model-response";
    case AgentLoopStreamEventType::ToolCallArgumentDelta:
      return "tool-call-argument-delta";
    case AgentLoopStreamEventType::ReActActionBatch:
      return "react-action-batch";
    case AgentLoopStreamEventType::ToolBatchStart:
      return "tool-batch-start";
    case AgentLoopStreamEventType::ToolStart:
      return "tool-start";
    case AgentLoopStreamEventType::ToolDelta:
      return "tool-delta";
    case AgentLoopStreamEventType::ToolComplete:
      return "tool-complete";
    case AgentLoopStreamEventType::ToolBatchComplete:
      return "tool-batch-complete";
    case AgentLoopStreamEventType::ReActMessage:
      return "react-message";
    case AgentLoopStreamEventType::ReActObservation:
      return "react-observation";
    case AgentLoopStreamEventType::ReActFinal:
      return "react-final";
    case AgentLoopStreamEventType::ReActFinalRejected:
      return "react-final-rejected";
    case AgentLoopStreamEventType::ReActReasoningProtocolLeak:
      return "react-reasoning-protocol-leak";
    case AgentLoopStreamEventType::ReActParseError:
      return "react-parse-error";
    case AgentLoopStreamEventType::Done:
      return "done";
  }
  return "iteration-start";
}

std::string to_string(AgentRunnerStreamEventType type) {
  switch (type) {
    case AgentRunnerStreamEventType::Status:
      return "status";
    case AgentRunnerStreamEventType::KnowledgeRetrieval:
      return "knowledge-retrieval";
    case AgentRunnerStreamEventType::MemoryRetrieval:
      return "memory-retrieval";
    case AgentRunnerStreamEventType::Planning:
      return "plan";
    case AgentRunnerStreamEventType::UserVisibleDelta:
      return "user-visible-delta";
    case AgentRunnerStreamEventType::Loop:
      return "loop";
    case AgentRunnerStreamEventType::ToolCallArgumentDelta:
      return "tool-call-argument-delta";
    case AgentRunnerStreamEventType::Done:
      return "done";
    case AgentRunnerStreamEventType::Cancelled:
      return "cancelled";
    case AgentRunnerStreamEventType::Error:
      return "error";
  }
  return "loop";
}

std::string to_string(AgentToolCallingStrategy strategy) {
  switch (strategy) {
    case AgentToolCallingStrategy::TextReAct:
      return "text-react";
    case AgentToolCallingStrategy::NativeToolCalling:
      return "native-tool-calling";
  }
  return "text-react";
}

}  // namespace agent
