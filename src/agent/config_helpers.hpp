#pragma once

#include "agent/core.hpp"
#include "agent/providers/reasoning_types.hpp"

#include <string>

namespace agent::config_detail {

std::string env_value(const std::string& key);
std::string resolve_env_config_value(const Value& value, const Value& env_key);
std::string resolve_defaulted_env_config_value(const Value& value,
                                               const Value& explicit_env_key,
                                               const std::string& default_env_key,
                                               const std::string& fallback = {});
ProviderReasoningMode reasoning_mode_from_string(const std::string& value, const std::string& path);
ProviderReasoningMode reasoning_mode_from_model_config(const Value& model, const std::string& path);
int embedding_dimensions_from_config(const Value& value, int fallback);

}  // namespace agent::config_detail
