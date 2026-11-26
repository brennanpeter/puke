#include "comp/nodes/TransformNode.h"

#include "Resample.h"
#include "Specs.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace comp {

int TransformNode::filterRadius() const { return resample::radius(resample::parseFilter(*this)); }

Affine TransformNode::matrix(double scale) const {
    if (scale != 1) return Affine::scale(scale, scale) * matrix(1) * Affine::scale(1 / scale, 1 / scale);
    const Vec2 t = vec2Param("translate", {0, 0});
    const Vec2 s = vec2Param("scale", {1, 1});
    const Vec2 c = vec2Param("center", {0, 0});
    // Negated so positive angles turn counter-clockwise on screen with y pointing down.
    const double radians = -numberParam("rotate", 0.0) * std::numbers::pi / 180.0;

    const Affine m = Affine::translate(t[0], t[1]) * Affine::translate(c[0], c[1]) *
                     Affine::rotate(radians) * Affine::scale(s[0], s[1]) *
                     Affine::translate(-c[0], -c[1]);
    if (!param<bool>("invert", false)) return m;
    if (!m.invertible()) throw Error(name() + ": cannot invert a transform with zero scale");
    return m.inverse();
}

ImageInfo TransformNode::computeInfo(const RenderContext& ctx,
                                     std::span<const ImageInfo* const> inputs) const {
    ImageInfo info = *inputs[0];
    info.dataWindow = resample::mapDataWindow(info.dataWindow, matrix(ctx.scale), resample::parseFilter(*this));
    return info;
}

Box TransformNode::inputRegion(const RenderContext& ctx, int, const Box& outputRegion) const {
    return resample::inputRegion(outputRegion, matrix(ctx.scale), resample::parseFilter(*this));
}

Image TransformNode::compute(const RenderContext& ctx, const Box& region,
                             std::span<const Image* const> inputs) const {
    return resample::apply(*inputs[0], region, matrix(ctx.scale), resample::parseFilter(*this));
}

std::vector<ParamSpec> TransformNode::paramSpecs() const {
    using namespace specs;
    return {
        make("translate", Kind::Vec2, Vec2{0, 0}, "Pixels; +y moves down", -1000, 1000),
        make("rotate", Kind::Double, 0.0, "Degrees, counter-clockwise", -180, 180),
        make("scale", Kind::Vec2, Vec2{1, 1}, "Around the center", 0, 4),
        make("center", Kind::Vec2, Vec2{0, 0}, "Pivot for rotate and scale, in pixels", 0, 2000),
        choice("filter", {"nearest", "bilinear", "cubic"}, "cubic", "Resampling filter"),
        make("invert", Kind::Bool, false, "Apply the inverse transform"),
    };
}

}  // namespace comp
