#pragma once

#include <string>

#include "puke/graph/Node.h"

namespace puke {

// Reads an image or image sequence from disk via OpenImageIO.
//
// Parameters:
//   file      (string, required)  path, may contain a frame token: plate.####.exr
//   first     (int, optional)     first frame of the sequence
//   last      (int, optional)     last frame of the sequence
//   outside   (string, "hold")    frames outside [first, last]: "hold", "black" or "error"
//   colorspace (string, "auto")   the file's color space (or an OCIO role). "auto":
//                                 float and half files are taken to be in the working
//                                 space already; integer files (8-, 16-bit) are sRGB,
//                                 via the config's "matte_paint" role.
//
// Pixels are returned as float32, converted into the working space (the
// config's scene_linear role; see ColorConfig).
class ReadNode : public Node {
public:
    using Node::Node;

    const char* typeName() const override { return "Read"; }
    std::vector<ParamSpec> paramSpecs() const override;
    int maxInputs() const override { return 0; }

    ImageInfo computeInfo(const RenderContext& ctx,
                          std::span<const ImageInfo* const> inputs) const override;
    // Full-size pixels of `region`, in the working space.
    Image readRegion(const RenderContext& ctx, const Box& region) const;
    Image compute(const RenderContext& ctx, const Box& region,
                  std::span<const Image* const> inputs) const override;

    // Every compute() opens the file and reads whole rows, so tiling would
    // repeat that work per tile. TODO: allow tiles once there's a file cache.
    bool supportsTiles() const override { return false; }

    // The file's size and modification time, so cached tiles are dropped
    // when the file on disk changes; and the color config.
    void appendHash(Hasher& h, const RenderContext& ctx) const override;

    // The file path that would be read for this context, or "" if the frame
    // is outside the range and `outside` is "black".
    std::string resolvePath(const RenderContext& ctx) const;

private:
    ImageInfo fileInfo(const RenderContext& ctx) const;
};

}  // namespace puke
