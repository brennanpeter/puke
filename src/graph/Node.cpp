#include "comp/graph/Node.h"

#include <atomic>
#include <string>

namespace comp {

namespace {
std::atomic<std::uint64_t> nextNodeId{1};

// The ParamScope in effect on this thread, if any.
thread_local const Node* boundNode = nullptr;
thread_local const ParamMap* boundParams = nullptr;
thread_local std::span<const ImageInfo* const> boundInputs;
}  // namespace

ParamScope::ParamScope(const Node& node, const ParamMap& params, std::span<const ImageInfo* const> inputInfos)
    : prevNode_(boundNode), prevParams_(boundParams), prevInputs_(boundInputs) {
    boundNode = &node;
    boundParams = &params;
    boundInputs = inputInfos;
}

ParamScope::~ParamScope() {
    boundNode = prevNode_;
    boundParams = prevParams_;
    boundInputs = prevInputs_;
}

const ImageInfo& Node::inputInfo(int index) const {
    if (boundNode != this || boundInputs.empty())
        throw Error(name_ + ": inputInfo() is only available while the engine renders this node");
    if (index < 0 || index >= static_cast<int>(boundInputs.size()) || !boundInputs[index])
        throw Error(name_ + ": input " + std::to_string(index) + " is not connected");
    return *boundInputs[index];
}

namespace {
const std::shared_ptr<const ParamMap>& emptyParams() {
    static const auto empty = std::make_shared<const ParamMap>();
    return empty;
}
const std::shared_ptr<const CurveMap>& emptyCurves() {
    static const auto empty = std::make_shared<const CurveMap>();
    return empty;
}
}  // namespace

Node::Node(std::string name) : name_(std::move(name)), id_(nextNodeId.fetch_add(1)), params_(emptyParams()), curves_(emptyCurves()) {}

const ParamMap& Node::params() const { return boundNode == this ? *boundParams : *params_; }

void Node::setParam(const std::string& key, ParamValue value) {
    if (animated(key)) setCurve(key, {});  // first: dropping a curve unsets the plain value too
    auto copy = std::make_shared<ParamMap>(*params_);
    (*copy)[key] = std::move(value);
    params_ = std::move(copy);
}

void Node::clearParam(const std::string& key) {
    if (animated(key)) setCurve(key, {});
    if (!params_->count(key)) return;
    auto copy = std::make_shared<ParamMap>(*params_);
    copy->erase(key);
    params_ = std::move(copy);
}

const Curve* Node::curve(const std::string& key) const {
    auto it = curves_->find(key);
    return it == curves_->end() ? nullptr : &it->second;
}

void Node::setCurve(const std::string& key, Curve curve) {
    auto copy = std::make_shared<CurveMap>(*curves_);
    if (curve.empty()) copy->erase(key);
    else (*copy)[key] = std::move(curve);
    curves_ = std::move(copy);
    if (params_->count(key)) {  // animated or unset: never both
        auto p = std::make_shared<ParamMap>(*params_);
        p->erase(key);
        params_ = std::move(p);
    }
}

std::optional<ParamValue> Node::valueAt(const std::string& key, double frame) const {
    if (const Curve* c = curve(key)) return c->valueAt(frame);
    auto it = params_->find(key);
    if (it == params_->end()) return std::nullopt;
    return it->second;
}

ParamMap Node::paramsAt(double frame) const { return *evaluate(params_, *curves_, frame); }

std::shared_ptr<const ParamMap> Node::evaluate(const std::shared_ptr<const ParamMap>& params, const CurveMap& curves,
                                               double frame) {
    if (curves.empty()) return params;
    auto out = std::make_shared<ParamMap>(*params);
    for (const auto& [key, curve] : curves) (*out)[key] = curve.valueAt(frame);
    return out;
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
