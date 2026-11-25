#pragma once

#include "comp/core/Path.h"
#include "comp/graph/Node.h"

namespace comp {

// A shape drawn as a matte (like Nuke's Roto, one shape per node: merge
// several Rotos for shapes with different settings).
//
// Input 0 (optional, "bg"): an image to draw over. Without it the frame is
// the project format.
//
// Parameters:
//   shape    (path, "")          the outline, as SVG path data (see Path.h);
//                                curves that overlap fill once (nonzero)
//   feather  (number, 0)         pixels of soft edge outside the outline
//   opacity  (number, 1)
//   invert   (bool, false)       the matte outside the shape instead
//   output   (choice, "alpha")   "alpha": the matte goes over the bg's alpha,
//                                its other channels pass through; "rgba":
//                                `color` (premultiplied by the matte times
//                                its own alpha) goes over R, G, B and A
//   color    (color, 1 1 1 1)    for output "rgba"
//
// Edges are antialiased: coverage falls off over feather + 1 pixels,
// centred half a pixel inside the outline.
class RotoNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Roto"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }
    bool inputOptional(int) const override { return true; }
    std::string inputLabel(int) const override { return "bg"; }

    Path shape() const;  // parsed; throws with the node's name

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;
};

// The coverage of `polygons` (closed, nonzero fill) at each pixel of
// `region`, row by row: 1 inside, falling to 0 over feather + 1 pixels
// around the edge. Exposed for tests.
std::vector<float> rasterize(const std::vector<std::vector<Path::Point>>& polygons, double feather,
                             const Box& region);

}  // namespace comp
