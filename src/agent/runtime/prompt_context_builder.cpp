#include "prompt_context_builder.hpp"

namespace agent {

PromptContextBuildResult PromptContextBuilder::build(PromptContextBuildOptions options) const {
    if (!options.context_manager) {
      throw ConfigurationError("PromptContextBuilder requires a context manager.");
    }
    if (!options.session) {
      throw ConfigurationError("PromptContextBuilder requires a session.");
    }

    PromptContextBuildResult result;
    auto& assembly = result.assembly;
    assembly.session_id = options.session->session_id();
    assembly.iteration = options.iteration;
    if (options.trace_length) {
      assembly.metadata = Value::object({{"traceLength", static_cast<long long>(*options.trace_length)}});
    }

    auto append_segment = [&](PromptAssemblySegment segment) {
      if (segment.messages.empty()) {
        return;
      }
      assembly.messages.insert(assembly.messages.end(), segment.messages.begin(), segment.messages.end());
      assembly.segments.push_back(std::move(segment));
    };

    if (!options.system_messages.empty()) {
      append_segment(PromptAssemblySegment{
          .id = "system.prompt",
          .label = "System Prompt",
          .kind = PromptAssemblySegmentKind::System,
          .messages = std::move(options.system_messages),
          .metadata = options.system_metadata.is_object() ? options.system_metadata : Value::object({}),
      });
    } else if (!options.system_prompt.empty()) {
      append_segment(PromptAssemblySegment{
          .id = "system.prompt",
          .label = "System Prompt",
          .kind = PromptAssemblySegmentKind::System,
          .messages = {create_message(MessageRole::System, options.system_prompt,
                                      Value::object({{"source", "runner"}}))},
          .metadata = Value::object({{"source", "runner"}}),
      });
    }
    if (!options.preface_messages.empty()) {
      const auto preface_count = options.preface_messages.size();
      append_segment(PromptAssemblySegment{
          .id = "runtime.preface",
          .label = "Runtime Preface",
          .kind = PromptAssemblySegmentKind::Preface,
          .messages = std::move(options.preface_messages),
          .metadata = Value::object({{"source", "runner"},
                                    {"count", static_cast<long long>(preface_count)}}),
      });
    }

    Value runtime = options.runtime_context.is_object() ? options.runtime_context : Value::object({});
    runtime["input"] = options.input_value.is_null() ? Value(options.input) : options.input_value;
    runtime["iteration"] = options.iteration;
    result.context_assembly = options.context_manager->build_assembly(runtime);
    if (result.context_assembly.message) {
      Value::Array blocks;
      for (const auto& block : result.context_assembly.blocks) {
        blocks.push_back(block.stats_id);
      }
      append_segment(PromptAssemblySegment{
          .id = "runtime.embedded-context",
          .label = "Embedded Context",
          .kind = PromptAssemblySegmentKind::EmbeddedContext,
          .messages = {*result.context_assembly.message},
          .metadata = Value::object({{"source", "embedded-context"}, {"blocks", Value(std::move(blocks))}}),
      });
    }

    auto session_messages = options.session->get_messages();
    append_segment(PromptAssemblySegment{
        .id = "session.conversation",
        .label = "Conversation",
        .kind = PromptAssemblySegmentKind::Conversation,
        .messages = std::move(session_messages),
        .metadata = Value::object({{"source", "session-memory"},
                                  {"sessionId", options.session->session_id()}}),
    });
    return result;
  }


}  // namespace agent
