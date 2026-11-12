#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "comp/graph/Node.h"

namespace comp {

// Owns nodes and keeps their connections valid: unique names, in-range
// inputs, no cycles. Holds no pixels; rendering is the Engine's job.
class Graph {
public:
    template <typename T, typename... Args>
    T& create(const std::string& name, Args&&... args) {
        if (name.empty()) throw Error("node name cannot be empty");
        if (find(name)) throw Error("a node named '" + name + "' already exists");
        auto node = std::make_unique<T>(name, std::forward<Args>(args)...);
        T& ref = *node;
        nodes_.push_back(std::move(node));
        return ref;
    }

    Node* find(const std::string& name) const;

    // Feeds `source` into input `index` of `target`. Throws if it would create a cycle.
    void connect(Node& source, Node& target, int index);
    void disconnect(Node& target, int index);

    // Deletes the node, disconnecting anything that used it as an input.
    void remove(Node& node);

    // Nodes that use `node` as an input.
    std::vector<Node*> consumers(const Node& node) const;

    const std::vector<std::unique_ptr<Node>>& nodes() const { return nodes_; }

private:
    void requireOwned(const Node& node) const;

    std::vector<std::unique_ptr<Node>> nodes_;
};

}  // namespace comp
