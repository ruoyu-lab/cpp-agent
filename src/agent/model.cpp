#include "agent/model_api.hpp"
#include "detail/helpers.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>

namespace agent {

#include "model/core_helpers.inc"
#include "model/tagged_reasoning_helpers.inc"
#include "model/chat_core.inc"
#include "model/embedding_core_helpers.inc"
#include "model/embeddings_core.inc"
#include "model/usage.inc"

}  // namespace agent
