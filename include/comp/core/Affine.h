#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "comp/core/Box.h"

namespace comp {

// 2D affine transform in pixel coordinates (y points down):
//   x' = a*x + b*y + c
//   y' = d*x + e*y + f
// Pixel (i, j) covers [i, i+1) x [j, j+1); its centre is (i + 0.5, j + 0.5).
struct Affine {
    double a = 1, b = 0, c = 0;
    double d = 0, e = 1, f = 0;

    static Affine translate(double x, double y) { return {1, 0, x, 0, 1, y}; }
    static Affine scale(double sx, double sy) { return {sx, 0, 0, 0, sy, 0}; }
    // Standard rotation matrix. With y pointing down, positive angles turn
    // clockwise on screen.
    static Affine rotate(double radians) {
        const double cs = std::cos(radians), sn = std::sin(radians);
        return {cs, -sn, 0, sn, cs, 0};
    }

    // (A * B) applies B first, then A.
    Affine operator*(const Affine& o) const {
        return {a * o.a + b * o.d, a * o.b + b * o.e, a * o.c + b * o.f + c,
                d * o.a + e * o.d, d * o.b + e * o.e, d * o.c + e * o.f + f};
    }

    double determinant() const { return a * e - b * d; }
    bool invertible() const { return std::fabs(determinant()) > 1e-12; }

    Affine inverse() const {
        const double id = 1.0 / determinant();
        return {e * id, -b * id, (b * f - e * c) * id, -d * id, a * id, (d * c - a * f) * id};
    }

    std::array<double, 2> apply(double x, double y) const {
        return {a * x + b * y + c, d * x + e * y + f};
    }

    bool isIdentity() const { return a == 1 && b == 0 && c == 0 && d == 0 && e == 1 && f == 0; }

    // Smallest pixel box containing the mapped area of `box` (its pixel edges,
    // not centres). Empty in, empty out.
    Box mapBounds(const Box& box) const {
        if (box.empty()) return {};
        double minX = HUGE_VAL, minY = HUGE_VAL, maxX = -HUGE_VAL, maxY = -HUGE_VAL;
        for (const auto& [x, y] : {std::array<double, 2>{double(box.x0), double(box.y0)},
                                   std::array<double, 2>{double(box.x1), double(box.y0)},
                                   std::array<double, 2>{double(box.x0), double(box.y1)},
                                   std::array<double, 2>{double(box.x1), double(box.y1)}}) {
            const auto [px, py] = apply(x, y);
            minX = std::min(minX, px);
            minY = std::min(minY, py);
            maxX = std::max(maxX, px);
            maxY = std::max(maxY, py);
        }
        // Clamp so absurd transforms can't overflow int.
        constexpr double kLimit = 1 << 28;
        auto toInt = [&](double v) { return static_cast<int>(std::clamp(v, -kLimit, kLimit)); };
        // A tiny epsilon keeps exact integer edges from growing by a pixel through rounding error.
        constexpr double eps = 1e-9;
        return {toInt(std::floor(minX + eps)), toInt(std::floor(minY + eps)),
                toInt(std::ceil(maxX - eps)), toInt(std::ceil(maxY - eps))};
    }
};

}  // namespace comp
