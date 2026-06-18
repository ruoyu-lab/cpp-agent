#pragma once

#include "agent/tools.hpp"

namespace agent {

class WorkflowEngine;
struct WorkflowDefinition;

inline const ToolServiceToken<WorkflowEngine> kToolServiceWorkflowEngine{"workflow.engine"};
inline const ToolServiceToken<const WorkflowDefinition> kToolServiceWorkflowDefinition{
    "workflow.definition"};

}  // namespace agent
