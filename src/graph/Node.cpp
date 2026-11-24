#include "comp/graph/Node.h"

#include <atomic>

namespace comp {

namespace {
std::atomic<std::uint64_t> nextNodeId{1};

// The ParamScope in effect on this thread, if any.
thread_local const Node* boundNode = nullptr;
thread_local const ParamMap* boundParams = nullptr;
}  // namespace

ParamScope::ParamScope(const Node& node, const ParamMap& params) : prevNode_(boundNode), prevParams_(boundParams) {
    boundNode = &node;
    boundParams = &params;
}

ParamScope::~ParamScope() {
    boundNode = prevNode_;
    boundParams = prevParams_;
}

namespace {
const std::shared_ptr<const ParamMap>& emptyParams() {
    static const auto empty = std::make_shared<const ParamMap>();
    return empty;
}
}  // namespace

Node::Node(std::string name) : name_(std::move(name)), id_(nextNodeId.fetch_add(1)), params_(emptyParams()) {}

const ParamMap& Node::params() const { return boundNode == this ? *boundParams : *params_; }

void Node::setParam(const std::string& key, ParamValue value) {
    auto copy = std::make_shared<ParamMap>(*params_);
    (*copy)[key] = std::move(value);
    params_ = std::move(copy);
}

void Node::clearParam(const std::string& key) {
    if (!params_->count(key)) return;
    auto copy = std::make_shared<ParamMap>(*params_);
    copy->erase(key);
    params_ = std::move(copy);
}

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
    const ParamMap& ps = params();
    auto it = ps.find(key);
    if (it == ps.end()) return fallback;
    if (auto* d = std::get_if<double>(&it->second)) return *d;
    if (auto* i = std::get_if<int>(&it->second)) return *i;
    throw Error(name_ + ": parameter '" + key + "' must be a number");
}

Vec2 Node::vec2Param(const std::string& key, Vec2 fallback) const {
    const ParamMap& ps = params();
    auto it = ps.find(key);
    if (it == ps.end()) return fallback;
    if (auto* v = std::get_if<Vec2>(&it->second)) return *v;
    const double n = numberParam(key, 0.0);
    return {n, n};
}

Color Node::colorParam(const std::string& key, double fallback) const {
    const ParamMap& ps = params();
    auto it = ps.find(key);
    if (it == ps.end()) return {fallback, fallback, fallback, fallback};
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
