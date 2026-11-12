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
        checkNewName(name);
        return static_cast<T&>(add(std::make_unique<T>(name, std::forward<Args>(args)...)));
    }

    // Takes ownership of a node made elsewhere (e.g. by createNode()).
    // Throws if its name is empty or already used (the node is then discarded).
    Node& add(std::unique_ptr<Node> node);

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
    void checkNewName(const std::string& name) const;
    void requireOwned(const Node& node) const;

    std::vector<std::unique_ptr<Node>> nodes_;
};

}  // namespace comp
