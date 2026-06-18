#include "agent/model_providers.hpp"
#include "agent/providers/reasoning.hpp"
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
#include "model/provider_helpers.inc"
#include "model/provider_protocols.inc"
#include "model/stream_framing.inc"
#include "model/stream_incremental.inc"
#include "model/provider_adapters.inc"
#include "model/provider_embeddings.inc"

}  // namespace agent
