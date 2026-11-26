#include "puke/graph/Curve.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "puke/core/Path.h"

namespace puke {

namespace {

auto byFrame(std::vector<Curve::Key>& keys, double frame) {
    return std::lower_bound(keys.begin(), keys.end(), frame,
                            [](const Curve::Key& k, double f) { return k.frame < f; });
}

// A value as numbers, to be weighted and added; and back.
std::optional<std::vector<double>> numbers(const ParamValue& v, bool shapes = true) {
    if (auto* d = std::get_if<double>(&v)) return std::vector<double>{*d};
    if (auto* i = std::get_if<int>(&v)) return std::vector<double>{double(*i)};
    if (auto* a = std::get_if<Vec2>(&v)) return std::vector<double>(a->begin(), a->end());
    if (auto* c = std::get_if<Color>(&v)) return std::vector<double>(c->begin(), c->end());
    if (auto* s = std::get_if<std::string>(&v); s && shapes) {
        try {
            const Path p = Path::parse(*s);
            if (p.empty()) return std::nullopt;
            std::vector<double> out;
            for (const auto& c : p.curves())
                for (const auto& k : c)
                    for (const auto& q : {k.point, k.in, k.out}) out.insert(out.end(), q.begin(), q.end());
            return out;
        } catch (const Error&) {
            return std::nullopt;  // an ordinary string
        }
    }
    return std::nullopt;
}

// Whether two values can be blended: same type, and for shapes the same
// curves and points.
bool blendable(const ParamValue& a, const ParamValue& b) {
    if (a.index() != b.index() || std::holds_alternative<bool>(a)) return false;
    if (auto* s = std::get_if<std::string>(&a)) {
        try {
            const Path pa = Path::parse(*s), pb = Path::parse(std::get<std::string>(b));
            if (pa.empty() || pa.curves().size() != pb.curves().size()) return false;
            for (std::size_t i = 0; i < pa.curves().size(); ++i)
                if (pa.curves()[i].size() != pb.curves()[i].size()) return false;
            return true;
        } catch (const Error&) {
            return false;
        }
    }
    return true;
}

ParamValue fromNumbers(const ParamValue& like, const std::vector<double>& n) {
    if (std::holds_alternative<double>(like)) return n[0];
    if (std::holds_alternative<int>(like)) return static_cast<int>(std::lround(n[0]));
    if (std::holds_alternative<Vec2>(like)) return Vec2{n[0], n[1]};
    if (std::holds_alternative<Color>(like)) return Color{n[0], n[1], n[2], n[3]};
    // A shape: the same layout as `like`, with new numbers.
    Path p = Path::parse(std::get<std::string>(like));
    std::size_t i = 0;
    for (auto& c : p.curves())
        for (auto& k : c)
            for (auto* q : {&k.point, &k.in, &k.out}) (*q)[0] = n[i], (*q)[1] = n[i + 1], i += 2;
    return p.str();
}

}  // namespace

void Curve::setKey(double frame, ParamValue value) {
    auto it = byFrame(keys_, frame);
    if (it != keys_.end() && it->frame == frame) it->value = std::move(value);
    else keys_.insert(it, Key{frame, std::move(value), Interp::Smooth, {}});
}

void Curve::setKey(double frame, ParamValue value, Interp interp) {
    setKey(frame, std::move(value));
    byFrame(keys_, frame)->interp = interp;
}

bool Curve::removeKey(double frame) {
    auto it = byFrame(keys_, frame);
    if (it == keys_.end() || it->frame != frame) return false;
    keys_.erase(it);
    return true;
}

const Curve::Key* Curve::keyAt(double frame) const {
    auto it = byFrame(const_cast<std::vector<Key>&>(keys_), frame);
    return it != keys_.end() && it->frame == frame ? &*it : nullptr;
}

void Curve::setInterp(double frame, Interp interp) {
    auto it = byFrame(keys_, frame);
    if (it == keys_.end() || it->frame != frame) throw Error("no key at frame " + std::to_string(frame));
    it->interp = interp;
}

std::optional<std::vector<double>> Curve::components(const ParamValue& v, bool shapes) { return numbers(v, shapes); }

std::vector<double> Curve::slopeAt(std::size_t i) const {
    if (i >= keys_.size()) return {};
    const Key& k = keys_[i];
    const auto p = numbers(k.value);
    if (!p || std::holds_alternative<bool>(k.value)) return {};
    if (!k.slope.empty() && k.slope.size() == p->size()) return k.slope;
    std::vector<double> m(p->size(), 0.0);  // flat
    // Automatic: through the neighbours, if both can be blended with it.
    if (i == 0 || i + 1 >= keys_.size()) return m;
    const Key& prev = keys_[i - 1];
    const Key& next = keys_[i + 1];
    if (!blendable(prev.value, k.value) || !blendable(next.value, k.value)) return m;
    const auto a = numbers(prev.value), b = numbers(next.value);
    if (!a || !b || a->size() != m.size() || b->size() != m.size()) return m;
    for (std::size_t c = 0; c < m.size(); ++c) m[c] = ((*b)[c] - (*a)[c]) / (next.frame - prev.frame);
    return m;
}

void Curve::setSlope(double frame, std::vector<double> slope) {
    auto it = byFrame(keys_, frame);
    if (it == keys_.end() || it->frame != frame) throw Error("no key at frame " + std::to_string(frame));
    if (!slope.empty()) {
        const auto n = numbers(it->value, false);
        if (!n || std::holds_alternative<bool>(it->value))
            throw Error("only keys holding numbers, Vec2s or Colors can have a slope");
        if (slope.size() != n->size())
            throw Error("a slope needs " + std::to_string(n->size()) + " number(s) for this key, not " +
                        std::to_string(slope.size()));
        for (double d : slope)
            if (!std::isfinite(d)) throw Error("a slope must be finite");
    }
    it->slope = std::move(slope);
}

void Curve::moveKey(double from, double to) {
    auto it = byFrame(keys_, from);
    if (it == keys_.end() || it->frame != from) throw Error("no key at frame " + std::to_string(from));
    Key k = std::move(*it);
    keys_.erase(it);
    k.frame = to;
    auto at = byFrame(keys_, to);
    if (at != keys_.end() && at->frame == to) *at = std::move(k);
    else keys_.insert(at, std::move(k));
}

ParamValue Curve::valueAt(double frame) const {
    if (keys_.empty()) throw Error("an animation curve with no keys");
    if (frame <= keys_.front().frame) return keys_.front().value;
    if (frame >= keys_.back().frame) return keys_.back().value;
    // The segment from key i to key i + 1.
    const std::size_t i = static_cast<std::size_t>(
        std::upper_bound(keys_.begin(), keys_.end(), frame, [](double f, const Key& k) { return f < k.frame; }) -
        keys_.begin() - 1);
    const Key& a = keys_[i];
    const Key& b = keys_[i + 1];
    if (a.interp == Interp::Constant || !blendable(a.value, b.value)) return a.value;
    const auto pa = numbers(a.value), pb = numbers(b.value);
    if (!pa || !pb || pa->size() != pb->size()) return a.value;

    const double h = b.frame - a.frame, s = (frame - a.frame) / h;
    std::vector<double> out(pa->size());
    if (a.interp == Interp::Linear) {
        for (std::size_t c = 0; c < out.size(); ++c) out[c] = (1 - s) * (*pa)[c] + s * (*pb)[c];
    } else {  // smooth: cubic Hermite between the two keys' tangents
        const double s2 = s * s, s3 = s2 * s;
        const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        const std::vector<double> ma = slopeAt(i), mb = slopeAt(i + 1);
        for (std::size_t c = 0; c < out.size(); ++c)
            out[c] = h00 * (*pa)[c] + h01 * (*pb)[c] + h * (h10 * (c < ma.size() ? ma[c] : 0) + h11 * (c < mb.size() ? mb[c] : 0));
    }
    return fromNumbers(a.value, out);
}

const char* Curve::interpName(Interp i) {
    switch (i) {
        case Interp::Smooth: return "smooth";
        case Interp::Linear: return "linear";
        case Interp::Constant: return "constant";
    }
    return "smooth";
}

Curve::Interp Curve::parseInterp(const std::string& s) {
    if (s == "smooth") return Interp::Smooth;
    if (s == "linear") return Interp::Linear;
    if (s == "constant") return Interp::Constant;
    throw Error("unknown interpolation '" + s + "' (use smooth, linear or constant)");
}

}  // namespace puke
