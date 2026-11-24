#pragma once

#include <string>

#include "comp/engine/Engine.h"
#include "comp/graph/Node.h"

namespace comp {

// Writes its input to disk via OpenImageIO.
//
// Parameters:
//   file         (string, required)  output path, may contain a frame token: out.####.exr
//   datatype     (string, "half")    "float", "half", "uint8" or "uint16"
//   compression  (string, optional)  passed to the writer, e.g. "zip", "piz", "dwaa" for EXR
//   colorspace   (string, "auto")    color space (or OCIO role) to write in. "auto": float
//                                    and half files stay in the working space; integer
//                                    files (8-, 16-bit) get sRGB, via the "matte_paint" role.
//                                    Decided by what the format actually writes: PNG asked
//                                    for "half" writes integers, so gets sRGB.
//
// Files are written to "<path>.partial" and renamed into place when complete,
// so an interrupted render never leaves a truncated frame behind.
class WriteNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Write"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 1; }

    // Pass-through: a Write node can sit in the middle of a graph.
    ImageInfo computeInfo(const RenderContext&,
                          std::span<const ImageInfo* const> inputs) const override {
        return *inputs[0];
    }
    Image compute(const RenderContext&, const Box& region,
                  std::span<const Image* const> inputs) const override {
        return cropped(*inputs[0], region);
    }

    // Renders the input's full data window for this frame and writes the file.
    // Returns the path that was written.
    std::string execute(Engine& engine, const RenderContext& ctx) const;
};

}  // namespace comp
