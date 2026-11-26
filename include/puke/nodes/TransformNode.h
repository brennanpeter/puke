#pragma once

#include "puke/core/Affine.h"
#include "puke/graph/Node.h"

namespace puke {

// 2D affine transform: translate, rotate and scale around a centre point.
//
// Parameters:
//   translate  (Vec2, {0,0})    pixels; +y moves down (y points down, as in EXR files)
//   rotate     (number, 0)      degrees; positive turns counter-clockwise on screen, as in Nuke
//   scale      (Vec2 or number, 1)
//   center     (Vec2, {0,0})    pivot for rotate and scale, in pixels. Like Nuke, this
//                               defaults to the origin; a GUI should set it to the
//                               middle of the format when the node is created.
//   filter     (string, "cubic") "nearest", "bilinear" or "cubic" (Catmull-Rom)
//   invert     (bool, false)    apply the inverse transform
//
// Pixels are resampled by inverse mapping: each output pixel centre is mapped
// back into the input and filtered there. Integer translations are exact with
// every filter.
//
// TODO: downscaling more than ~2x aliases, because the filter doesn't widen
// with the scale yet (needs filter footprints or mipmaps). Motion blur.
class TransformNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Transform"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    // Input pixel space -> output pixel space, at `scale` (a proxy render:
    // translate and center are in full-size pixels).
    Affine matrix(double scale = 1) const;

    ImageInfo computeInfo(const RenderContext& ctx,
                          std::span<const ImageInfo* const> inputs) const override;
    Box inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const override;
    Image compute(const RenderContext& ctx, const Box& region,
                  std::span<const Image* const> inputs) const override;

private:
    // How many input pixels the filter reaches beyond the sample point.
    int filterRadius() const;
};

}  // namespace puke
