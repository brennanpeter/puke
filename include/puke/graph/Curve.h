#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "puke/graph/Param.h"

namespace puke {

// An animated parameter: keyframes, each a frame and a value, evaluated at
// any frame. Before the first key and after the last the value holds.
//
// How a segment moves from one key to the next is the earlier key's interp:
//   Smooth    a cubic; each key's tangent (its slope, value per frame) is
//             automatic (Catmull-Rom: through the neighbouring keys, flat
//             at the first and last key) unless the key sets its own
//   Linear    a straight line
//   Constant  holds the value until the next key
//
// Numbers, Vec2 and Color values are interpolated component by component
// (ints round to the nearest whole number). Strings normally hold, except
// shapes: two keys whose strings are Paths with the same curves and points
// (see Path.h) move point by point. Bools hold. Keys of different types
// hold too.
class Curve {
public:
    enum class Interp { Smooth, Linear, Constant };

    struct Key {
        double frame = 0;
        ParamValue value;
        Interp interp = Interp::Smooth;
        // The key's own tangent, one slope per component (1 for a number, 2
        // for a Vec2, 4 for a Color); empty for automatic. Used by the smooth
        // segments on both sides.
        std::vector<double> slope;
        bool operator==(const Key&) const = default;
    };

    Curve() = default;

    // Frames in order, at most one key per frame.
    const std::vector<Key>& keys() const { return keys_; }
    bool empty() const { return keys_.empty(); }

    // Adds a key, or replaces the one at that frame (keeping its interp
    // unless `interp` is given).
    void setKey(double frame, ParamValue value);
    void setKey(double frame, ParamValue value, Interp interp);
    // Returns whether there was a key there.
    bool removeKey(double frame);
    const Key* keyAt(double frame) const;
    void setInterp(double frame, Interp interp);
    // Sets the key's tangent; empty for automatic. Throws Error if there's
    // no key there, or the slope doesn't match its value (numbers only).
    void setSlope(double frame, std::vector<double> slope);
    // Moves the key at `from` to `to`, replacing any key there.
    void moveKey(double from, double to);
    // The tangent smooth segments use at key `index`: its own, or the
    // automatic one. Empty for values that have no slope (strings, bools).
    std::vector<double> slopeAt(std::size_t index) const;

    // A value's numbers, for interpolating and drawing: a number, Vec2 or
    // Color (and a shape's points, when `shapes`); nullopt for the rest.
    static std::optional<std::vector<double>> components(const ParamValue& v, bool shapes = false);

    // Throws Error if the curve has no keys.
    ParamValue valueAt(double frame) const;

    static const char* interpName(Interp i);
    // "smooth", "linear" or "constant"; throws Error for anything else.
    static Interp parseInterp(const std::string& s);

    bool operator==(const Curve&) const = default;

private:
    std::vector<Key> keys_;
};

// A node's animated parameters, by name.
using CurveMap = std::map<std::string, Curve>;

}  // namespace puke
