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

    // Like remove(), but hands the node back instead of deleting it, so it
    // can be put back later (undo): see insert(). Its connections are
    // cleared both ways; the caller records them if it needs them.
    std::shared_ptr<Node> take(Node& node);
    // Puts a node (e.g. one from take()) at `index` in the node order, or
    // at the end if `index` is past it. Throws if its name is taken.
    Node& insert(std::shared_ptr<Node> node, std::size_t index);
    // Where the node is in nodes(); throws if it isn't in this graph.
    std::size_t indexOf(const Node& node) const;

    // Renames a node. Throws if the name is empty or used by another node.
    void rename(Node& node, const std::string& name);

    // Nodes that use `node` as an input.
    std::vector<Node*> consumers(const Node& node) const;

    // Nodes in the order they were added. Shared so a render's Snapshot can
    // keep a node alive after it's removed from the graph.
    const std::vector<std::shared_ptr<Node>>& nodes() const { return nodes_; }

private:
    void checkNewName(const std::string& name) const;
    void requireOwned(const Node& node) const;

    std::vector<std::shared_ptr<Node>> nodes_;
};

}  // namespace comp
