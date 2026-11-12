#include "comp/graph/Graph.h"

#include <algorithm>
#include <unordered_set>

namespace comp {
namespace {

// True if `target` is `start` or anywhere upstream of it.
bool isUpstreamOrSelf(const Node& start, const Node& target) {
    std::vector<const Node*> stack{&start};
    std::unordered_set<const Node*> seen;
    while (!stack.empty()) {
        const Node* n = stack.back();
        stack.pop_back();
        if (n == &target) return true;
        if (!seen.insert(n).second) continue;
        for (int i = 0; i < n->maxInputs(); ++i)
            if (const Node* in = n->input(i)) stack.push_back(in);
    }
    return false;
}

}  // namespace

Node* Graph::find(const std::string& name) const {
    for (const auto& n : nodes_)
        if (n->name() == name) return n.get();
    return nullptr;
}

void Graph::requireOwned(const Node& node) const {
    for (const auto& n : nodes_)
        if (n.get() == &node) return;
    throw Error(node.name() + ": node does not belong to this graph");
}

void Graph::connect(Node& source, Node& target, int index) {
    requireOwned(source);
    requireOwned(target);
    if (isUpstreamOrSelf(source, target))
        throw Error("connecting " + source.name() + " into " + target.name() + " would create a cycle");
    target.setInput(index, &source);
}

void Graph::disconnect(Node& target, int index) {
    requireOwned(target);
    target.setInput(index, nullptr);
}

std::vector<Node*> Graph::consumers(const Node& node) const {
    std::vector<Node*> out;
    for (const auto& n : nodes_)
        for (int i = 0; i < n->maxInputs(); ++i)
            if (n->input(i) == &node) {
                out.push_back(n.get());
                break;
            }
    return out;
}

void Graph::remove(Node& node) {
    requireOwned(node);
    for (const auto& n : nodes_)
        for (int i = 0; i < n->maxInputs(); ++i)
            if (n->input(i) == &node) n->setInput(i, nullptr);
    std::erase_if(nodes_, [&](const auto& n) { return n.get() == &node; });
}

}  // namespace comp
