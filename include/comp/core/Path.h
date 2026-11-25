#pragma once

#include <array>
#include <string>
#include <vector>

#include "comp/core/Box.h"

namespace comp {

// A shape's outline: one or more closed curves, each a loop of points joined
// by cubic Bezier segments. Written and read as SVG path data, a subset of
// it: absolute M, L, C and Z, numbers separated by spaces or commas.
//
//   "M 10 10 L 90 10 C 100 40 100 60 90 90 L 10 90 Z"
//
// Every curve is closed, Z or not. Coordinates are pixels, y down, as in
// comp's images. A Roto node's "shape" parameter holds one of these.
class Path {
public:
    using Point = std::array<double, 2>;

    // A point on the curve with its two handles (absolute positions). A
    // handle on its point makes that side of the point straight.
    struct Knot {
        Point point{};
        Point in{};   // controls the segment arriving from the previous knot
        Point out{};  // controls the segment leaving for the next knot
        bool operator==(const Knot&) const = default;
    };
    using Curve = std::vector<Knot>;

    Path() = default;
    explicit Path(std::vector<Curve> curves) : curves_(std::move(curves)) {}

    // Throws Error naming what's wrong (an unknown command, a missing number).
    static Path parse(const std::string& svg);
    // Back to SVG path data: L for straight segments, C for the rest; reads
    // back as the same path.
    std::string str() const;

    const std::vector<Curve>& curves() const { return curves_; }
    std::vector<Curve>& curves() { return curves_; }
    bool empty() const;

    // Each curve as a closed polygon (the last point joins the first),
    // bending no more than about `step` pixels per edge.
    std::vector<std::vector<Point>> polygons(double step = 1.0) const;
    // The whole pixels the curves can reach (handles included).
    Box bounds() const;

    bool operator==(const Path&) const = default;

private:
    std::vector<Curve> curves_;
};

}  // namespace comp
