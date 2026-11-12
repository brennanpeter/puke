#pragma once

#include <string>

#include "comp/graph/Node.h"

namespace comp {

// Reads an image or image sequence from disk via OpenImageIO.
//
// Parameters:
//   file      (string, required)  path, may contain a frame token: plate.####.exr
//   first     (int, optional)     first frame of the sequence
//   last      (int, optional)     last frame of the sequence
//   outside   (string, "hold")    frames outside [first, last]: "hold", "black" or "error"
//
// Pixels are returned as float32, in whatever color space the file is in.
// (Color management via OCIO comes in a later milestone.)
class ReadNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Read"; }
    int maxInputs() const override { return 0; }

    ImageInfo computeInfo(const RenderContext& ctx,
                          std::span<const ImageInfo* const> inputs) const override;
    Image compute(const RenderContext& ctx, const Box& region,
                  std::span<const Image* const> inputs) const override;

    // Every compute() opens the file and reads whole rows, so tiling would
    // repeat that work per tile. TODO: allow tiles once there's a file cache.
    bool supportsTiles() const override { return false; }

    // The file path that would be read for this context, or "" if the frame
    // is outside the range and `outside` is "black".
    std::string resolvePath(const RenderContext& ctx) const;

private:
    ImageInfo fileInfo(const RenderContext& ctx) const;
};

}  // namespace comp
