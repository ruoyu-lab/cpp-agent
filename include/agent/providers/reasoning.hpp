#pragma once

#include "agent/model.hpp"
#include "agent/providers/native.hpp"
#include "agent/providers/reasoning_types.hpp"

#include <memory>

namespace agent {

struct ProviderRequestProfile {
  std::string provider;
  std::string runtime;
  ProviderRequestProtocol protocol = ProviderRequestProtocol::Unknown;
  ProviderReasoningMode reasoning_mode = ProviderReasoningMode::None;
  bool supports_reasoning = false;
  bool supports_reasoning_budget = false;
  bool supports_reasoning_visibility = false;
  bool strict_reasoning = false;
  ProviderReasoningDisableStrategy reasoning_disable_strategy =
      ProviderReasoningDisableStrategy::Unknown;
};

struct ProviderReasoningContext {
  std::string provider;
  std::string model;
  std::optional<ReasoningSettings> reasoning;
  ProviderRequestProfile profile;
};

struct ProviderReasoningApplyResult {
  bool requested = false;
  bool applied = false;
  bool disabled = false;
  bool degraded = false;
  bool unsupported = false;
  ProviderReasoningMode mode = ProviderReasoningMode::None;
  ProviderReasoningDisableStrategy disable_strategy =
      ProviderReasoningDisableStrategy::Unknown;
  std::string message;
  Value metadata = Value::object({});
};

class ProviderReasoningMapper {
 public:
  virtual ~ProviderReasoningMapper() = default;

  [[nodiscard]] virtual bool matches(const ProviderRequestProfile& profile) const = 0;
  virtual ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const = 0;
};

class ProviderReasoningMapperRegistry {
 public:
  ProviderReasoningMapperRegistry();

  void add(std::shared_ptr<const ProviderReasoningMapper> mapper);
  [[nodiscard]] const ProviderReasoningMapper& resolve(
      const ProviderRequestProfile& profile) const;
  ProviderReasoningApplyResult apply(
      NativeProviderRequest& request,
      const ProviderReasoningContext& context) const;

  [[nodiscard]] static const ProviderReasoningMapperRegistry& default_registry();

 private:
  std::vector<std::shared_ptr<const ProviderReasoningMapper>> mappers_;
};

[[nodiscard]] std::string to_string(ProviderRequestProtocol protocol);
[[nodiscard]] std::string to_string(ProviderReasoningMode mode);
[[nodiscard]] std::string to_string(ProviderReasoningDisableStrategy strategy);
[[nodiscard]] Value provider_reasoning_apply_result_to_value(
    const ProviderReasoningApplyResult& result);

[[nodiscard]] ProviderRequestProfile make_provider_request_profile(
    std::string provider,
    std::string model,
    ProviderRequestProtocol protocol,
    std::string runtime = {},
    ProviderReasoningMode reasoning_mode = ProviderReasoningMode::None,
    ProviderReasoningDisableStrategy reasoning_disable_strategy =
        ProviderReasoningDisableStrategy::Unknown);

ProviderReasoningApplyResult apply_provider_reasoning(
    NativeProviderRequest& request,
    ProviderReasoningContext context);

[[nodiscard]] bool provider_request_profile_supports_reasoning_field(
    const ProviderRequestProfile& profile,
    const std::string& field);

}  // namespace agent
