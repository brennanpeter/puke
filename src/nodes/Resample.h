#pragma once

// Resampling through an affine map, shared by Transform and Reformat.
// Internal to the nodes library.

#include <algorithm>
#include <cmath>
#include <vector>

#include "puke/core/Affine.h"
#include "puke/graph/Node.h"

namespace puke::resample {

enum class Filter { Nearest, Bilinear, Cubic };

inline Filter parseFilter(const Node& node) {
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

inline Taps taps(Filter filter, double p) {
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


// How far the filter reaches from a sample, in pixels.
inline int radius(Filter f) {
    switch (f) {
        case Filter::Nearest: return 0;
        case Filter::Bilinear: return 1;
        case Filter::Cubic: return 2;
    }
    return 2;
}

// The data window of `in` mapped by `m`; empty if `m` collapses it.
inline Box mapDataWindow(const Box& in, const Affine& m, Filter f) {
    if (!m.invertible() || in.empty()) return {};
    if (m.isIdentity()) return in;  // passed through untouched (see apply())
    // The filter spreads each input pixel a little, so grow before mapping.
    return m.mapBounds(in.padded(radius(f)));
}

// The input region needed for `out` under `m`.
inline Box inputRegion(const Box& out, const Affine& m, Filter f) {
    if (!m.invertible()) return {};
    // +1 guards against rounding at the edges.
    return m.inverse().mapBounds(out).padded(radius(f) + 1);
}

// `region` of `src` mapped by `m`: each output pixel centre is mapped back
// into the input and filtered there.
inline Image apply(const Image& src, const Box& region, const Affine& m, Filter filter) {
    Image out(region, src.channels());
    if (!m.invertible()) return out;
    if (m.isIdentity()) return cropped(src, region);

    const Affine inv = m.inverse();
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

}  // namespace puke::resample
