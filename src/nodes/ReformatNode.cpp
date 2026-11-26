#include "comp/nodes/ReformatNode.h"

#include <algorithm>
#include <cmath>

#include "Resample.h"
#include "Specs.h"

namespace comp {

std::vector<ParamSpec> ReformatNode::paramSpecs() const {
    using namespace specs;
    return {
        choice("format", {"project", "custom"}, "project", "The project's format, or a size of its own"),
        make("size", Kind::Vec2, Vec2{1920, 1080}, "Width and height, when format is custom", 1, 8192),
        choice("resize", {"none", "width", "height", "fit", "fill", "distort"}, "width",
               "How the input's frame fits the new one"),
        make("center", Kind::Bool, true, "Centre the input in the new frame (otherwise top-left corners meet)"),
        choice("filter", {"nearest", "bilinear", "cubic"}, "cubic", "Resampling filter"),
    };
}

Box ReformatNode::frame(const RenderContext& ctx) const {
    const std::string format = param<std::string>("format", "project");
    if (format == "project") return ctx.frameBox();
    if (format != "custom") throw Error(name() + ": unknown format '" + format + "' (use project or custom)");
    const Vec2 size = vec2Param("size", Vec2{1920, 1080});
    for (double v : size)
        if (!(v >= 1 && v <= 65536 && v == std::floor(v)))
            throw Error(name() + ": size must be whole numbers from 1 to 65536");
    return Box{0, 0, static_cast<int>(size[0]), static_cast<int>(size[1])}.scaled(ctx.scale);
}

Affine ReformatNode::matrix(const Box& in, const Box& out) const {
    if (in.empty()) return Affine::scale(0, 0);  // nothing to scale: collapses (empty output)
    const double rx = double(out.width()) / in.width(), ry = double(out.height()) / in.height();
    const std::string resize = param<std::string>("resize", "width");
    double sx, sy;
    if (resize == "none") sx = sy = 1;
    else if (resize == "width") sx = sy = rx;
    else if (resize == "height") sx = sy = ry;
    else if (resize == "fit") sx = sy = std::min(rx, ry);
    else if (resize == "fill") sx = sy = std::max(rx, ry);
    else if (resize == "distort") sx = rx, sy = ry;
    else throw Error(name() + ": resize must be none, width, height, fit, fill or distort, not '" + resize + "'");

    if (param<bool>("center", true)) {  // centre to centre
        const double icx = (in.x0 + in.x1) / 2.0, icy = (in.y0 + in.y1) / 2.0;
        const double ocx = (out.x0 + out.x1) / 2.0, ocy = (out.y0 + out.y1) / 2.0;
        return Affine::translate(ocx, ocy) * Affine::scale(sx, sy) * Affine::translate(-icx, -icy);
    }
    return Affine::translate(out.x0, out.y0) * Affine::scale(sx, sy) * Affine::translate(-in.x0, -in.y0);
}

ImageInfo ReformatNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const {
    ImageInfo info = *inputs[0];
    const Box out = frame(ctx);
    info.dataWindow = resample::mapDataWindow(info.dataWindow, matrix(info.displayWindow, out),
                                              resample::parseFilter(*this));
    info.displayWindow = out;
    return info;
}

Box ReformatNode::inputRegion(const RenderContext& ctx, int, const Box& outputRegion) const {
    return resample::inputRegion(outputRegion, matrix(inputInfo(0).displayWindow, frame(ctx)),
                                 resample::parseFilter(*this));
}

Image ReformatNode::compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const {
    return resample::apply(*inputs[0], region, matrix(inputInfo(0).displayWindow, frame(ctx)),
                           resample::parseFilter(*this));
}

}  // namespace comp
