#include "agent/server_api.hpp"
#include "detail/helpers.hpp"

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>

namespace agent {

#include "server/http_trace_helpers.inc"
#include "server/value_serialization.inc"
#include "server/run_result_serialization.inc"
#include "server/request_value_parsing.inc"
#include "server/access_control.inc"
#include "server/governance_policy.inc"
#include "server/query_filters.inc"
#include "server/approval_codec.inc"
#include "server/audit_values.inc"
#include "server/governance_summary_values.inc"
#include "server/approval_stores.inc"
#include "server/metrics.inc"
#include "server/route_modules.inc"
#include "server/app_core.inc"
#include "server/chat_routes.inc"
#include "server/task_routes.inc"
#include "server/async_routes.inc"
#include "server/autonomous_routes.inc"
#include "server/workflow_routes.inc"
#include "server/approval_routes.inc"
#include "server/workflow_runtime_access.inc"
#include "server/session_access.inc"

}  // namespace agent
