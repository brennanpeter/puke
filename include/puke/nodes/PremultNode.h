#pragma once

#include "puke/graph/Node.h"

namespace puke {

// Premult multiplies R, G and B by A; Unpremult divides them by it (leaving
// pixels whose alpha is 0 alone). Other channels, and images without an A
// channel, pass through unchanged. No parameters.
class PremultNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Premult"; }
    int maxInputs() const override { return 1; }
    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const> inputs) const override {
        return *inputs[0];
    }
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;

protected:
    virtual bool divide() const { return false; }
};

class UnpremultNode : public PremultNode {
public:
    using PremultNode::PremultNode;
    const char* typeName() const override { return "Unpremult"; }

protected:
    bool divide() const override { return true; }
};

}  // namespace puke
