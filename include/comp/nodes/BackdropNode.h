#pragma once

#include "comp/graph/Node.h"

namespace comp {

// A labelled, colored box behind a group of nodes in the node graph, for
// organizing a script (like Nuke's BackdropNode). It has no inputs and its
// output is empty; it only exists for the editor.
//
// Parameters:
//   label  (string, "")                 text shown in the box's title bar
//   color  (color, 0.27 0.30 0.36 1)    fill color
//   size   (vec2, 400 250)              width and height in graph units;
//                                       position() is its top-left corner
class BackdropNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Backdrop"; }
    std::vector<ParamSpec> paramSpecs() const override;

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const>) const override { return {}; }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const>) const override {
        return Image(region, {});
    }
};

}  // namespace comp
