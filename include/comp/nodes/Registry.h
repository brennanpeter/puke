#pragma once

#include <memory>
#include <string>
#include <vector>

#include "comp/graph/Node.h"

namespace comp {

// Built-in node types, by the name their typeName() returns ("Read",
// "Grade", ...). Front ends that build graphs from text (Python, project
// files) create nodes through this rather than naming C++ classes.

// Every type name createNode() accepts, in alphabetical order.
std::vector<std::string> nodeTypes();

// A new, unconnected node of the given type. Throws Error for an unknown type.
// Add it to a graph with Graph::add().
std::unique_ptr<Node> createNode(const std::string& type, const std::string& name);

}  // namespace comp
