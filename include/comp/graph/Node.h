#pragma once

#include <array>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "comp/core/Box.h"
#include "comp/core/Image.h"

namespace comp {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// Everything a render request needs besides the region.
// Time is a double so retiming and motion blur can sample sub-frames later.
struct RenderContext {
    double frame = 1.0;
};

using Vec2 = std::array<double, 2>;   // x, y
using Color = std::array<double, 4>;  // r, g, b, a

using ParamValue = std::variant<bool, int, double, std::string, Vec2, Color>;

// Base class for all nodes.
//
// Nodes never call their inputs themselves. The Engine walks the graph and
// calls these three hooks, handing each node its inputs' results:
//
//   computeInfo()  What this node produces (data window, format, channels),
//                  given its inputs' infos. No pixel work; must be cheap.
//
//   inputRegion()  Which region of input `index` is needed to produce
//                  `outputRegion`. Default: the same region (pixel-wise ops).
//                  A blur pads it; a transform maps it through the inverse.
//
//   compute()      Produce pixels for exactly `region` with exactly the
//                  channels from computeInfo(). `region` is never empty and
//                  always lies inside this node's data window.
//                  Each inputs[i] covers at least inputRegion(i, region)
//                  clipped to that input's data window (everything outside
//                  a data window is zero by definition). Read input pixels
//                  with Image::at(), which returns 0 outside the image.
//
// Unconnected optional inputs arrive as nullptr. All hooks are const: nodes
// must not change state while rendering (this is what will let the engine
// run them on many threads later).
class Node {
public:
    explicit Node(std::string name) : name_(std::move(name)) {}
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const std::string& name() const { return name_; }
    virtual const char* typeName() const = 0;

    // --- inputs ---
    virtual int maxInputs() const { return 0; }
    virtual bool inputOptional(int /*index*/) const { return false; }
    void setInput(int index, Node* node);  // nullptr disconnects
    Node* input(int index) const;
    // Like input(), but throws if unconnected.
    Node& requireInput(int index) const;

    // --- parameters ---
    void setParam(const std::string& key, ParamValue value) { params_[key] = std::move(value); }
    bool hasParam(const std::string& key) const { return params_.count(key) > 0; }

    template <typename T>
    T param(const std::string& key) const {
        auto it = params_.find(key);
        if (it == params_.end()) throw Error(name_ + ": missing parameter '" + key + "'");
        if (const T* v = std::get_if<T>(&it->second)) return *v;
        throw Error(name_ + ": parameter '" + key + "' has the wrong type");
    }

    template <typename T>
    T param(const std::string& key, T fallback) const {
        return hasParam(key) ? param<T>(key) : fallback;
    }

    // Lenient getters for numeric parameters, so callers needn't care whether
    // a value was stored as 2 or 2.0, or as one number or per-component.
    double numberParam(const std::string& key, double fallback) const;  // int or double
    Vec2 vec2Param(const std::string& key, Vec2 fallback) const;         // Vec2, or one number for both
    Color colorParam(const std::string& key, double fallback) const;     // Color, or one number for all

    // --- evaluation hooks (called by Engine) ---
    virtual ImageInfo computeInfo(const RenderContext& ctx,
                                  std::span<const ImageInfo* const> inputs) const = 0;

    virtual Box inputRegion(const RenderContext& /*ctx*/, int /*index*/,
                            const Box& outputRegion) const {
        return outputRegion;
    }

    virtual Image compute(const RenderContext& ctx, const Box& region,
                          std::span<const Image* const> inputs) const = 0;

private:
    std::string name_;
    std::vector<Node*> inputs_;  // non-owning; the Graph owns nodes
    std::map<std::string, ParamValue> params_;
};

}  // namespace comp
