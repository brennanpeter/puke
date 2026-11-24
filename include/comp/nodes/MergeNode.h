#pragma once

#include "comp/graph/Node.h"

namespace comp {

// Combines two images, with Nuke's input order and operations.
//
// Inputs:
//   0  B  background (required)
//   1  A  foreground (optional; if unconnected, B passes through unchanged)
//
// Parameters:
//   operation  (string, "over")   see below
//   bbox       (string, "union")  output data window: "union", "intersection", "A" or "B"
//   mix        (number, 1)        blend between B (0) and the result (1)
//
// Operations (a, b are the A and B alphas; images are assumed premultiplied):
//   over        A + B(1-a)          under       A(1-b) + B
//   plus        A + B               from        B - A
//   minus       A - B               multiply    A * B
//   screen      A + B - A*B         difference  |A - B|
//   max         max(A, B)           min         min(A, B)
//   average     (A + B) / 2         copy        A
//   in          A * b               out         A(1-b)
//   atop        A*b + B(1-a)        xor         A(1-b) + B(1-a)
//   mask        B * a               stencil     B(1-a)
//
// The formula applies to every channel, alpha included. Output channels are
// B's, followed by any channels only A has. A channel missing from one input
// reads as zero there; an input with no alpha is treated as alpha 0.
class MergeNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Merge"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 2; }
    bool inputOptional(int index) const override { return index == 1; }
    std::string inputLabel(int index) const override { return index == 0 ? "B" : "A"; }

    ImageInfo computeInfo(const RenderContext& ctx,
                          std::span<const ImageInfo* const> inputs) const override;
    Image compute(const RenderContext& ctx, const Box& region,
                  std::span<const Image* const> inputs) const override;
};

}  // namespace comp
