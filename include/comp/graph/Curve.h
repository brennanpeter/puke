#pragma once

#include <map>
#include <string>
#include <vector>

#include "comp/graph/Param.h"

namespace comp {

// An animated parameter: keyframes, each a frame and a value, evaluated at
// any frame. Before the first key and after the last the value holds.
//
// How a segment moves from one key to the next is the earlier key's interp:
//   Smooth    a cubic through the neighbouring keys (Catmull-Rom tangents,
//             flat at the first and last key), so motion eases in and out
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

}  // namespace comp
