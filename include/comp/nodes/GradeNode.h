#pragma once

#include "comp/graph/Node.h"

namespace comp {

// Colour correction with Nuke's Grade formula.
//
// Parameters (each a number, or a Color for per-channel r, g, b, a values):
//   blackpoint  (0)  input value that maps to `lift`
//   whitepoint  (1)  input value that maps to `gain`
//   lift        (0)
//   gain        (1)
//   multiply    (1)
//   offset      (0)
//   gamma       (1)  applied last, only to positive values; must be > 0
// Other parameters:
//   channels    (string, "rgb")  "rgb" or "rgba"
//   black_clamp (bool, true)     clamp results below 0
//   white_clamp (bool, false)    clamp results above 1
//   mix         (number, 1)      blend between input (0) and result (1)
//
//   A = multiply * (gain - lift) / (whitepoint - blackpoint)
//   B = offset + lift - A * blackpoint
//   out = pow(A * in + B, 1 / gamma)
//
// As in Nuke, the data window is left unchanged: lifting black does not turn
// the empty area outside the data window grey.
class GradeNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Grade"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const> inputs) const override {
        return *inputs[0];
    }
    Image compute(const RenderContext& ctx, const Box& region,
                  std::span<const Image* const> inputs) const override;
};

}  // namespace comp
