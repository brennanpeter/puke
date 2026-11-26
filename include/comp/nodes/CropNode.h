#pragma once

#include "comp/graph/Node.h"

namespace comp {

// Keeps a rectangle of the image and blacks out the rest.
//
// Parameters:
//   box_min   (vec2, 0 0)          the box's top-left corner, in pixels
//   box_max   (vec2, 1920 1080)    its bottom-right corner (exclusive). A GUI
//                                  should set the box to the input's frame
//                                  when the node is created, as for
//                                  Transform's center.
//   reformat  (bool, false)        make the box the new frame: move it to the
//                                  origin and make it the display window.
//                                  Otherwise the frame is unchanged.
//
// Corners are rounded to whole pixels. The data window shrinks to the box,
// so nothing outside it is computed upstream.
class CropNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Crop"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    // The box, in input pixels at `scale` (see RenderContext::scale).
    // Throws if it's empty.
    Box box(double scale = 1) const;

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Box inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;
};

}  // namespace comp
