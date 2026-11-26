#pragma once

#include <vector>

#include "puke/graph/Node.h"

namespace puke {

// Blurs every channel.
//
// Parameters:
//   size    (vec2, 0 0)          how wide the blur is, in pixels, across and
//                                down (one number for both); 0 leaves that
//                                direction alone
//   filter  (choice, "gaussian") "gaussian": the kernel reaches size/2 each
//                                way, 3 standard deviations; "box": an even
//                                average over `size` pixels (partly
//                                weighting the end pixels for fractional
//                                sizes)
//
// Separable (across, then down). The data window grows by the reach of the
// kernel; outside it the image is black, so edges darken as in Nuke.
class BlurNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Blur"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    // Normalized weights for offsets -r..r along one axis (size 1: no blur).
    std::vector<float> kernel(double size) const;

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Box inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;

private:
    Vec2 size(double scale) const;  // checked, in pixels at `scale`
};

}  // namespace puke
