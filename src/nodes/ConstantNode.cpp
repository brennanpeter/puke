#include "comp/nodes/ConstantNode.h"

#include <algorithm>
#include <cmath>

#include "Specs.h"

namespace comp {

std::vector<ParamSpec> ConstantNode::paramSpecs() const {
    using namespace specs;
    return {
        make("color", Kind::Color, Color{0, 0, 0, 0}, "The color of every pixel"),
        choice("format", {"project", "custom"}, "project", "The project's format, or a size of its own"),
        make("size", Kind::Vec2, Vec2{1920, 1080}, "Width and height, when format is custom", 1, 8192),
    };
}

ImageInfo ConstantNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const>) const {
    const std::string format = param<std::string>("format", "project");
    Box frame;
    if (format == "project") {
        frame = ctx.format.box();
    } else if (format == "custom") {
        const Vec2 size = vec2Param("size", Vec2{1920, 1080});
        for (double v : size)
            if (!(v >= 1 && v <= 65536 && v == std::floor(v)))
                throw Error(name() + ": size must be whole numbers from 1 to 65536");
        frame = {0, 0, static_cast<int>(size[0]), static_cast<int>(size[1])};
    } else {
        throw Error(name() + ": unknown format '" + format + "' (use project or custom)");
    }
    return {frame, frame, {"R", "G", "B", "A"}};
}

Image ConstantNode::compute(const RenderContext&, const Box& region, std::span<const Image* const>) const {
    const Color c = colorParam("color", 0.0);
    Image img(region, {"R", "G", "B", "A"});
    float* p = img.data();
    const std::size_t n = static_cast<std::size_t>(region.area());
    for (std::size_t i = 0; i < n; ++i, p += 4)
        for (int ch = 0; ch < 4; ++ch) p[ch] = static_cast<float>(c[ch]);
    return img;
}

}  // namespace comp
