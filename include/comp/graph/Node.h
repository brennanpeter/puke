#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "comp/core/Box.h"
#include "comp/core/Hash.h"
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
using ParamMap = std::map<std::string, ParamValue>;

// Describes one parameter of a node type, for user interfaces (a properties
// panel builds its controls from these) and documentation. Rendering never
// depends on them: nodes read parameters by name, with their own defaults.
struct ParamSpec {
    enum class Kind { Bool, Int, Double, String, File, Choice, Vec2, Color };

    std::string name;
    Kind kind = Kind::Double;
    ParamValue defaultValue;  // what the node uses when the parameter isn't set
    std::string label;        // empty: use the name
    std::string hint;         // a sentence for tooltips
    std::vector<std::string> options;  // Choice: the values offered (stored as strings)
    bool openChoice = false;  // Choice: other values are allowed too (e.g. OCIO roles)
    double min = 0, max = 1;  // numbers: the range a slider should cover (not a limit)
    bool optional = false;    // may be left unset, which means something different from any value
};

class Node;

// While one of these is alive, params() on `node` *in this thread* returns
// `params` instead of the node's live values. The engine sets one around
// every hook it calls, so a render only ever sees the parameter values of
// the snapshot it started from, even while the graph is being edited.
class ParamScope {
public:
    ParamScope(const Node& node, const ParamMap& params);
    ~ParamScope();
    ParamScope(const ParamScope&) = delete;
    ParamScope& operator=(const ParamScope&) = delete;

private:
    const Node* prevNode_;
    const ParamMap* prevParams_;
};

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
// Unconnected optional inputs arrive as nullptr.
//
// Threading: the engine splits each node's region into tiles and calls
// compute() for several tiles at once, from different threads. All hooks are
// const, and nodes must not change any state while rendering. Each tile's
// pixels must depend only on the inputs and parameters, not on how the
// region was split, so tiled and untiled renders match exactly.
//
// Caching: the engine may reuse tiles from an earlier render. A node's cache
// key covers its type, parameters, inputs, the frame and the node's id (so
// two nodes never share tiles). If a node's output can change for any other
// reason after it's created (a file on disk, say), it must add that to the
// key in appendHash(), or stale tiles will be reused.
//
// Editing while rendering: parameters are copy-on-write (each change makes
// a new map), so a render can keep the map it started with; see Snapshot.
// Edit a graph from one thread only.
class Node : public std::enable_shared_from_this<Node> {
public:
    explicit Node(std::string name);
    virtual ~Node() = default;

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    const std::string& name() const { return name_; }

    // Unique for the life of the program, never reused, even after the node
    // is destroyed. Part of every cache key.
    std::uint64_t id() const { return id_; }
    virtual const char* typeName() const = 0;

    // --- inputs ---
    virtual int maxInputs() const { return 0; }
    virtual bool inputOptional(int /*index*/) const { return false; }
    // A short name for an input, for node graph editors ("B", "Mask"). Default: none.
    virtual std::string inputLabel(int /*index*/) const { return {}; }
    void setInput(int index, Node* node);  // nullptr disconnects
    Node* input(int index) const;
    // Like input(), but throws if unconnected.
    Node& requireInput(int index) const;

    // --- parameters ---
    void setParam(const std::string& key, ParamValue value);
    // Removes a parameter, so the node uses its default again.
    void clearParam(const std::string& key);
    bool hasParam(const std::string& key) const { return params().count(key) > 0; }
    // The live values, or inside a render the snapshot's (see ParamScope).
    const ParamMap& params() const;
    // The current values, shared: they never change once taken.
    std::shared_ptr<const ParamMap> paramsSnapshot() const { return params_; }

    template <typename T>
    T param(const std::string& key) const {
        const ParamMap& ps = params();
        auto it = ps.find(key);
        if (it == ps.end()) throw Error(name_ + ": missing parameter '" + key + "'");
        if (const T* v = std::get_if<T>(&it->second)) return *v;
        throw Error(name_ + ": parameter '" + key + "' has the wrong type");
    }

    template <typename T>
    T param(const std::string& key, T fallback) const {
        return hasParam(key) ? param<T>(key) : fallback;
    }

    // The parameters this node type understands, in display order. Default:
    // none described (the node still reads whatever parameters it's given).
    virtual std::vector<ParamSpec> paramSpecs() const { return {}; }

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

    // Whether the engine may split this node's region into tiles. Return
    // false when each compute() call has a large fixed cost that tiling
    // would repeat (e.g. opening a file); the node then gets its whole
    // region in one call, on one thread.
    virtual bool supportsTiles() const { return true; }

    // Adds anything besides parameters and inputs that this node's output
    // depends on to its cache key. Called once per render, before compute.
    // Default: nothing.
    virtual void appendHash(Hasher& /*h*/, const RenderContext& /*ctx*/) const {}

    // --- user interface ---
    // Where the node sits in a node graph editor, if it's been placed.
    // Saved in project files; never affects rendering.
    const std::optional<Vec2>& position() const { return position_; }
    void setPosition(std::optional<Vec2> p) { position_ = p; }

private:
    std::string name_;
    std::uint64_t id_;
    std::vector<Node*> inputs_;  // non-owning; the Graph owns nodes
    std::shared_ptr<const ParamMap> params_;  // never null; replaced, never modified
    std::optional<Vec2> position_;
};

}  // namespace comp
