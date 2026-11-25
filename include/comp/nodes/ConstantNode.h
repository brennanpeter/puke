#pragma once

#include "comp/graph/Node.h"

namespace comp {

// A solid color filling a whole frame: the simplest generator (no inputs).
//
// Parameters:
//   color   (color, 0 0 0 0)          the RGBA value of every pixel
//   format  (choice, "project")       "project": the render's format
//                                     (RenderContext::format, from the
//                                     project settings); "custom": `size`
//   size    (vec2, 1920 1080)         width and height when format is
//                                     "custom" (whole numbers, at least 1)
//
// The data window is the display window: the frame, nothing outside it.
class ConstantNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Constant"; }
    std::vector<ParamSpec> paramSpecs() const override;

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;
};

}  // namespace comp
