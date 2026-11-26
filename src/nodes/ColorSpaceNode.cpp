#include "puke/nodes/ColorSpaceNode.h"

#include "Specs.h"

#include "puke/color/ColorConfig.h"

namespace puke {

Image ColorSpaceNode::compute(const RenderContext&, const Box& region, std::span<const Image* const> inputs) const {
    Image out = cropped(*inputs[0], region);
    try {
        // The engine only asks for regions inside the data window, and the
        // input image covers exactly what exists of it there.
        ColorConfig::current()->convert(out, param<std::string>("in", "scene_linear"),
                                        param<std::string>("out", "scene_linear"), inputs[0]->region());
    } catch (const Error& e) {
        throw Error(name() + ": " + e.what());
    }
    return out;
}

void ColorSpaceNode::appendHash(Hasher& h, const RenderContext&) const { h.add(ColorConfig::current()->hash()); }

std::vector<ParamSpec> ColorSpaceNode::paramSpecs() const {
    using namespace specs;
    return {
        colorSpace("in", "scene_linear", "Color space the input is in", false),
        colorSpace("out", "scene_linear", "Color space to convert to", false),
    };
}

}  // namespace puke
