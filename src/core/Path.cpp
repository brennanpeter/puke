#include "comp/core/Path.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>

#include "comp/graph/Param.h"  // Error

namespace comp {

namespace {

struct Reader {
    const std::string& s;
    std::size_t i = 0;

    void skip() {
        while (i < s.size() && (std::isspace(static_cast<unsigned char>(s[i])) || s[i] == ',')) ++i;
    }
    bool done() {
        skip();
        return i >= s.size();
    }
    bool numberNext() {
        skip();
        return i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+' || s[i] == '.');
    }
    double number(char command) {
        skip();
        double v = 0;
        const char* begin = s.data() + i;
        if (i < s.size() && s[i] == '+') ++begin;  // from_chars doesn't take a leading +
        const auto [end, ec] = std::from_chars(begin, s.data() + s.size(), v);
        if (ec != std::errc() || !std::isfinite(v))
            throw Error("path: expected a number for '" + std::string(1, command) + "' at character " +
                        std::to_string(i + 1));
        i = static_cast<std::size_t>(end - s.data());
        return v;
    }
    Path::Point point(char command) {
        const double x = number(command);
        return {x, number(command)};
    }
};

// A double as short as it can be and still read back exactly.
std::string num(double v) {
    if (v == 0) v = 0;  // no "-0"
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, end);
}

double dist(const Path::Point& a, const Path::Point& b) { return std::hypot(b[0] - a[0], b[1] - a[1]); }

}  // namespace

Path Path::parse(const std::string& svg) {
    Reader r{svg};
    std::vector<Curve> curves;
    Curve cur;
    auto finish = [&] {
        if (cur.empty()) return;
        // A last point back on the first, as SVG closes with: the same knot.
        if (cur.size() > 1 && cur.back().point == cur.front().point) {
            cur.front().in = cur.back().in;
            cur.pop_back();
        }
        curves.push_back(std::move(cur));
        cur.clear();
    };
    char command = 0;
    while (!r.done()) {
        if (!r.numberNext()) {
            command = svg[r.i++];
        } else if (command == 0 || command == 'Z') {
            throw Error("path: a number where a command (M, L, C or Z) should be, at character " +
                        std::to_string(r.i + 1));
        }
        // A number after a command repeats it (M's extra pairs are L).
        switch (command) {
            case 'M': {
                finish();
                const Point p = r.point('M');
                cur.push_back({p, p, p});
                command = 'L';
                break;
            }
            case 'L': {
                if (cur.empty()) throw Error("path: 'L' before any 'M'");
                const Point p = r.point('L');
                cur.push_back({p, p, p});
                break;
            }
            case 'C': {
                if (cur.empty()) throw Error("path: 'C' before any 'M'");
                const Point c1 = r.point('C'), c2 = r.point('C'), p = r.point('C');
                cur.back().out = c1;
                cur.push_back({p, c2, p});
                break;
            }
            case 'Z':
                finish();
                break;
            default:
                throw Error("path: unsupported command '" + std::string(1, command) +
                            "' (only absolute M, L, C and Z are)");
        }
    }
    finish();
    return Path(std::move(curves));
}

std::string Path::str() const {
    std::string out;
    auto pt = [&](const Point& p) { out += num(p[0]) + " " + num(p[1]); };
    for (const Curve& c : curves_) {
        if (c.empty()) continue;
        if (!out.empty()) out += " ";
        out += "M ";
        pt(c[0].point);
        for (std::size_t i = 1; i <= c.size(); ++i) {
            const Knot& a = c[i - 1];
            const Knot& b = c[i % c.size()];
            const bool straight = a.out == a.point && b.in == b.point;
            if (i == c.size() && straight) break;  // Z draws the straight closing segment
            if (straight) {
                out += " L ";
            } else {
                out += " C ";
                pt(a.out);
                out += " ";
                pt(b.in);
                out += " ";
            }
            pt(b.point);
        }
        out += " Z";
    }
    return out;
}

bool Path::empty() const {
    return std::all_of(curves_.begin(), curves_.end(), [](const Curve& c) { return c.empty(); });
}

std::vector<std::vector<Path::Point>> Path::polygons(double step) const {
    std::vector<std::vector<Point>> out;
    for (const Curve& c : curves_) {
        if (c.empty()) continue;
        std::vector<Point> poly;
        for (std::size_t i = 0; i < c.size(); ++i) {
            const Knot& a = c[i];
            const Knot& b = c[(i + 1) % c.size()];
            poly.push_back(a.point);
            if (a.out == a.point && b.in == b.point) continue;  // straight
            // Enough pieces that each is about `step` long, judged by the
            // control polygon (never shorter than the curve).
            const double len = dist(a.point, a.out) + dist(a.out, b.in) + dist(b.in, b.point);
            const int n = std::clamp(static_cast<int>(std::ceil(len / std::max(step, 0.01))), 1, 1000);
            for (int k = 1; k < n; ++k) {
                const double t = double(k) / n, u = 1 - t;
                const double w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
                poly.push_back({w0 * a.point[0] + w1 * a.out[0] + w2 * b.in[0] + w3 * b.point[0],
                                w0 * a.point[1] + w1 * a.out[1] + w2 * b.in[1] + w3 * b.point[1]});
            }
        }
        out.push_back(std::move(poly));
    }
    return out;
}

Box Path::bounds() const {
    double x0 = std::numeric_limits<double>::infinity(), y0 = x0, x1 = -x0, y1 = -x0;
    for (const Curve& c : curves_)
        for (const Knot& k : c)
            for (const Point& p : {k.point, k.in, k.out}) {
                x0 = std::min(x0, p[0]), y0 = std::min(y0, p[1]);
                x1 = std::max(x1, p[0]), y1 = std::max(y1, p[1]);
            }
    if (x0 > x1) return {};
    auto clampi = [](double v) { return static_cast<int>(std::clamp(v, -1e8, 1e8)); };
    return {clampi(std::floor(x0)), clampi(std::floor(y0)), clampi(std::ceil(x1)), clampi(std::ceil(y1))};
}

}  // namespace comp
