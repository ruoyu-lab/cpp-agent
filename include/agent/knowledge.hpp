#pragma once

// Knowledge APIs currently share storage and embedding primitives with the
// memory package. This header gives hosts a stable domain entrypoint while the
// implementation continues to split out of memory.cpp.
#include "agent/memory.hpp"

