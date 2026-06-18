#pragma once

#include "agent/model.hpp"
#include "agent/providers/native.hpp"

namespace agent {

Value serialize_chat_tool_descriptor(const ChatToolDescriptor& tool);
Value serialize_openai_chat_messages(const std::vector<AgentMessage>& messages);

NativeProviderRequest build_openai_chat_request(const GenerateParams& params, std::string model,
                                                std::string endpoint,
                                                std::string base_url);
NativeProviderRequest build_openai_chat_stream_request(const GenerateParams& params, std::string model,
                                                       std::string endpoint,
                                                       std::string base_url);

AgentOutput parse_openai_chat_response(const Value& raw, std::string provider, std::string model);
std::vector<ModelStreamEvent> parse_openai_chat_stream_events(
    const std::vector<std::string>& chunks,
    std::string provider,
    std::string model);

}  // namespace agent
