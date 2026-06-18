#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <thread>

namespace agent {

Value stream_error_payload(const AgentFrameworkError& error) {
  return Value::object({
      {"message", std::string(error.what())},
      {"name", std::string(error.error_name())},
      {"category", std::string(error.error_category())},
      {"code", std::string(error.error_code())},
  });
}

Value stream_cancellation_payload(const CancellationError& error) {
  Value payload = stream_error_payload(error);
  if (!error.target().empty()) {
    payload["target"] = error.target();
  }
  if (!error.reason().empty()) {
    payload["reason"] = error.reason();
  }
  return payload;
}

Value stream_error_payload(const std::exception& error) {
  return Value::object({
      {"message", std::string(error.what())},
      {"name", "Error"},
      {"category", "unknown"},
      {"code", "unknown-error"},
  });
}


}  // namespace agent
