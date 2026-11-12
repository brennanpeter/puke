#include "comp/graph/Node.h"

#include <atomic>

namespace comp {

namespace {
std::atomic<std::uint64_t> nextNodeId{1};
}

Node::Node(std::string name) : name_(std::move(name)), id_(nextNodeId.fetch_add(1)) {}

void Node::setInput(int index, Node* node) {
    if (index < 0 || index >= maxInputs())
        throw Error(name_ + ": input index " + std::to_string(index) + " out of range");
    if (static_cast<int>(inputs_.size()) <= index) inputs_.resize(index + 1, nullptr);
    inputs_[index] = node;
}

Node* Node::input(int index) const {
    if (index < 0 || index >= static_cast<int>(inputs_.size())) return nullptr;
    return inputs_[index];
}

double Node::numberParam(const std::string& key, double fallback) const {
    auto it = params_.find(key);
    if (it == params_.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* i = std::get_if<int>(&it->second)) return *i;
    throw Error(name_ + ": parameter '" + key + "' must be a number");
}

Vec2 Node::vec2Param(const std::string& key, Vec2 fallback) const {
    auto it = params_.find(key);
    if (it == params_.end()) return fallback;
    if (auto* v = std::get_if<Vec2>(&it->second)) return *v;
    const double n = numberParam(key, 0.0);
    return {n, n};
}

Color Node::colorParam(const std::string& key, double fallback) const {
    auto it = params_.find(key);
    if (it == params_.end()) return {fallback, fallback, fallback, fallback};
    if (auto* c = std::get_if<Color>(&it->second)) return *c;
    const double n = numberParam(key, 0.0);
    return {n, n, n, n};
}

Node& Node::requireInput(int index) const {
    Node* n = input(index);
    if (!n) throw Error(name_ + ": input " + std::to_string(index) + " is not connected");
    return *n;
}

}  // namespace comp
