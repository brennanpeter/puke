#include "puke/nodes/BlurNode.h"

#include <algorithm>
#include <cmath>

#include "Specs.h"

namespace puke {

std::vector<ParamSpec> BlurNode::paramSpecs() const {
    using namespace specs;
    return {
        make("size", Kind::Vec2, Vec2{0, 0}, "Width of the blur in pixels, across and down", 0, 100),
        choice("filter", {"gaussian", "box"}, "gaussian", "Shape of the blur"),
    };
}

Vec2 BlurNode::size(double scale) const {
    const Vec2 s = vec2Param("size", {0, 0});
    for (double v : s)
        if (!(v >= 0 && v <= 10000)) throw Error(name() + ": size must be from 0 to 10000");
    return {s[0] * scale, s[1] * scale};
}

std::vector<float> BlurNode::kernel(double size) const {
    const std::string filter = param<std::string>("filter", "gaussian");
    std::vector<double> w;
    if (filter == "gaussian") {
        const double sigma = size / 6;
        if (sigma < 0.05) return {1.0f};  // narrower than a pixel can show
        const int r = static_cast<int>(std::ceil(size / 2));
        for (int k = -r; k <= r; ++k) w.push_back(std::exp(-k * k / (2 * sigma * sigma)));
    } else if (filter == "box") {
        if (size <= 1) return {1.0f};
        // How much of pixel k (covering k-0.5..k+0.5) is inside -size/2..size/2.
        const double h = size / 2;
        const int r = static_cast<int>(std::ceil(h - 0.5));
        for (int k = -r; k <= r; ++k) w.push_back(std::max(0.0, std::min(k + 0.5, h) - std::max(k - 0.5, -h)));
    } else {
        throw Error(name() + ": filter must be gaussian or box, not '" + filter + "'");
    }
    double sum = 0;
    for (double v : w) sum += v;
    std::vector<float> out;
    for (double v : w) out.push_back(static_cast<float>(v / sum));
    return out;
}

namespace {
Box grown(const Box& b, int rx, int ry) { return b.empty() ? b : Box{b.x0 - rx, b.y0 - ry, b.x1 + rx, b.y1 + ry}; }
}  // namespace

ImageInfo BlurNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const {
    ImageInfo info = *inputs[0];
    const Vec2 s = size(ctx.scale);
    const int rx = static_cast<int>(kernel(s[0]).size() / 2), ry = static_cast<int>(kernel(s[1]).size() / 2);
    info.dataWindow = grown(info.dataWindow, rx, ry);
    return info;
}

Box BlurNode::inputRegion(const RenderContext& ctx, int, const Box& outputRegion) const {
    const Vec2 s = size(ctx.scale);
    return grown(outputRegion, static_cast<int>(kernel(s[0]).size() / 2), static_cast<int>(kernel(s[1]).size() / 2));
}

Image BlurNode::compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const {
    const Image& src = *inputs[0];
    const Vec2 s = size(ctx.scale);
    const std::vector<float> kx = kernel(s[0]), ky = kernel(s[1]);
    const int rx = static_cast<int>(kx.size() / 2), ry = static_cast<int>(ky.size() / 2);
    const int n = src.numChannels();

    // Across, into rows reaching ry above and below the region...
    const Box mid{region.x0, region.y0 - ry, region.x1, region.y1 + ry};
    Image across(mid, src.channels());
    for (int y = mid.y0; y < mid.y1; ++y) {
        float* px = across.pixel(mid.x0, y);
        for (int x = mid.x0; x < mid.x1; ++x, px += n)
            for (int k = -rx; k <= rx; ++k) {
                const float w = kx[k + rx];
                for (int c = 0; c < n; ++c) px[c] += w * src.at(x + k, y, c);
            }
    }
    // ...then down.
    Image out(region, src.channels());
    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += n)
            for (int k = -ry; k <= ry; ++k) {
                const float w = ky[k + ry];
                const float* in = across.pixel(x, y + k);
                for (int c = 0; c < n; ++c) px[c] += w * in[c];
            }
    }
    return out;
}

}  // namespace puke
