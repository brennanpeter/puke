#include "puke/nodes/BackdropNode.h"

#include "Specs.h"

namespace puke {

std::vector<ParamSpec> BackdropNode::paramSpecs() const {
    using namespace specs;
    return {
        make("label", Kind::String, std::string(), "Text shown in the title bar"),
        make("color", Kind::Color, Color{0.27, 0.30, 0.36, 1.0}, "Fill color"),
        make("size", Kind::Vec2, Vec2{400, 250}, "Width and height in the node graph", 40, 2000),
    };
}

}  // namespace puke
