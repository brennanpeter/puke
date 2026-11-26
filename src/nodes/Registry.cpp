#include "puke/nodes/Registry.h"

#include <functional>
#include <map>

#include "puke/nodes/BackdropNode.h"
#include "puke/nodes/ColorSpaceNode.h"
#include "puke/nodes/ConstantNode.h"
#include "puke/nodes/ShuffleNode.h"
#include "puke/nodes/ReformatNode.h"
#include "puke/nodes/RotoNode.h"
#include "puke/nodes/PremultNode.h"
#include "puke/nodes/CropNode.h"
#include "puke/nodes/BlurNode.h"
#include "puke/nodes/GradeNode.h"
#include "puke/nodes/MergeNode.h"
#include "puke/nodes/ReadNode.h"
#include "puke/nodes/TransformNode.h"
#include "puke/nodes/WriteNode.h"
#include "puke/ofx/OfxHost.h"

namespace puke {
namespace {

using Factory = std::function<std::unique_ptr<Node>(const std::string& name)>;

template <typename T>
Factory make() {
    return [](const std::string& name) { return std::make_unique<T>(name); };
}

// std::map, so nodeTypes() comes out sorted.
const std::map<std::string, Factory>& registry() {
    static const std::map<std::string, Factory> types{
        {"Backdrop", make<BackdropNode>()},
        {"Blur", make<BlurNode>()},
        {"ColorSpace", make<ColorSpaceNode>()},
        {"Constant", make<ConstantNode>()},
        {"Crop", make<CropNode>()},
        {"Grade", make<GradeNode>()},
        {"Merge", make<MergeNode>()},
        {"Premult", make<PremultNode>()},
        {"Read", make<ReadNode>()},
        {"Reformat", make<ReformatNode>()},
        {"Roto", make<RotoNode>()},
        {"Shuffle", make<ShuffleNode>()},
        {"Transform", make<TransformNode>()},
        {"Unpremult", make<UnpremultNode>()},
        {"Write", make<WriteNode>()},
    };
    return types;
}

}  // namespace

std::vector<std::string> nodeTypes() {
    std::vector<std::string> names;
    for (const auto& [name, factory] : registry()) names.push_back(name);
    for (const auto& p : ofx::plugins()) names.push_back(ofx::kNodeTypePrefix + p.identifier);
    return names;
}

std::unique_ptr<Node> createNode(const std::string& type, const std::string& name) {
    const std::string prefix = ofx::kNodeTypePrefix;
    if (type.rfind(prefix, 0) == 0) return ofx::createNode(type.substr(prefix.size()), name);
    auto it = registry().find(type);
    if (it == registry().end()) {
        std::string known;
        for (const auto& [t, factory] : registry()) known += (known.empty() ? "" : ", ") + t;
        throw Error("unknown node type '" + type + "' (built-in types: " + known +
                    "; OFX plugins are \"ofx:<plugin id>\")");
    }
    return it->second(name);
}

}  // namespace puke
