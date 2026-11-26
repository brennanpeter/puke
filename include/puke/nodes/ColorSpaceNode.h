#pragma once

#include "puke/graph/Node.h"

namespace puke {

// Converts its input between two color spaces (like Nuke's OCIOColorSpace).
//
// Parameters:
//   in   (string, "scene_linear")  color space (or OCIO role) the input is in
//   out  (string, "scene_linear")  color space (or OCIO role) to convert to
//
// Converts R, G and B (or a lone Y), treating color as premultiplied when
// there's an alpha channel; other channels pass through. Only pixels inside
// the input's data window are converted, so the data window is unchanged.
class ColorSpaceNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "ColorSpace"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const> inputs) const override {
        return *inputs[0];
    }
    Image compute(const RenderContext& ctx, const Box& region,
                  std::span<const Image* const> inputs) const override;

    // The color config, so cached tiles are dropped when it changes.
    void appendHash(Hasher& h, const RenderContext& ctx) const override;
};

}  // namespace puke
