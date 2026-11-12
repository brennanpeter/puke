#pragma once

#include <algorithm>

namespace comp {

// Half-open integer pixel region: [x0, x1) x [y0, y1).
// Y increases downward, matching OpenImageIO and EXR data windows.
struct Box {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    constexpr int width() const { return std::max(0, x1 - x0); }
    constexpr int height() const { return std::max(0, y1 - y0); }
    constexpr bool empty() const { return width() == 0 || height() == 0; }
    constexpr long long area() const { return static_cast<long long>(width()) * height(); }

    constexpr bool contains(int x, int y) const {
        return x >= x0 && x < x1 && y >= y0 && y < y1;
    }

    constexpr Box intersect(const Box& o) const {
        Box b{std::max(x0, o.x0), std::max(y0, o.y0), std::min(x1, o.x1), std::min(y1, o.y1)};
        return b.empty() ? Box{} : b;
    }

    constexpr Box unite(const Box& o) const {
        if (empty()) return o;
        if (o.empty()) return *this;
        return {std::min(x0, o.x0), std::min(y0, o.y0), std::max(x1, o.x1), std::max(y1, o.y1)};
    }

    constexpr Box padded(int p) const { return {x0 - p, y0 - p, x1 + p, y1 + p}; }

    friend constexpr bool operator==(const Box&, const Box&) = default;
};

}  // namespace comp
