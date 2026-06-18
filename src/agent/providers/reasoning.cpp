#include "agent/providers/reasoning.hpp"

#include "agent/core.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace agent {

#include "reasoning/common.inc"
#include "reasoning/mappers.inc"
#include "reasoning/registry.inc"
#include "reasoning/serialization.inc"
#include "reasoning/profile_policy.inc"
#include "reasoning/apply.inc"

}  // namespace agent
