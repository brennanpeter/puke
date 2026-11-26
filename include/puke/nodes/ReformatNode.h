#pragma once

#include "puke/core/Affine.h"
#include "puke/graph/Node.h"

namespace puke {

// Resizes an image to another format, like Nuke's Reformat.
//
// Parameters:
//   format  (choice, "project")    "project": the render's format
//                                  (RenderContext::format); "custom": `size`
//   size    (vec2, 1920 1080)      width and height when format is "custom"
//   resize  (choice, "width")      how the input's frame fits the new one:
//             none     no scaling
//             width    scale so the widths match
//             height   scale so the heights match
//             fit      the whole input fits inside (scale by the smaller ratio)
//             fill     the input covers the whole frame (the larger ratio)
//             distort  widths and heights both match (scales x and y apart)
//   center  (bool, true)           put the input's centre at the new frame's
//                                  centre; otherwise top-left corners meet
//   filter  (choice, "cubic")      "nearest", "bilinear" or "cubic", as in
//                                  Transform
//
// The output's display window is the new format; its data window is the
// input's, scaled. Like Transform, scaling down by much more than 2x aliases
// (the filter doesn't widen yet).
class ReformatNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Reformat"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    // The new frame, for a render in `ctx`. Throws if the size is invalid.
    Box frame(const RenderContext& ctx) const;
    // Input pixels -> output pixels, for an input whose frame is `inFrame`.
    Affine matrix(const Box& inFrame, const Box& outFrame) const;

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Box inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;
};

}  // namespace puke
