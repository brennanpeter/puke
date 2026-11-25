#include "comp/nodes/RotoNode.h"

#include <algorithm>
#include <cmath>

#include "Specs.h"

namespace comp {

namespace {

struct Edge {
    double x0, y0, x1, y1;
};

double segmentDistance(const Edge& e, double px, double py) {
    const double dx = e.x1 - e.x0, dy = e.y1 - e.y0;
    const double len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((px - e.x0) * dx + (py - e.y0) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(px - (e.x0 + t * dx), py - (e.y0 + t * dy));
}

}  // namespace

std::vector<float> rasterize(const std::vector<std::vector<Path::Point>>& polygons, double feather,
                             const Box& region) {
    std::vector<float> out(static_cast<std::size_t>(std::max(0LL, region.area())), 0.0f);
    if (region.empty()) return out;
    feather = std::max(0.0, feather);
    // Coverage falls from 1 to 0 as the signed distance s (negative inside)
    // goes from -0.5 to feather + 0.5.
    const double reach = feather + 0.5;

    std::vector<Edge> edges;
    for (const auto& poly : polygons)
        for (std::size_t i = 0; i < poly.size(); ++i) {
            const auto& a = poly[i];
            const auto& b = poly[(i + 1) % poly.size()];
            if (a != b) edges.push_back({a[0], a[1], b[0], b[1]});
        }
    // Only edges near the region matter for distances.
    std::vector<const Edge*> near;
    for (const Edge& e : edges)
        if (std::max(e.x0, e.x1) >= region.x0 - reach && std::min(e.x0, e.x1) <= region.x1 + reach &&
            std::max(e.y0, e.y1) >= region.y0 - reach && std::min(e.y0, e.y1) <= region.y1 + reach)
            near.push_back(&e);

    std::vector<std::pair<double, int>> crossings;
    const int w = region.width();
    for (int y = region.y0; y < region.y1; ++y) {
        const double cy = y + 0.5;
        // Where the edges cross this row's centre line, and which way.
        crossings.clear();
        for (const Edge& e : edges) {
            const bool down = e.y0 <= cy && e.y1 > cy, up = e.y1 <= cy && e.y0 > cy;
            if (!down && !up) continue;
            const double t = (cy - e.y0) / (e.y1 - e.y0);
            crossings.push_back({e.x0 + t * (e.x1 - e.x0), down ? 1 : -1});
        }
        std::sort(crossings.begin(), crossings.end());

        float* row = out.data() + static_cast<std::size_t>(y - region.y0) * w;
        std::size_t next = 0;
        int winding = 0;
        for (int x = region.x0; x < region.x1; ++x) {
            const double cx = x + 0.5;
            while (next < crossings.size() && crossings[next].first <= cx) winding += crossings[next++].second;
            const bool inside = winding != 0;
            double d = reach;  // only distances below this change anything
            for (const Edge* e : near) d = std::min(d, segmentDistance(*e, cx, cy));
            const double s = inside ? -d : d;
            const double t = std::clamp((s + 0.5) / (feather + 1), 0.0, 1.0);
            row[x - region.x0] = static_cast<float>(1 - t * t * (3 - 2 * t));  // smoothstep falloff
        }
    }
    return out;
}

std::vector<ParamSpec> RotoNode::paramSpecs() const {
    using namespace specs;
    return {
        make("shape", Kind::Path, std::string(), "The outline: drawn in the viewer, stored as SVG path data"),
        make("feather", Kind::Double, 0.0, "Pixels of soft edge outside the outline", 0, 100),
        make("opacity", Kind::Double, 1.0, "How solid the matte is"),
        make("invert", Kind::Bool, false, "The matte outside the shape instead"),
        choice("output", {"alpha", "rgba"}, "alpha", "alpha: over the bg's alpha; rgba: color over all four"),
        make("color", Kind::Color, Color{1, 1, 1, 1}, "The shape's color, for rgba output"),
    };
}

Path RotoNode::shape() const {
    try {
        return Path::parse(param<std::string>("shape", ""));
    } catch (const Error& e) {
        throw Error(name() + ": " + e.what());
    }
}

namespace {
const ChannelList kRGBA{"R", "G", "B", "A"};
}

ImageInfo RotoNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const {
    const ImageInfo* bg = inputs.empty() ? nullptr : inputs[0];
    ImageInfo info;
    info.displayWindow = bg ? bg->displayWindow : ctx.format.box();
    info.channels = kRGBA;
    if (bg)
        for (const auto& c : bg->channels)
            if (std::find(info.channels.begin(), info.channels.end(), c) == info.channels.end())
                info.channels.push_back(c);

    const std::string output = param<std::string>("output", "alpha");
    if (output != "alpha" && output != "rgba")
        throw Error(name() + ": output must be alpha or rgba, not '" + output + "'");
    const Path p = shape();
    const int pad = static_cast<int>(std::ceil(std::max(0.0, numberParam("feather", 0.0)))) + 1;
    Box matte = p.empty() ? Box{} : p.bounds().padded(pad);
    if (param<bool>("invert", false)) matte = info.displayWindow;  // everything outside the shape too
    info.dataWindow = matte.unite(bg ? bg->dataWindow : Box{});
    return info;
}

Image RotoNode::compute(const RenderContext&, const Box& region, std::span<const Image* const> inputs) const {
    const Image* bg = inputs.empty() ? nullptr : inputs[0];
    ChannelList channels = kRGBA;
    if (bg)
        for (const auto& c : bg->channels())
            if (std::find(channels.begin(), channels.end(), c) == channels.end()) channels.push_back(c);
    Image out(region, channels);

    std::vector<float> m = rasterize(shape().polygons(), numberParam("feather", 0.0), region);
    const float opacity = static_cast<float>(numberParam("opacity", 1.0));
    const bool invert = param<bool>("invert", false);
    for (float& v : m) v = opacity * (invert ? 1 - v : v);

    const bool rgba = param<std::string>("output", "alpha") == "rgba";
    const Color color = colorParam("color", 1.0);
    const int n = static_cast<int>(channels.size());
    std::vector<int> fromBg(n, -1);
    if (bg)
        for (int c = 0; c < n; ++c) fromBg[c] = bg->channelIndex(channels[c]);

    std::size_t i = 0;
    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += n, ++i) {
            // In rgba mode the color's alpha scales the matte, so the RGB
            // stays premultiplied.
            const float a = rgba ? m[i] * static_cast<float>(color[3]) : m[i];
            for (int c = 0; c < n; ++c) {
                const float under = fromBg[c] >= 0 ? bg->at(x, y, fromBg[c]) : 0.0f;
                if (c == 3) px[c] = a + under * (1 - a);  // alpha: over, in both modes
                else if (rgba && c < 3) px[c] = static_cast<float>(color[c]) * a + under * (1 - a);
                else px[c] = under;
            }
        }
    }
    return out;
}

}  // namespace comp
