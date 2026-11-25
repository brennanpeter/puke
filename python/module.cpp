// Python bindings for comp (module name: comp).
//
//   import comp, numpy as np
//   g = comp.Graph()
//   read = g.create("Read", file="plate.####.exr", first=1001, last=1100)
//   grade = g.create("Grade", gain=(1.2, 1.1, 1.0, 1.0))
//   g.connect(read, grade)
//   engine = comp.Engine()
//   img = engine.render(grade, frame=1001)
//   pixels = np.asarray(img)          # shape (height, width, channels), float32
//   g.save("shot.comp")               # comp.Graph.load("shot.comp") reads it back
//
// Notes on the design:
//   - Graphs own their nodes. Python only ever holds a handle (graph + node
//     id) that is checked on every use, so using a node after
//     Graph.remove() raises comp.Error instead of crashing.
//   - Images expose their pixels through the buffer protocol, read-only and
//     without copying; numpy is not needed to build or import the module.
//   - Engine.render() and .info() snapshot the graph (with the GIL held),
//     then release the GIL while rendering: other Python threads keep
//     running and may edit the graph without affecting that render. One
//     Engine renders one thing at a time; a second call waits its turn.
//     Write.execute() keeps the GIL (it snapshots part-way through).

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "comp/color/ColorConfig.h"
#include "comp/engine/Engine.h"
#include "comp/engine/Snapshot.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/Registry.h"
#include "comp/nodes/WriteNode.h"
#include "comp/project/Project.h"

namespace py = pybind11;
using namespace py::literals;

namespace {

using comp::Box;
using comp::Color;
using comp::Engine;
using comp::Error;
using comp::Graph;
using comp::Image;
using comp::ImageInfo;
using comp::Node;
using comp::ParamValue;
using comp::Vec2;

// ---------------------------------------------------------------- parameters

bool isNumber(py::handle v) {
    if (py::isinstance<py::bool_>(v)) return false;
    return py::hasattr(v, "__index__") || py::hasattr(v, "__float__");
}

// Python value -> parameter, by explicit rules rather than pybind11's
// variant conversion, so the stored type is always predictable:
//   bool -> bool, int -> int, float -> float, str -> string,
//   a sequence of 2 numbers -> Vec2, of 4 numbers -> Color.
ParamValue toParam(const std::string& key, py::handle v) {
    if (py::isinstance<py::bool_>(v)) return v.cast<bool>();
    if (py::isinstance<py::str>(v)) return v.cast<std::string>();
    // Sequences first: numpy arrays also define __index__.
    if (py::isinstance<py::sequence>(v) && !py::isinstance<py::bytes>(v)) {
        const auto seq = py::reinterpret_borrow<py::sequence>(v);
        const std::size_t n = seq.size();
        bool numbers = true;
        for (std::size_t i = 0; numbers && i < n; ++i) numbers = isNumber(seq[i]);
        if (numbers && n == 2) return Vec2{seq[0].cast<double>(), seq[1].cast<double>()};
        if (numbers && n == 4)
            return Color{seq[0].cast<double>(), seq[1].cast<double>(), seq[2].cast<double>(),
                         seq[3].cast<double>()};
        throw py::type_error("parameter '" + key + "': a sequence must be 2 numbers (Vec2) or 4 (Color), got " +
                             std::to_string(n) + (numbers ? " numbers" : " values, not all numbers"));
    }
    if (py::hasattr(v, "__index__")) {  // int, numpy integers
        const auto i = py::reinterpret_steal<py::int_>(PyNumber_Index(v.ptr()));
        if (!i) throw py::error_already_set();
        const long long n = i.cast<long long>();
        if (n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
            throw py::value_error("parameter '" + key + "': " + std::to_string(n) +
                                  " doesn't fit in a 32-bit int");
        return static_cast<int>(n);
    }
    if (py::hasattr(v, "__float__")) return v.cast<double>();  // float, numpy floats
    throw py::type_error("parameter '" + key +
                         "' must be a bool, int, float, str, or a sequence of 2 or 4 numbers, not " +
                         std::string(py::str(py::type::of(v).attr("__name__"))));
}

py::object fromParam(const ParamValue& v) {
    return std::visit(
        [](const auto& x) -> py::object {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, Vec2>) return py::make_tuple(x[0], x[1]);
            else if constexpr (std::is_same_v<T, Color>) return py::make_tuple(x[0], x[1], x[2], x[3]);
            else return py::cast(x);
        },
        v);
}

// ---------------------------------------------------------------- node handles

// What Python holds for a node: the owning graph, plus the node's pointer and
// id. get() checks the node is still in the graph, and is still the same node
// (a new node could reuse a removed one's address; ids are never reused).
struct NodeRef {
    std::shared_ptr<Graph> graph;
    Node* node;
    std::uint64_t id;

    Node& get() const {
        for (const auto& n : graph->nodes())
            if (n.get() == node && n->id() == id) return *node;
        throw Error("this node was removed from its graph");
    }
};

NodeRef refTo(const std::shared_ptr<Graph>& g, Node& n) { return {g, &n, n.id()}; }

std::optional<NodeRef> optionalRef(const std::shared_ptr<Graph>& g, Node* n) {
    if (!n) return std::nullopt;
    return refTo(g, *n);
}

// Graph.create() names: "Grade1", "Grade2", ... first one not taken. OFX
// types ("ofx:net.sf.openfx.GammaPlugin") use the identifier's last part.
std::string freeName(const Graph& g, const std::string& type) {
    std::string base = type;
    if (base.rfind("ofx:", 0) == 0) base = base.substr(base.find_last_of(".:") + 1);
    for (int i = 1;; ++i) {
        std::string name = base + std::to_string(i);
        if (!g.find(name)) return name;
    }
}

std::string reprBox(const Box& b) {
    return "Box(" + std::to_string(b.x0) + ", " + std::to_string(b.y0) + ", " + std::to_string(b.x1) +
           ", " + std::to_string(b.y1) + ")";
}

std::string reprChannels(const comp::ChannelList& c) {
    std::string s = "[";
    for (std::size_t i = 0; i < c.size(); ++i) s += (i ? ", '" : "'") + c[i] + "'";
    return s + "]";
}

// A render at `frame` of a node in `graph`, in the graph's project format.
comp::RenderContext context(const Graph& graph, double frame) { return comp::RenderContext{frame, graph.settings().format}; }

// What Python's Engine wraps: an engine plus a lock, because renders run
// without the GIL and an Engine renders one thing at a time.
struct PyEngine {
    explicit PyEngine(comp::EngineOptions o) : engine(o) {}
    Engine engine;
    std::mutex mutex;
};

}  // namespace

PYBIND11_MODULE(comp, m) {
    m.doc() = "comp: a node-based VFX compositor (Python bindings for the C++ library)";

    py::register_exception<Error>(m, "Error");

    m.def("node_types", &comp::nodeTypes, "Every node type Graph.create() accepts.");

    // ------------------------------------------------------------ Box

    py::class_<Box>(m, "Box", R"(Half-open integer pixel region [x0, x1) x [y0, y1).
Y increases downward, as in EXR data windows. A plain 4-tuple works anywhere a Box is expected.)")
        .def(py::init<>())
        .def(py::init([](int x0, int y0, int x1, int y1) { return Box{x0, y0, x1, y1}; }), "x0"_a,
             "y0"_a, "x1"_a, "y1"_a)
        .def(py::init([](const py::tuple& t) {
                 if (t.size() != 4) throw py::value_error("a Box needs 4 numbers: (x0, y0, x1, y1)");
                 return Box{t[0].cast<int>(), t[1].cast<int>(), t[2].cast<int>(), t[3].cast<int>()};
             }),
             "xyxy"_a)
        .def_readwrite("x0", &Box::x0)
        .def_readwrite("y0", &Box::y0)
        .def_readwrite("x1", &Box::x1)
        .def_readwrite("y1", &Box::y1)
        .def_property_readonly("width", &Box::width)
        .def_property_readonly("height", &Box::height)
        .def_property_readonly("area", &Box::area)
        .def("empty", &Box::empty)
        .def("contains", &Box::contains, "x"_a, "y"_a)
        .def("intersect", &Box::intersect, "other"_a)
        .def("unite", &Box::unite, "other"_a)
        .def("padded", &Box::padded, "pixels"_a)
        .def("__eq__", [](const Box& a, const Box& b) { return a == b; })
        .def("__hash__", [](const Box& b) { return py::hash(py::make_tuple(b.x0, b.y0, b.x1, b.y1)); })
        .def("__iter__", [](const Box& b) { return py::iter(py::make_tuple(b.x0, b.y0, b.x1, b.y1)); })
        .def("__repr__", &reprBox);
    py::implicitly_convertible<py::tuple, Box>();

    // ------------------------------------------------------------ ImageInfo

    py::class_<ImageInfo>(m, "ImageInfo", "What a node produces, without computing any pixels.")
        .def_readonly("data_window", &ImageInfo::dataWindow, "Where pixels actually exist.")
        .def_readonly("display_window", &ImageInfo::displayWindow, "The nominal frame (format).")
        .def_readonly("channels", &ImageInfo::channels)
        .def("__repr__", [](const ImageInfo& i) {
            return "ImageInfo(data_window=" + reprBox(i.dataWindow) +
                   ", display_window=" + reprBox(i.displayWindow) + ", channels=" + reprChannels(i.channels) +
                   ")";
        });

    // ------------------------------------------------------------ Image

    py::class_<Image>(m, "Image", py::buffer_protocol(), R"(A block of float32 pixels covering `region`.

numpy.asarray(img) gives a read-only view, shape (height, width, channels),
without copying. Array index [row, col] is pixel (region.x0 + col, region.y0 + row).)")
        .def_property_readonly("region", &Image::region)
        .def_property_readonly("channels", &Image::channels)
        .def_property_readonly("width", [](const Image& i) { return i.region().width(); })
        .def_property_readonly("height", [](const Image& i) { return i.region().height(); })
        .def("channel_index", &Image::channelIndex, "name"_a, "Index of a channel, or -1 if absent.")
        .def(
            "pixel",
            [](const Image& img, int x, int y) {
                py::tuple t(img.numChannels());
                for (int c = 0; c < img.numChannels(); ++c) t[c] = img.at(x, y, c);
                return t;
            },
            "x"_a, "y"_a, "All channels of pixel (x, y), in absolute coordinates; zeros outside the region.")
        .def_buffer([](const Image& img) {
            static const float empty = 0.0f;  // numpy wants a pointer even for 0 elements
            const auto h = static_cast<py::ssize_t>(img.region().height());
            const auto w = static_cast<py::ssize_t>(img.region().width());
            const auto c = static_cast<py::ssize_t>(img.numChannels());
            const py::ssize_t f = sizeof(float);
            return py::buffer_info(const_cast<float*>(img.data() ? img.data() : &empty), f,
                                   py::format_descriptor<float>::format(), 3, {h, w, c},
                                   {w * c * f, c * f, f}, /*readonly=*/true);
        })
        .def("__repr__", [](const Image& i) {
            return "Image(region=" + reprBox(i.region()) + ", channels=" + reprChannels(i.channels()) + ")";
        });

    // ------------------------------------------------------------ RenderStats

    py::class_<comp::RenderStats>(m, "RenderStats", "Counters from an Engine's most recent render.")
        .def_readonly("nodes_computed", &comp::RenderStats::nodesComputed)
        .def_readonly("pixels_computed", &comp::RenderStats::pixelsComputed)
        .def_readonly("tiles_computed", &comp::RenderStats::tilesComputed)
        .def_readonly("tiles_from_cache", &comp::RenderStats::tilesFromCache)
        .def("__repr__", [](const comp::RenderStats& s) {
            return "RenderStats(nodes_computed=" + std::to_string(s.nodesComputed) +
                   ", pixels_computed=" + std::to_string(s.pixelsComputed) +
                   ", tiles_computed=" + std::to_string(s.tilesComputed) +
                   ", tiles_from_cache=" + std::to_string(s.tilesFromCache) + ")";
        });

    // ------------------------------------------------------------ Engine

    py::class_<PyEngine>(m, "Engine", R"(Renders node graphs.

threads:     0 = one per core, 1 = the calling thread only
tile_size:   tile edge in pixels
cache_bytes: memory for the tile cache; 0 turns it off.

Renders release the GIL: the graph is snapshotted first, so other threads
may keep editing it. One Engine renders one thing at a time.)")
        .def(py::init([](int threads, int tileSize, std::size_t cacheBytes) {
                 if (threads < 0) throw py::value_error("threads must be 0 or more");
                 return std::make_unique<PyEngine>(comp::EngineOptions{threads, tileSize, cacheBytes});
             }),
             "threads"_a = 0, "tile_size"_a = 64, "cache_bytes"_a = 0)
        .def_property_readonly("threads", [](const PyEngine& e) { return e.engine.threads(); })
        .def_property_readonly("tile_size", [](const PyEngine& e) { return e.engine.options().tileSize; })
        .def_property(
            "cache_bytes", [](const PyEngine& e) { return e.engine.options().cacheBytes; },
            [](PyEngine& e, std::size_t bytes) {
                py::gil_scoped_release nogil;
                std::lock_guard lock(e.mutex);
                e.engine.setCacheBytes(bytes);
            })
        .def_property_readonly("cache_bytes_used", [](const PyEngine& e) { return e.engine.cacheBytesUsed(); })
        .def("clear_cache",
             [](PyEngine& e) {
                 py::gil_scoped_release nogil;
                 std::lock_guard lock(e.mutex);
                 e.engine.clearCache();
             })
        .def_property_readonly("last_stats", [](const PyEngine& e) { return e.engine.lastStats(); })
        .def(
            "info",
            [](PyEngine& e, const NodeRef& n, double frame) {
                const auto snap = comp::Snapshot::of(n.get());  // with the GIL: nothing edits meanwhile
                const auto ctx = context(*n.graph, frame);
                py::gil_scoped_release nogil;
                std::lock_guard lock(e.mutex);
                return e.engine.info(snap, ctx);
            },
            "node"_a, "frame"_a = 1.0)
        .def(
            "render",
            [](PyEngine& e, const NodeRef& n, std::optional<Box> region, double frame) {
                const auto snap = comp::Snapshot::of(n.get());
                const auto ctx = context(*n.graph, frame);
                py::gil_scoped_release nogil;
                std::lock_guard lock(e.mutex);
                return e.engine.render(snap, ctx, region ? *region : e.engine.info(snap, ctx).dataWindow);
            },
            "node"_a, "region"_a = py::none(), "frame"_a = 1.0,
            "Renders `region` (default: the node's data window). Pixels outside the data window are zero.");

    // ------------------------------------------------------------ Node

    py::class_<NodeRef>(m, "Node", R"(A node in a Graph. Create nodes with Graph.create().

Parameters work like a dict: node["gain"] = 1.2, node["gain"], "gain" in node.
Values are bool, int, float, str, or a sequence of 2 (Vec2) or 4 (Color) numbers.)")
        .def_property_readonly("name", [](const NodeRef& n) { return n.get().name(); })
        .def_property_readonly("type", [](const NodeRef& n) { return std::string(n.get().typeName()); })
        .def_property_readonly("id", [](const NodeRef& n) { return n.get().id(); })
        .def_property_readonly("graph", [](const NodeRef& n) { return n.graph; })
        .def_property_readonly("max_inputs", [](const NodeRef& n) { return n.get().maxInputs(); })
        .def("input_optional", [](const NodeRef& n, int i) { return n.get().inputOptional(i); }, "index"_a)
        .def("input", [](const NodeRef& n, int i) { return optionalRef(n.graph, n.get().input(i)); }, "index"_a,
             "The node connected to input `index`, or None.")
        .def_property_readonly("inputs",
                               [](const NodeRef& n) {
                                   std::vector<std::optional<NodeRef>> out;
                                   for (int i = 0; i < n.get().maxInputs(); ++i)
                                       out.push_back(optionalRef(n.graph, n.get().input(i)));
                                   return out;
                               })
        .def("__getitem__",
             [](const NodeRef& n, const std::string& key) {
                 const auto& params = n.get().params();
                 auto it = params.find(key);
                 if (it == params.end()) throw py::key_error(key);
                 return fromParam(it->second);
             })
        .def("__setitem__", [](const NodeRef& n, const std::string& key,
                               py::handle value) { n.get().setParam(key, toParam(key, value)); })
        .def("__contains__", [](const NodeRef& n, const std::string& key) { return n.get().hasParam(key); })
        .def(
            "get",
            [](const NodeRef& n, const std::string& key, py::object fallback) {
                const auto& params = n.get().params();
                auto it = params.find(key);
                return it == params.end() ? fallback : fromParam(it->second);
            },
            "key"_a, "default"_a = py::none())
        .def(
            "set",
            [](const NodeRef& n, const py::kwargs& params) {
                Node& node = n.get();
                // Convert everything first, so a bad value changes nothing.
                std::vector<std::pair<std::string, ParamValue>> values;
                for (const auto& [k, v] : params) {
                    const auto key = k.cast<std::string>();
                    values.emplace_back(key, toParam(key, v));
                }
                for (auto& [key, value] : values) node.setParam(key, std::move(value));
                return n;
            },
            "Sets several parameters: node.set(gain=1.2, mix=0.5). Returns the node.")
        .def_property_readonly(
            "param_specs",
            [](const NodeRef& n) {
                static const char* kinds[] = {"bool", "int", "float", "str", "file", "choice", "vec2", "color"};
                py::list out;
                for (const auto& s : n.get().paramSpecs()) {
                    py::dict d;
                    d["name"] = s.name;
                    d["kind"] = kinds[static_cast<int>(s.kind)];
                    d["default"] = fromParam(s.defaultValue);
                    d["label"] = s.label.empty() ? s.name : s.label;
                    d["hint"] = s.hint;
                    if (s.kind == comp::ParamSpec::Kind::Choice) {
                        d["options"] = s.options;
                        d["open_choice"] = s.openChoice;
                    }
                    d["range"] = py::make_tuple(s.min, s.max);
                    d["optional"] = s.optional;
                    out.append(d);
                }
                return out;
            },
            "The parameters this node type understands, as dicts: name, kind, default, label, hint, ...")
        .def("clear", [](const NodeRef& n, const std::string& key) { n.get().clearParam(key); }, "key"_a,
             "Removes a parameter, so the node uses its default again.")
        .def_property(
            "position",
            [](const NodeRef& n) -> py::object {
                const auto& p = n.get().position();
                return p ? py::object(py::make_tuple((*p)[0], (*p)[1])) : py::object(py::none());
            },
            [](const NodeRef& n, py::object v) {
                if (v.is_none()) return n.get().setPosition(std::nullopt);
                const auto t = v.cast<std::pair<double, double>>();
                n.get().setPosition(Vec2{t.first, t.second});
            },
            "Where the node sits in a node graph editor, or None. Saved in projects.")
        .def_property_readonly("params",
                               [](const NodeRef& n) {
                                   py::dict d;
                                   for (const auto& [k, v] : n.get().params()) d[py::str(k)] = fromParam(v);
                                   return d;
                               },
                               "A copy of all parameters as a dict.")
        .def(
            "execute",
            [](const NodeRef& n, PyEngine& engine, double frame) {
                const auto* write = dynamic_cast<const comp::WriteNode*>(&n.get());
                if (!write) throw py::type_error(n.get().name() + " is a " + n.get().typeName() +
                                                 " node; only Write nodes can be executed");
                std::lock_guard lock(engine.mutex);  // the GIL stays held: execute() snapshots part-way
                return write->execute(engine.engine, context(*n.graph, frame));
            },
            "engine"_a, "frame"_a = 1.0, "Write nodes only: renders and writes one frame. Returns the path written.")
        .def("__eq__", [](const NodeRef& a, const NodeRef& b) { return a.node == b.node && a.id == b.id; })
        .def("__hash__", [](const NodeRef& n) { return py::hash(py::int_(n.id)); })
        .def("__repr__", [](const NodeRef& n) {
            try {
                const Node& node = n.get();
                return "<" + std::string(node.typeName()) + " node '" + node.name() + "'>";
            } catch (const Error&) {
                return std::string("<removed node>");
            }
        });

    // ------------------------------------------------------------ ColorConfig

    using ConfigPtr = std::shared_ptr<comp::ColorConfig>;
    auto mut = [](std::shared_ptr<const comp::ColorConfig> c) { return std::const_pointer_cast<comp::ColorConfig>(c); };
    py::class_<comp::ColorConfig, ConfigPtr>(m, "ColorConfig", R"(An OpenColorIO config.

Pixels inside a graph are in the working space (the config's scene_linear
role). Read converts into it, Write out of it, the ColorSpace node between
any two spaces. The current config is $OCIO if set, else comp's built-in one.
Color space names may also be OCIO roles ("scene_linear", "matte_paint").)")
        .def_static("builtin", [mut] { return mut(comp::ColorConfig::builtin()); })
        .def_static("from_file", [mut](const std::string& path) { return mut(comp::ColorConfig::fromFile(path)); },
                    "path"_a)
        .def_static("from_environment", [mut] { return mut(comp::ColorConfig::fromEnvironment()); },
                    "$OCIO if set and not empty, otherwise the built-in config.")
        .def_static("current", [mut] { return mut(comp::ColorConfig::current()); }, "The config nodes use.")
        .def_static(
            "set_current", [](const ConfigPtr& c) { comp::ColorConfig::setCurrent(c); }, "config"_a,
            "Makes `config` current. None: reload from the environment on next use.")
        .def_property_readonly("source", &comp::ColorConfig::source, "The file it came from, or 'built-in'.")
        .def_property_readonly("color_spaces", &comp::ColorConfig::colorSpaces)
        .def_property_readonly("working_space", &comp::ColorConfig::workingSpace)
        .def("has", &comp::ColorConfig::has, "name"_a)
        .def("resolve", &comp::ColorConfig::resolve, "name"_a, "The color space a name or role refers to.")
        .def("__repr__", [](const comp::ColorConfig& c) { return "<ColorConfig '" + c.source() + "'>"; });

    // ------------------------------------------------------------ Graph

    py::class_<Graph, std::shared_ptr<Graph>>(m, "Graph", "Owns nodes and their connections.")
        .def(py::init<>())
        .def_static(
            "load",
            [](const std::filesystem::path& path) { return std::make_shared<Graph>(comp::loadProject(path)); },
            "path"_a, "Reads a project file (.comp).")
        .def_static(
            "from_json",
            [](const std::string& text) { return std::make_shared<Graph>(comp::projectFromJson(text)); },
            "text"_a, "Reads a project from a JSON string.")
        .def("save", [](const Graph& g, const std::filesystem::path& path) { comp::saveProject(g, path); },
             "path"_a, "Writes the graph as a project file. The file is replaced only once fully written.")
        .def("to_json", &comp::projectToJson, "The project file text for this graph.")
        .def_property(
            "frame_range", [](const Graph& g) { return std::make_pair(g.settings().firstFrame, g.settings().lastFrame); },
            [](Graph& g, std::pair<int, int> r) {
                comp::ProjectSettings s = g.settings();
                s.firstFrame = r.first;
                s.lastFrame = r.second;
                g.setSettings(s);
            },
            "The project's (first, last) frame, inclusive. comp_render renders it by default.")
        .def_property(
            "fps", [](const Graph& g) { return g.settings().fps; },
            [](Graph& g, double fps) {
                comp::ProjectSettings s = g.settings();
                s.fps = fps;
                g.setSettings(s);
            },
            "Playback speed in frames per second.")
        .def_property(
            "format", [](const Graph& g) { return std::make_pair(g.settings().format.width, g.settings().format.height); },
            [](Graph& g, std::pair<int, int> wh) {
                comp::ProjectSettings s = g.settings();
                s.format = {wh.first, wh.second};
                g.setSettings(s);
            },
            "The project's (width, height): what generators such as Constant produce. Renders use it.")
        .def(
            "create",
            [](const std::shared_ptr<Graph>& g, const std::string& type, std::optional<std::string> name,
               const py::kwargs& params) {
                auto node = comp::createNode(type, name ? *name : freeName(*g, type));
                for (const auto& [k, v] : params) {
                    const auto key = k.cast<std::string>();
                    node->setParam(key, toParam(key, v));
                }
                return refTo(g, g->add(std::move(node)));
            },
            "type"_a, "name"_a = py::none(),
            R"(Adds a node of `type` (see comp.node_types()). Without a name it gets the
first free one of "Type1", "Type2", ... Keyword arguments set parameters.)")
        .def("find", [](const std::shared_ptr<Graph>& g, const std::string& name) { return optionalRef(g, g->find(name)); },
             "name"_a, "The node called `name`, or None.")
        .def("__getitem__",
             [](const std::shared_ptr<Graph>& g, const std::string& name) {
                 Node* n = g->find(name);
                 if (!n) throw py::key_error(name);
                 return refTo(g, *n);
             })
        .def("__contains__", [](const Graph& g, const std::string& name) { return g.find(name) != nullptr; })
        .def("__len__", [](const Graph& g) { return g.nodes().size(); })
        .def_property_readonly("nodes",
                               [](const std::shared_ptr<Graph>& g) {
                                   std::vector<NodeRef> out;
                                   for (const auto& n : g->nodes()) out.push_back(refTo(g, *n));
                                   return out;
                               })
        .def(
            "connect",
            [](Graph& g, const NodeRef& source, const NodeRef& target, int input) {
                g.connect(source.get(), target.get(), input);
            },
            "source"_a, "target"_a, "input"_a = 0, "Feeds `source` into input `input` of `target`.")
        .def(
            "disconnect", [](Graph& g, const NodeRef& target, int input) { g.disconnect(target.get(), input); },
            "target"_a, "input"_a = 0)
        .def(
            "remove", [](Graph& g, const NodeRef& node) { g.remove(node.get()); }, "node"_a,
            "Deletes the node, disconnecting anything that used it. Handles to it stop working.")
        .def(
            "consumers",
            [](const std::shared_ptr<Graph>& g, const NodeRef& node) {
                std::vector<NodeRef> out;
                for (Node* c : g->consumers(node.get())) out.push_back(refTo(g, *c));
                return out;
            },
            "node"_a, "Nodes that use `node` as an input.");
}
