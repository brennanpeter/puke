#include "comp/nodes/Registry.h"

#include <functional>
#include <map>

#include "comp/nodes/GradeNode.h"
#include "comp/nodes/MergeNode.h"
#include "comp/nodes/ReadNode.h"
#include "comp/nodes/TransformNode.h"
#include "comp/nodes/WriteNode.h"

namespace comp {
namespace {

using Factory = std::function<std::unique_ptr<Node>(const std::string& name)>;

template <typename T>
Factory make() {
    return [](const std::string& name) { return std::make_unique<T>(name); };
}

// std::map, so nodeTypes() comes out sorted.
const std::map<std::string, Factory>& registry() {
    static const std::map<std::string, Factory> types{
        {"Grade", make<GradeNode>()},
        {"Merge", make<MergeNode>()},
        {"Read", make<ReadNode>()},
        {"Transform", make<TransformNode>()},
        {"Write", make<WriteNode>()},
    };
    return types;
}

}  // namespace

std::vector<std::string> nodeTypes() {
    std::vector<std::string> names;
    for (const auto& [name, factory] : registry()) names.push_back(name);
    return names;
}

std::unique_ptr<Node> createNode(const std::string& type, const std::string& name) {
    auto it = registry().find(type);
    if (it == registry().end()) {
        std::string known;
        for (const auto& t : nodeTypes()) known += (known.empty() ? "" : ", ") + t;
        throw Error("unknown node type '" + type + "' (known types: " + known + ")");
    }
    return it->second(name);
}

}  // namespace comp
