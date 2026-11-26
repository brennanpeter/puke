#include "comp/nodes/CropNode.h"

#include <algorithm>
#include <cmath>

#include "Specs.h"

namespace comp {

std::vector<ParamSpec> CropNode::paramSpecs() const {
    using namespace specs;
    return {
        make("box_min", Kind::Vec2, Vec2{0, 0}, "Top-left corner, in pixels", 0, 4096),
        make("box_max", Kind::Vec2, Vec2{1920, 1080}, "Bottom-right corner, in pixels", 0, 4096),
        make("reformat", Kind::Bool, false, "Make the box the new frame, moved to the origin"),
    };
}

Box CropNode::box(double scale) const {
    const Vec2 a = vec2Param("box_min", {0, 0}), b = vec2Param("box_max", {1920, 1080});
    auto px = [](double v) { return static_cast<int>(std::lround(std::clamp(v, -1e8, 1e8))); };
    const Box box{px(a[0]), px(a[1]), px(b[0]), px(b[1])};
    if (box.empty()) throw Error(name() + ": the box is empty (box_max must be right of and below box_min)");
    return box.scaled(scale);
}

namespace {
// Where the box's pixels go: to the origin if reformatting.
Box shifted(const Box& b, int dx, int dy) { return {b.x0 + dx, b.y0 + dy, b.x1 + dx, b.y1 + dy}; }
}  // namespace

ImageInfo CropNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const {
    ImageInfo info = *inputs[0];
    const Box b = box(ctx.scale);
    info.dataWindow = info.dataWindow.intersect(b);
    if (param<bool>("reformat", false)) {
        info.dataWindow = info.dataWindow.empty() ? Box{} : shifted(info.dataWindow, -b.x0, -b.y0);
        info.displayWindow = {0, 0, b.width(), b.height()};
    }
    return info;
}

Box CropNode::inputRegion(const RenderContext& ctx, int, const Box& outputRegion) const {
    const Box b = box(ctx.scale);
    const Box in = param<bool>("reformat", false) ? shifted(outputRegion, b.x0, b.y0) : outputRegion;
    return in.intersect(b);
}

Image CropNode::compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const {
    const Image& src = *inputs[0];
    const Box b = box(ctx.scale);
    const bool reformat = param<bool>("reformat", false);
    const int dx = reformat ? b.x0 : 0, dy = reformat ? b.y0 : 0;  // output -> input
    Image out(region, src.channels());
    const int n = src.numChannels();
    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += n) {
            const int sx = x + dx, sy = y + dy;
            if (!b.contains(sx, sy)) continue;  // already zero
            for (int c = 0; c < n; ++c) px[c] = src.at(sx, sy, c);
        }
    }
    return out;
}

}  // namespace comp
