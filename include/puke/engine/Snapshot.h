#pragma once

#include <memory>
#include <vector>

#include "puke/graph/Node.h"

namespace puke {

// A frozen copy of everything a render needs from the graph: the nodes
// upstream of an output, their parameter values and their connections, as
// they were when the snapshot was taken.
//
// Take a snapshot on the thread that edits the graph, then render it on any
// thread, while the graph goes on being edited. Taking one is cheap: it
// copies pointers, not parameters (Node parameters are copy-on-write) and
// not pixels. Nodes are kept alive by the snapshot even if they're removed
// from the graph meanwhile.
//
// Rendering a snapshot never reads the live graph: the engine uses the
// snapshot's connections, and binds each node's snapshot parameters around
// every call it makes into the node (see ParamScope).
class Snapshot {
public:
    struct Entry {
        std::shared_ptr<const Node> keepAlive;  // null if the node isn't shared-owned
        const Node* node = nullptr;
        std::shared_ptr<const ParamMap> params;
        std::shared_ptr<const CurveMap> curves;  // evaluated at the render's frame
        std::vector<int> inputs;  // index into entries() per input; -1 if unconnected
    };

    // Walks upstream from `output`. Throws Error if a required input isn't
    // connected or there's a cycle.
    static Snapshot of(const Node& output);

    // The part of this snapshot that feeds the output's input `index`, as a
    // snapshot whose output is that input. Throws Error if it's unconnected.
    Snapshot upstream(int index) const;

    // This snapshot with every animated parameter evaluated at `frame`
    // into a plain value (and no curves left). The engine renders these.
    Snapshot at(double frame) const;

    // Inputs before the nodes that use them; the output is last.
    const std::vector<Entry>& entries() const { return entries_; }
    const Entry& output() const { return entries_.back(); }

private:
    std::vector<Entry> entries_;
};

}  // namespace puke
