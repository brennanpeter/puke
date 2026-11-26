#pragma once

#include <memory>
#include <string>
#include <vector>

#include "puke/graph/Node.h"

namespace puke {

// Built-in node types, by the name their typeName() returns ("Read",
// "Grade", ...). Front ends that build graphs from text (Python, project
// files) create nodes through this rather than naming C++ classes.

// Every type name createNode() accepts: the built-in types in alphabetical
// order, then OFX plugins as "ofx:<identifier>" (see puke/ofx/OfxHost.h).
// The first call scans for OFX plugins.
std::vector<std::string> nodeTypes();

// A new, unconnected node of the given type. Throws Error for an unknown type.
// Add it to a graph with Graph::add().
std::unique_ptr<Node> createNode(const std::string& type, const std::string& name);

}  // namespace puke
