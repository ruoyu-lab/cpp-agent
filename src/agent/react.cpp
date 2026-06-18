#include "agent/react.hpp"
#include "detail/helpers.hpp"
#include "runtime/prompt_context_builder.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace agent {

#include "react/common_foundation.inc"
#include "react/final_answer_validation_helpers.inc"
#include "react/runtime_context_helpers.inc"
#include "react/visible_stream_parser.inc"
#include "react/parser.inc"
#include "react/prompt_and_observation.inc"
#include "react/model_calls.inc"
#include "react/loop.inc"

}  // namespace agent
