#include "comp/engine/Snapshot.h"

#include <unordered_map>

namespace comp {

// Iterative depth-first search, so deep graphs can't overflow the call stack.
Snapshot Snapshot::of(const Node& output) {
    enum class Mark { Visiting, Done };
    struct Frame {
        const Node* node;
        int nextInput;
    };

    std::vector<const Node*> order;
    std::unordered_map<const Node*, int> index;
    std::unordered_map<const Node*, Mark> marks{{&output, Mark::Visiting}};
    std::vector<Frame> stack{{&output, 0}};

    while (!stack.empty()) {
        const Node* node = stack.back().node;
        const int i = stack.back().nextInput;

        if (i < node->maxInputs()) {
            ++stack.back().nextInput;
            const Node* in = node->input(i);
            if (!in) {
                if (!node->inputOptional(i))
                    throw Error(node->name() + ": input " + std::to_string(i) + " is not connected");
                continue;
            }
            auto it = marks.find(in);
            if (it == marks.end()) {
                marks.emplace(in, Mark::Visiting);
                stack.push_back({in, 0});
            } else if (it->second == Mark::Visiting) {
                throw Error("cycle in graph: " + node->name() + " depends on " + in->name() +
                            ", which depends on " + node->name());
            }
        } else {
            marks[node] = Mark::Done;
            index.emplace(node, static_cast<int>(order.size()));
            order.push_back(node);
            stack.pop_back();
        }
    }

    Snapshot snap;
    snap.entries_.reserve(order.size());
    for (const Node* node : order) {
        Entry e;
        e.keepAlive = node->weak_from_this().lock();
        e.node = node;
        e.params = node->paramsSnapshot();
        e.inputs.assign(node->maxInputs(), -1);
        for (int j = 0; j < node->maxInputs(); ++j)
            if (const Node* in = node->input(j)) e.inputs[j] = index.at(in);
        snap.entries_.push_back(std::move(e));
    }
    return snap;
}

}  // namespace comp
