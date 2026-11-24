#include "comp/nodes/TransformNode.h"

#include "Specs.h"

#include <cmath>
#include <numbers>
#include <vector>

namespace comp {
namespace {

enum class Filter { Nearest, Bilinear, Cubic };

Filter parseFilter(const Node& node) {
    const auto s = node.param<std::string>("filter", "cubic");
    if (s == "nearest") return Filter::Nearest;
    if (s == "bilinear") return Filter::Bilinear;
    if (s == "cubic") return Filter::Cubic;
    throw Error(node.name() + ": filter must be nearest, bilinear or cubic, not '" + s + "'");
}

// Filter taps along one axis for a sample at continuous coordinate `p`
// (pixel i's centre is at i + 0.5).
struct Taps {
    int first = 0;
    int count = 0;
    float w[4] = {};
};

Taps taps(Filter filter, double p) {
    Taps t;
    const double s = p - 0.5;  // position in "pixel index" space
    switch (filter) {
        case Filter::Nearest:
            t.first = static_cast<int>(std::floor(p));
            t.count = 1;
            t.w[0] = 1;
            break;
        case Filter::Bilinear: {
            const double i = std::floor(s);
            const float f = static_cast<float>(s - i);
            t.first = static_cast<int>(i);
            t.count = 2;
            t.w[0] = 1 - f;
            t.w[1] = f;
            break;
        }
        case Filter::Cubic: {  // Catmull-Rom
            const double i = std::floor(s);
            const float f = static_cast<float>(s - i), f2 = f * f, f3 = f2 * f;
            t.first = static_cast<int>(i) - 1;
            t.count = 4;
            t.w[0] = 0.5f * (-f3 + 2 * f2 - f);
            t.w[1] = 0.5f * (3 * f3 - 5 * f2 + 2);
            t.w[2] = 0.5f * (-3 * f3 + 4 * f2 + f);
            t.w[3] = 0.5f * (f3 - f2);
            break;
        }
    }
    return t;
}

}  // namespace

int TransformNode::filterRadius() const {
    switch (parseFilter(*this)) {
        case Filter::Nearest: return 0;
        case Filter::Bilinear: return 1;
        case Filter::Cubic: return 2;
    }
    return 2;
}

Affine TransformNode::matrix() const {
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

ImageInfo TransformNode::computeInfo(const RenderContext&,
                                     std::span<const ImageInfo* const> inputs) const {
    ImageInfo info = *inputs[0];
    const Affine m = matrix();
    if (!m.invertible() || info.dataWindow.empty()) {
        info.dataWindow = {};  // collapsed to nothing
        return info;
    }
    // The filter spreads each input pixel a little, so grow before mapping.
    info.dataWindow = m.mapBounds(info.dataWindow.padded(filterRadius()));
    return info;
}

Box TransformNode::inputRegion(const RenderContext&, int, const Box& outputRegion) const {
    const Affine m = matrix();
    if (!m.invertible()) return {};
    // +1 guards against rounding at the edges.
    return m.inverse().mapBounds(outputRegion).padded(filterRadius() + 1);
}

Image TransformNode::compute(const RenderContext&, const Box& region,
                             std::span<const Image* const> inputs) const {
    const Image& src = *inputs[0];
    Image out(region, src.channels());
    const Affine m = matrix();
    if (!m.invertible()) return out;
    if (m.isIdentity()) return cropped(src, region);

    const Affine inv = m.inverse();
    const Filter filter = parseFilter(*this);
    const int n = src.numChannels();
    std::vector<float> acc(n);

    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += n) {
            const auto [u, v] = inv.apply(x + 0.5, y + 0.5);
            const Taps tx = taps(filter, u);
            const Taps ty = taps(filter, v);

            std::fill(acc.begin(), acc.end(), 0.0f);
            for (int j = 0; j < ty.count; ++j) {
                const int sy = ty.first + j;
                for (int i = 0; i < tx.count; ++i) {
                    const float w = tx.w[i] * ty.w[j];
                    if (w == 0.0f) continue;
                    const int sx = tx.first + i;
                    for (int c = 0; c < n; ++c) acc[c] += w * src.at(sx, sy, c);
                }
            }
            std::copy(acc.begin(), acc.end(), px);
        }
    }
    return out;
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
