#include "comp/engine/Snapshot.h"

#include <string>
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
        e.curves = node->curvesSnapshot();
        e.inputs.assign(node->maxInputs(), -1);
        for (int j = 0; j < node->maxInputs(); ++j)
            if (const Node* in = node->input(j)) e.inputs[j] = index.at(in);
        snap.entries_.push_back(std::move(e));
    }
    return snap;
}

Snapshot Snapshot::upstream(int index) const {
    const Entry& out = output();
    if (index < 0 || index >= static_cast<int>(out.inputs.size()) || out.inputs[index] < 0)
        throw Error(out.node->name() + ": input " + std::to_string(index) + " is not connected");
    // Entries are in dependency order, so walking down from the input finds
    // everything it needs.
    const int top = out.inputs[index];
    std::vector<bool> needed(top + 1, false);
    needed[top] = true;
    for (int i = top; i >= 0; --i)
        if (needed[i])
            for (int in : entries_[i].inputs)
                if (in >= 0) needed[in] = true;
    std::vector<int> remap(top + 1, -1);
    Snapshot snap;
    for (int i = 0; i <= top; ++i) {
        if (!needed[i]) continue;
        remap[i] = static_cast<int>(snap.entries_.size());
        Entry e = entries_[i];
        for (int& in : e.inputs)
            if (in >= 0) in = remap[in];
        snap.entries_.push_back(std::move(e));
    }
    return snap;
}

Snapshot Snapshot::at(double frame) const {
    Snapshot out = *this;
    for (Entry& e : out.entries_) {
        if (!e.curves || e.curves->empty()) continue;
        e.params = Node::evaluate(e.params, *e.curves, frame);
        e.curves = nullptr;
    }
    return out;
}

}  // namespace comp
