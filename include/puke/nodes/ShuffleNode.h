#pragma once

#include <array>

#include "puke/graph/Node.h"

namespace puke {

// Rearranges channels: each of the output's R, G, B and A comes from one of
// the input's channels, a constant, or a channel of an optional second input
// (to copy an alpha across, say).
//
// Inputs: 0 the image (required), 1 a second image (optional, labelled "2").
// Parameters, each a choice of
//   R G B A        that channel of input 0 (zero if it hasn't got it)
//   0 1            a constant
//   2:R 2:G 2:B 2:A  that channel of input 2
//     red    (default R)
//     green  (default G)
//     blue   (default B)
//     alpha  (default A)
//
// The output always has R, G, B and A; any other channels of input 0 pass
// through. The frame is input 0's; the data window is input 0's, joined by
// input 2's when a channel is taken from it.
class ShuffleNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Shuffle"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 2; }
    bool inputOptional(int index) const override { return index == 1; }
    std::string inputLabel(int index) const override { return index == 1 ? "2" : ""; }

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Box inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;

private:
    // The four sources (red, green, blue, alpha), checked.
    std::array<std::string, 4> sources() const;
    bool usesSecond() const;
};

}  // namespace puke
