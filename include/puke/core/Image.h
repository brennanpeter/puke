#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "puke/core/Box.h"

namespace puke {

// Ordered list of named channels, e.g. {"R","G","B","A"} or {"Z"} or
// {"diffuse.R","diffuse.G","diffuse.B"}. Names follow EXR conventions.
using ChannelList = std::vector<std::string>;

// What a node will produce, without computing any pixels.
struct ImageInfo {
    Box dataWindow;     // where pixels actually exist
    Box displayWindow;  // the nominal frame (format)
    ChannelList channels;
};

// A block of float32 pixels covering `region`, interleaved by channel.
// Engine-internal processing is always float32; storage types (half, 8-bit)
// only exist at Read/Write boundaries.
class Image {
public:
    Image() = default;
    Image(Box region, ChannelList channels)
        : region_(region),
          channels_(std::move(channels)),
          pixels_(static_cast<std::size_t>(region.area()) * channels_.size(), 0.0f) {}

    const Box& region() const { return region_; }
    const ChannelList& channels() const { return channels_; }
    int numChannels() const { return static_cast<int>(channels_.size()); }

    // Index of a channel by name, or -1 if absent.
    int channelIndex(const std::string& name) const {
        for (int i = 0; i < numChannels(); ++i)
            if (channels_[i] == name) return i;
        return -1;
    }

    // Channel `c` of pixel (x, y), or 0 outside region(). Use this to read
    // input images, which may not cover everything you ask for.
    float at(int x, int y, int c) const {
        return region_.contains(x, y) ? pixels_[offset(x, y) + c] : 0.0f;
    }

    float* data() { return pixels_.data(); }
    const float* data() const { return pixels_.data(); }

    // Pointer to the first channel of pixel (x, y), in absolute pixel coords.
    float* pixel(int x, int y) { return pixels_.data() + offset(x, y); }
    const float* pixel(int x, int y) const { return pixels_.data() + offset(x, y); }

    std::size_t rowStride() const {  // floats per row
        return static_cast<std::size_t>(region_.width()) * channels_.size();
    }

private:
    std::size_t offset(int x, int y) const {
        return (static_cast<std::size_t>(y - region_.y0) * region_.width() + (x - region_.x0)) *
               channels_.size();
    }

    Box region_;
    ChannelList channels_;
    std::vector<float> pixels_;
};

// Copy of `src` covering exactly `region`. Pixels outside src.region() are zero.
inline Image cropped(const Image& src, const Box& region) {
    Image out(region, src.channels());
    const Box overlap = region.intersect(src.region());
    if (overlap.empty()) return out;
    const std::size_t bytes = static_cast<std::size_t>(overlap.width()) * src.numChannels() * sizeof(float);
    for (int y = overlap.y0; y < overlap.y1; ++y)
        std::memcpy(out.pixel(overlap.x0, y), src.pixel(overlap.x0, y), bytes);
    return out;
}

// `src` (full size) shrunk to `scale` times the size, for `region` (in
// scaled pixels): each output pixel is the average of the source area it
// covers, partial pixels weighted by how much of them it covers. Source
// pixels outside src.region() count as zero.
Image downscale(const Image& src, const Box& region, double scale);

}  // namespace puke
