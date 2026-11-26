// Tests for OpenFX hosting, using the plugins in tests/ofx/TestPlugins.cpp.
// ctest points $OFX_PLUGIN_PATH at the folder holding their bundle (and a
// deliberately broken one).

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "TestUtil.h"
#include "puke/engine/Engine.h"
#include "puke/graph/Graph.h"
#include "puke/nodes/Registry.h"
#include "puke/ofx/OfxHost.h"
#include "puke/ofx/Overlay.h"
#include "puke/project/Project.h"

using namespace puke;
using test::SourceNode;

namespace {

const ChannelList kRGBA{"R", "G", "B", "A"};

// A frame of 40x24 whose display window doesn't start at 0, with a data
// window that pokes outside it: catches mistakes in the y flip.
const Box kFormat{0, 4, 40, 28};
const Box kData{-3, 2, 30, 26};

float pattern(int x, int y, int c) { return c == 3 ? 0.5f + 0.01f * x : 0.01f * x + 0.03f * y + 0.1f * c; }

SourceNode& source(Graph& g, const std::string& name = "Src") {
    return g.create<SourceNode>(name, kData, kRGBA, pattern, kFormat);
}

Node& ofxNode(Graph& g, const std::string& id, const std::string& name) {
    return g.add(createNode("ofx:" + id, name));
}

bool samePixels(const Image& a, const Image& b) {
    if (a.region() != b.region() || a.channels() != b.channels()) return false;
    return std::equal(a.data(), a.data() + static_cast<std::size_t>(a.region().area()) * a.numChannels(), b.data());
}

}  // namespace

static void testDiscovery() {
    const auto plugins = ofx::plugins();
    std::vector<std::string> ids;
    for (const auto& p : plugins) ids.push_back(p.identifier);
    for (const char* id : {"org.puke.test.ByteOnly", "org.puke.test.Coords", "org.puke.test.Gain", "org.puke.test.Generator",
                           "org.puke.test.MaskMultiply", "org.puke.test.Point", "org.puke.test.Shift",
                           "org.puke.test.Transition"})
        CHECK(std::find(ids.begin(), ids.end(), id) != ids.end());
    CHECK(std::is_sorted(ids.begin(), ids.end()));
    for (const auto& p : plugins)
        if (p.identifier == "org.puke.test.Shift") CHECK(p.versionMajor == 1 && p.versionMinor == 2);

    // A broken bundle is reported, and doesn't stop the others loading.
    bool reported = false;
    for (const auto& msg : ofx::scanProblems()) reported = reported || msg.find("Broken.ofx.bundle") != std::string::npos;
    CHECK(reported);

    const auto types = nodeTypes();
    CHECK(std::find(types.begin(), types.end(), "ofx:org.puke.test.Gain") != types.end());

    CHECK_THROWS(createNode("ofx:org.puke.test.Nope", "X"), "no OFX plugin 'org.puke.test.Nope'");
    CHECK_THROWS(createNode("ofx:org.puke.test.Transition", "T"),
                 "has no context puke can host yet (Filter, Generator or General); it offers: OfxImageEffectContextTransition");
    CHECK_THROWS(createNode("ofx:org.puke.test.Transition", "T"), "has no context puke can host");  // remembered
    CHECK_THROWS(createNode("ofx:org.puke.test.ByteOnly", "B"), "doesn't support float images");
}

static void testNodeBasics() {
    Graph g;
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    CHECK(std::string(gain.typeName()) == "ofx:org.puke.test.Gain");
    CHECK(gain.maxInputs() == 1 && !gain.inputOptional(0));
    CHECK(gain.params().empty());  // defaults stay with the plugin

    Node& mask = ofxNode(g, "org.puke.test.MaskMultiply", "Mask1");
    CHECK(mask.maxInputs() == 2 && !mask.inputOptional(0) && mask.inputOptional(1));

    Engine e;
    CHECK_THROWS(e.render(gain, {}, kFormat), "Gain1: input 0 is not connected");
}

static void testParamSpecs() {
    Graph g;
    auto& src = source(g);
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    Node& explicitDefaults = ofxNode(g, "org.puke.test.Gain", "Gain2");
    g.connect(src, gain, 0);
    g.connect(src, explicitDefaults, 0);

    const auto specs = gain.paramSpecs();
    std::vector<std::string> names;
    for (const auto& s : specs) names.push_back(s.name);
    CHECK((names == std::vector<std::string>{"gain", "offset", "invert", "mode", "note", "fail", "count", "size"}));
    using Kind = ParamSpec::Kind;
    CHECK(specs[0].kind == Kind::Double && std::get<double>(specs[0].defaultValue) == 1.0);
    CHECK(specs[1].kind == Kind::Color && specs[2].kind == Kind::Bool);
    CHECK(specs[3].kind == Kind::Choice && (specs[3].options == std::vector<std::string>{"multiply", "add"}));
    CHECK(std::get<std::string>(specs[3].defaultValue) == "multiply");  // choices are offered by label
    CHECK(specs[4].kind == Kind::String && std::get<std::string>(specs[4].defaultValue) == "hello");
    CHECK(specs[6].kind == Kind::Int && std::get<int>(specs[6].defaultValue) == 3);
    CHECK(specs[7].kind == Kind::Vec2 && (std::get<Vec2>(specs[7].defaultValue) == Vec2{1.5, 2.5}));

    // The described defaults are the plugin's real ones.
    for (const auto& s : specs) explicitDefaults.setParam(s.name, s.defaultValue);
    Engine e;
    CHECK(samePixels(e.render(gain, {}, kData), e.render(explicitDefaults, {}, kData)));
}

static void testGain() {
    Graph g;
    auto& src = source(g);
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    g.connect(src, gain, 0);
    Engine e(EngineOptions{.threads = 4, .tileSize = 8});

    // Defaults (gain 1, nothing else): the input, unchanged, same windows.
    const Image in = e.render(src, {}, kData);
    CHECK(e.info(gain, {}).dataWindow == kData);
    CHECK(e.info(gain, {}).displayWindow == kFormat);
    CHECK(samePixels(e.render(gain, {}, kData), in));

    // Every parameter type, set the ways a user would.
    gain.setParam("gain", 2.0);
    gain.setParam("offset", Color{0.1, 0.2, 0.3, 0.25});
    gain.setParam("count", 7);
    gain.setParam("note", std::string("world"));
    const Image out = e.render(gain, {}, kData);
    bool ok = true;
    for (int y = kData.y0; y < kData.y1; ++y)
        for (int x = kData.x0; x < kData.x1; ++x) {
            const float* p = out.pixel(x, y);
            for (int c = 0; c < 3; ++c) ok = ok && std::fabs(p[c] - (2 * pattern(x, y, c) + 0.1f * (c + 1))) < 1e-6f;
            ok = ok && std::fabs(p[3] - (pattern(x, y, 3) + 0.25f)) < 1e-6f;
        }
    CHECK(ok);

    gain.setParam("mode", std::string("add"));  // a choice, by label
    gain.setParam("invert", true);
    const float added = 1.0f - (pattern(5, 9, 1) + 2.0f + 0.2f);
    CHECK(std::fabs(e.render(gain, {}, kData).pixel(5, 9)[1] - added) < 1e-6f);
    gain.setParam("mode", 0);  // ...or by index
    gain.setParam("invert", false);
    CHECK(std::fabs(e.render(gain, {}, kData).pixel(5, 9)[1] - (2 * pattern(5, 9, 1) + 0.2f)) < 1e-6f);

    // Tiles and threads don't change anything.
    const Image tiled = e.render(gain, {}, {-8, 0, 44, 30});
    const Image whole = Engine(EngineOptions{.threads = 1, .tileSize = 1 << 20}).render(gain, {}, {-8, 0, 44, 30});
    CHECK(samePixels(tiled, whole));
    CHECK(e.lastStats().tilesComputed > 4);
}

static void testErrors() {
    Graph g;
    auto& src = source(g);
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    g.connect(src, gain, 0);
    Engine e(EngineOptions{.threads = 4, .tileSize = 8});

    // A plugin's own error message reaches the user, with the node's name.
    gain.setParam("fail", true);
    CHECK_THROWS(e.render(gain, {}, kData),
                 "Gain1 (org.puke.test.Gain): OfxImageEffectActionRender failed: gain plugin asked to fail");
    gain.setParam("fail", false);

    // Values that don't fit the parameter.
    gain.setParam("note", 5);
    CHECK_THROWS(e.render(gain, {}, kData), "Gain1: parameter 'note' must be a string");
    gain.setParam("note", std::string("hello"));
    gain.setParam("gain", Vec2{1, 2});
    CHECK_THROWS(e.render(gain, {}, kData), "parameter 'gain' must be a number");
    gain.setParam("gain", 1.0);
    gain.setParam("mode", std::string("divide"));
    CHECK_THROWS(e.render(gain, {}, kData), "parameter 'mode' must be one of: multiply, add (or its index)");
    gain.setParam("mode", 0);
    gain.setParam("offset", Vec2{1, 2});
    CHECK_THROWS(e.render(gain, {}, kData), "parameter 'offset' must be 3 or 4 numbers (a Color)");
    gain.setParam("offset", Color{0, 0, 0, 0});

    // And it all works again afterwards.
    CHECK(samePixels(e.render(gain, {}, kData), e.render(src, {}, kData)));
}

static void testShiftAndCoordinates() {
    // Shift (3, 5) in OFX coordinates: right 3, *up* 5 - which is y - 5 in
    // puke's y-down coordinates. Exercises the region of definition, regions
    // of interest and the y flip together.
    Graph g;
    auto& src = source(g);
    Node& shift = ofxNode(g, "org.puke.test.Shift", "Shift1");
    shift.setParam("shift", Vec2{3, 5});
    g.connect(src, shift, 0);
    Engine e(EngineOptions{.threads = 4, .tileSize = 8});

    const Box moved{kData.x0 + 3, kData.y0 - 5, kData.x1 + 3, kData.y1 - 5};
    CHECK(e.info(shift, {}).dataWindow == moved);
    const Image out = e.render(shift, {}, moved);
    bool ok = true;
    for (int y = moved.y0; y < moved.y1; ++y)
        for (int x = moved.x0; x < moved.x1; ++x)
            for (int c = 0; c < 4; ++c) ok = ok && out.pixel(x, y)[c] == pattern(x - 3, y + 5, c);
    CHECK(ok);

    // A small region asks the input only for what it needs, moved back.
    src.resetProbe();
    e.render(shift, {}, {10, 10, 14, 12});
    CHECK(src.requested == (Box{7, 15, 11, 17}));
}

static void testCoordinateConvention() {
    // puke's y-down pixel rows are flipped around the display window, which
    // lands on the same numbers in OFX: display window y 4..28 (puke) is
    // OFX project y 4..28, top row 4 becoming OFX row 27. For a frame
    // starting at 0 that's OFX's usual bottom-left origin, as in Nuke.
    Graph g;
    auto& src = source(g);
    Node& coords = ofxNode(g, "org.puke.test.Coords", "Coords1");
    g.connect(src, coords, 0);
    const Image out = Engine(EngineOptions{.threads = 4, .tileSize = 8}).render(coords, {}, kData);
    bool ok = true;
    for (int y = kData.y0; y < kData.y1; ++y)
        for (int x = kData.x0; x < kData.x1; ++x) {
            const float* p = out.pixel(x, y);
            ok = ok && p[0] == float(x) && p[1] == float(kFormat.y0 + kFormat.y1 - 1 - y);
            ok = ok && p[2] == float(kFormat.y0) && p[3] == float(kFormat.y1);  // project y range
        }
    CHECK(ok);
}

static void testMaskAndThreadSafety() {
    Graph g;
    auto& src = source(g);
    auto& matte = g.create<SourceNode>("Matte", Box{0, 0, 20, 30}, ChannelList{"R", "G", "B", "A"},
                                       [](int x, int, int c) { return c == 3 ? (x < 10 ? 1.0f : 0.25f) : 9.0f; },
                                       kFormat);
    Node& mul = ofxNode(g, "org.puke.test.MaskMultiply", "Mask1");
    g.connect(src, mul, 0);
    Engine e(EngineOptions{.threads = 8, .tileSize = 4});

    // No mask connected (it's optional): the source, unchanged.
    CHECK(samePixels(e.render(mul, {}, kData), e.render(src, {}, kData)));

    // With a mask: only its alpha is used, and outside it the mask is 0.
    g.connect(matte, mul, 1);
    const Image out = e.render(mul, {}, kData);
    CHECK(std::fabs(out.pixel(5, 6)[0] - pattern(5, 6, 0)) < 1e-6f);
    CHECK(std::fabs(out.pixel(15, 6)[2] - 0.25f * pattern(15, 6, 2)) < 1e-6f);
    CHECK(out.pixel(25, 6)[1] == 0.0f);
    CHECK(!mul.supportsTiles());  // the plugin said no tiles: it gets each region in one call

    // The plugin says it isn't thread safe: separate engines, graphs and
    // instances rendering at once must still never overlap inside it.
    std::atomic<int> failures = 0;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back([&] {
            Graph tg;
            auto& s = source(tg);
            Node& m = ofxNode(tg, "org.puke.test.MaskMultiply", "Mask1");
            tg.connect(s, m, 0);
            Engine te(EngineOptions{.threads = 2});
            for (int i = 0; i < 10; ++i) {
                try {
                    te.render(m, {double(i)}, kData);
                } catch (const std::exception& ex) {
                    std::fprintf(stderr, "%s\n", ex.what());
                    ++failures;
                }
            }
        });
    for (auto& t : threads) t.join();
    CHECK(failures == 0);
}

// A generator: no inputs, the project's format as its frame.
static void testGenerator() {
    Graph g;
    Node& gen = ofxNode(g, "org.puke.test.Generator", "Gen1");
    CHECK(gen.maxInputs() == 0);
    Engine e({.threads = 4, .tileSize = 16, .cacheBytes = 64 << 20});
    const RenderContext ctx{1, Format{64, 32}};
    const ImageInfo info = e.info(gen, ctx);
    CHECK((info.displayWindow == Box{0, 0, 64, 32}) && (info.dataWindow == Box{0, 0, 64, 32}));
    Image img = e.render(gen, ctx, info.dataWindow);
    // OFX's y points up: puke's top row is OFX row 31.
    const float* p = img.pixel(5, 0);
    CHECK(p[0] == 0.5f && p[1] == 5.0f && p[2] == 31.0f && p[3] == 32.0f);
    CHECK(img.pixel(63, 31)[1] == 63.0f && img.pixel(63, 31)[2] == 0.0f);
    // Tiles on several threads match one piece.
    Engine whole({.threads = 0, .tileSize = 1 << 20, .cacheBytes = 0});
    CHECK(samePixels(img, whole.render(gen, ctx, info.dataWindow)));

    // Parameters, and a new format (part of the cache key: not stale tiles).
    gen.setParam("color", Color{0.25, 0, 0, 1});
    const RenderContext small{1, Format{20, 10}};
    img = e.render(gen, small, e.info(gen, small).dataWindow);
    CHECK((img.region() == Box{0, 0, 20, 10}));
    CHECK(img.pixel(0, 0)[0] == 0.25f && img.pixel(0, 0)[2] == 9.0f && img.pixel(0, 0)[3] == 10.0f);

    // Feeding a filter.
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    g.connect(gen, gain, 0);
    gain.setParam("gain", 2.0);
    CHECK(e.render(gain, small, {0, 0, 1, 1}).pixel(0, 0)[0] == 0.5f);
}

// Proxy renders: plugins get the render scale, regions in canonical
// (full-size) coordinates and images at the render scale.
static void testRenderScale() {
    Graph g;
    auto& src = source(g);
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    g.connect(src, gain, 0);
    gain.setParam("gain", 2.0);
    Node& shift = ofxNode(g, "org.puke.test.Shift", "Shift1");
    g.connect(gain, shift, 0);
    shift.setParam("shift", Vec2{4, -2});  // canonical pixels: 2, -1 at half size
    Engine e;
    const RenderContext full{1, Format{40, 24}, 1.0}, half{1, Format{40, 24}, 0.5};
    // The test SourceNode ignores the scale (its windows stay put), so this
    // checks the round trip through canonical coordinates: its data window,
    // doubled to canonical, shifted (4, -2) by the plugin (y up), halved
    // back: moved (2, +1).
    const ImageInfo hi = e.info(shift, half);
    CHECK((hi.dataWindow == Box{kData.x0 + 2, kData.y0 + 1, kData.x1 + 2, kData.y1 + 1}));
    (void)full;
    const Image in = e.render(src, half, hi.dataWindow);
    const Image out = e.render(shift, half, hi.dataWindow);
    // Shifted by (2, -1) pixels at half size; OFX y is up, so puke y moves +1.
    CHECK(std::fabs(out.pixel(5, 5)[0] - 2 * in.pixel(3, 4)[0]) < 1e-6f);

    // A generator: its frame is the format at the scale; the project size it
    // sees is full size; render coordinates are pixels at the scale.
    Node& gen = ofxNode(g, "org.puke.test.Generator", "Gen1");
    const RenderContext ctx{1, Format{64, 32}, 0.5};
    const ImageInfo gi = e.info(gen, ctx);
    CHECK((gi.displayWindow == Box{0, 0, 32, 16}) && (gi.dataWindow == Box{0, 0, 32, 16}));
    const Image img = e.render(gen, ctx, gi.dataWindow);
    CHECK(img.pixel(5, 0)[1] == 5.0f && img.pixel(5, 0)[2] == 15.0f && img.pixel(5, 0)[3] == 32.0f);
}

// Overlays: pen and key events reach the plugin in canonical coordinates,
// and values it sets go to the edit callback (or onto the node).
static void testOverlay() {
    Graph g;
    auto& src = source(g);
    Node& point = ofxNode(g, "org.puke.test.Point", "Point1");
    g.connect(src, point, 0);
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    CHECK(!ofx::Overlay::create(gain) && !ofx::Overlay::create(src));  // no overlay; not OFX
    auto overlay = ofx::Overlay::create(point);
    CHECK(overlay != nullptr);
    if (!overlay) return;

    ofx::Overlay::View view;
    view.flipY = kFormat.y0 + kFormat.y1;  // 32: canonical y = 32 - puke y
    view.time = 1;
    // The point defaults to canonical (10, 10): puke (10, 22).
    CHECK(!overlay->penDown(view, 30, 5));  // nowhere near: not the plugin's
    CHECK(overlay->penDown(view, 11, 21));
    std::vector<std::pair<std::string, ParamValue>> edits;
    auto record = [&](const std::string& k, ParamValue v) { edits.emplace_back(k, std::move(v)); };
    CHECK(overlay->penMotion(view, 25, 12, record));
    CHECK(edits.size() == 1 && edits[0].first == "point" && (std::get<Vec2>(edits[0].second) == Vec2{25, 20}));
    CHECK(!point.hasParam("point"));  // went to the callback, not the node
    CHECK(overlay->takeRedrawRequest() && !overlay->takeRedrawRequest());
    // Without a callback, onto the node.
    CHECK(overlay->penMotion(view, 26, 12));
    CHECK((std::get<Vec2>(point.params().at("point")) == Vec2{26, 20}));
    CHECK(overlay->penUp(view, 26, 12));
    CHECK(!overlay->penMotion(view, 30, 12));  // not dragging any more

    // Keys: R resets.
    CHECK(!overlay->keyDown(view, ofx::Overlay::Key::Character, "q"));
    CHECK(overlay->keyDown(view, ofx::Overlay::Key::Character, "r"));
    CHECK((std::get<Vec2>(point.params().at("point")) == Vec2{10, 10}));

    // The plugin sees animated values at the view's time.
    Curve c;
    c.setKey(1, Vec2{0, 0}, Curve::Interp::Linear);
    c.setKey(11, Vec2{20, 20}, Curve::Interp::Linear);
    point.setCurve("point", c);
    view.time = 6;  // (10, 10): puke (10, 22)
    CHECK(overlay->penDown(view, 10, 22));
    CHECK(overlay->penUp(view, 10, 22));
    view.time = 11;
    CHECK(!overlay->penDown(view, 10, 22));
    CHECK(overlay->penDown(view, 20, 12));

    // Canonical rectangles for the GL projection: y flipped.
    const auto r = ofx::Overlay::canonicalRect(view, 0, 4, 40, 28);
    CHECK(r[0] == 0 && r[1] == 4 && r[2] == 40 && r[3] == 28);
}

static void testCacheAndProjects() {
    Graph g;
    auto& src = source(g);
    Node& gain = ofxNode(g, "org.puke.test.Gain", "Gain1");
    gain.setParam("gain", 0.5);
    g.connect(src, gain, 0);

    Engine e(EngineOptions{.threads = 4, .tileSize = 8, .cacheBytes = 16 << 20});
    const Image first = e.render(gain, {}, kData);
    e.render(gain, {}, kData);
    CHECK(e.lastStats().tilesComputed == 0);  // cached
    gain.setParam("gain", 0.75);
    e.render(gain, {}, kData);
    CHECK(e.lastStats().tilesComputed > 0);  // a parameter change recomputes

    // OFX nodes save and load like any other.
    gain.setParam("gain", 0.5);
    Graph pg;
    auto& psrc = source(pg);
    Node& pgain = ofxNode(pg, "org.puke.test.Gain", "Gain1");
    pgain.setParam("gain", 0.5);
    pg.connect(psrc, pgain, 0);
    const std::string json = projectToJson(pg);
    CHECK(json.find("\"type\": \"ofx:org.puke.test.Gain\"") != std::string::npos);
    CHECK(json.find("\"gain\": 0.5") != std::string::npos);
    CHECK(json.find("\"offset\"") == std::string::npos);  // unset parameters aren't written

    // (Source nodes are test-only, so rebuild the input by hand after loading.)
    Graph loaded = projectFromJson(R"({"format": "puke", "version": 1, "nodes": [
        {"name": "Gain1", "type": "ofx:org.puke.test.Gain", "params": {"gain": 0.5}}]})");
    auto& lsrc = source(loaded);
    loaded.connect(lsrc, *loaded.find("Gain1"), 0);
    CHECK(samePixels(Engine().render(*loaded.find("Gain1"), {}, kData), first));
}

int main() {
    try {
        testDiscovery();
        testNodeBasics();
        testGain();
        testParamSpecs();
        testErrors();
        testShiftAndCoordinates();
        testCoordinateConvention();
        testMaskAndThreadSafety();
        testGenerator();
        testRenderScale();
        testOverlay();
        testCacheAndProjects();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    return test::finish("OFX");
}
