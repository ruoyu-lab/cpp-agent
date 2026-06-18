#pragma once

#include "agent/tools.hpp"

namespace agent {

enum class ReActStepType {
  ActionBatch,
  Final,
  FinalRejected,
  ReasoningProtocolLeak,
  ParseError,
};

std::string to_string(ReActStepType type);

enum class ReActPromptMode {
  Managed,
  Custom,
  External,
};

struct ReActParserOptions {
  std::size_t max_actions = 6;
};

struct ReActAction {
  std::string id;
  int index = 0;
  std::string tool;
  Value input = Value::object({});
};

struct ReActStep {
  ReActStepType type = ReActStepType::ActionBatch;
  int iteration = 0;
  std::string thought;
  std::string visible_message;
  std::vector<ReActAction> actions;
  std::string observation;
  std::string final_answer;
  std::string error;
  bool ok = true;
};

using ReActTraceEntry = ReActStep;

struct ReActParserResult {
  bool ok = false;
  bool final_answer = false;
  ReActStep step;
  std::string error;
};

Value react_trace_entry_to_value(const ReActTraceEntry& entry);

}  // namespace agent
