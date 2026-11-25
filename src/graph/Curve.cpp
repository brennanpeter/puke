#include "comp/graph/Curve.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include "comp/core/Path.h"

namespace comp {

namespace {

auto byFrame(std::vector<Curve::Key>& keys, double frame) {
    return std::lower_bound(keys.begin(), keys.end(), frame,
                            [](const Curve::Key& k, double f) { return k.frame < f; });
}

// A value as numbers, to be weighted and added; and back.
std::optional<std::vector<double>> numbers(const ParamValue& v) {
    if (auto* d = std::get_if<double>(&v)) return std::vector<double>{*d};
    if (auto* i = std::get_if<int>(&v)) return std::vector<double>{double(*i)};
    if (auto* a = std::get_if<Vec2>(&v)) return std::vector<double>(a->begin(), a->end());
    if (auto* c = std::get_if<Color>(&v)) return std::vector<double>(c->begin(), c->end());
    if (auto* s = std::get_if<std::string>(&v)) {
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
    else keys_.insert(it, Key{frame, std::move(value), Interp::Smooth});
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

    const double h = b.frame - a.frame, s = (frame - a.frame) / h;
    // Weights on the values of keys i-1, i, i+1, i+2.
    double w[4] = {0, 1 - s, s, 0};
    if (a.interp == Interp::Smooth) {
        const double s2 = s * s, s3 = s2 * s;
        const double h00 = 2 * s3 - 3 * s2 + 1, h10 = s3 - 2 * s2 + s, h01 = -2 * s3 + 3 * s2, h11 = s3 - s2;
        w[0] = 0, w[1] = h00, w[2] = h01, w[3] = 0;
        // Tangents (value per frame): through the neighbours, flat at the ends.
        // A neighbour that can't be blended counts as flat too.
        if (i > 0 && blendable(keys_[i - 1].value, a.value)) {
            const double k = h * h10 / (b.frame - keys_[i - 1].frame);
            w[2] += k, w[0] -= k;
        }
        if (i + 2 < keys_.size() && blendable(keys_[i + 2].value, b.value)) {
            const double k = h * h11 / (keys_[i + 2].frame - a.frame);
            w[3] += k, w[1] -= k;
        }
    }
    const ParamValue* vals[4] = {i > 0 ? &keys_[i - 1].value : nullptr, &a.value, &b.value,
                                 i + 2 < keys_.size() ? &keys_[i + 2].value : nullptr};
    std::vector<double> sum;
    for (int j = 0; j < 4; ++j) {
        if (w[j] == 0 || !vals[j]) continue;
        const auto n = numbers(*vals[j]);
        if (!n) return a.value;  // can't happen for blendable values
        if (sum.empty()) sum.assign(n->size(), 0.0);
        if (n->size() != sum.size()) return a.value;
        for (std::size_t k = 0; k < n->size(); ++k) sum[k] += w[j] * (*n)[k];
    }
    return sum.empty() ? a.value : fromNumbers(a.value, sum);
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

}  // namespace comp
