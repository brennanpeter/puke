// Tests for Graph and Engine, using small test-only nodes.

#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "TestUtil.h"
#include "comp/engine/Engine.h"
#include "comp/engine/ThreadPool.h"
#include "comp/engine/TileCache.h"
#include "comp/graph/Graph.h"

using namespace comp;

static const Box kFormat{0, 0, 64, 32};
static const ChannelList kRGBA{"R", "G", "B", "A"};

// Instrumentation shared by the test nodes: counts calls and records the
// bounding box of every region requested. The engine calls compute() from
// several threads (one call per tile), so updates take a lock. mutable
// because compute() is const; real nodes must not do this.
struct Probe {
    mutable std::mutex probeMutex;
    mutable int computeCalls = 0;
    mutable long long pixelsComputed = 0;
    mutable Box requested;

    void record(const Box& region) const {
        std::lock_guard lock(probeMutex);
        ++computeCalls;
        pixelsComputed += region.area();
        requested = requested.unite(region);
    }
    // Every requested pixel was computed exactly once (tiles don't overlap).
    void resetProbe() const {
        std::lock_guard lock(probeMutex);
        computeCalls = 0;
        pixelsComputed = 0;
        requested = {};
    }
    bool computedOnce() const { return computeCalls > 0 && pixelsComputed == requested.area(); }
};

// Solid colour (all channels) inside `box`, zero outside. The colour is the
// "value" parameter, so tests can edit it like a user would.
class ConstantNode : public Node, public Probe {
public:
    ConstantNode(std::string name, float value, Box box) : Node(std::move(name)), box_(box) {
        setParam("value", static_cast<double>(value));
    }
    const char* typeName() const override { return "Constant"; }

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const>) const override {
        return {box_, kFormat, kRGBA};
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const>) const override {
        record(region);
        Image out(region, kRGBA);
        const float value = static_cast<float>(numberParam("value", 0.0));
        const Box fill = region.intersect(box_);
        for (int y = fill.y0; y < fill.y1; ++y)
            for (int x = fill.x0; x < fill.x1; ++x)
                for (int c = 0; c < 4; ++c) out.pixel(x, y)[c] = value;
        return out;
    }

private:
    Box box_;
};

// A + B per channel. B is optional.
class AddNode : public Node, public Probe {
public:
    using Node::Node;
    const char* typeName() const override { return "Add"; }
    int maxInputs() const override { return 2; }
    bool inputOptional(int index) const override { return index == 1; }

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const> in) const override {
        ImageInfo info = *in[0];
        if (in[1]) info.dataWindow = info.dataWindow.unite(in[1]->dataWindow);
        return info;
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const> in) const override {
        record(region);
        Image out(region, in[0]->channels());
        for (int y = region.y0; y < region.y1; ++y)
            for (int x = region.x0; x < region.x1; ++x)
                for (int c = 0; c < out.numChannels(); ++c)
                    out.pixel(x, y)[c] = in[0]->at(x, y, c) + (in[1] ? in[1]->at(x, y, c) : 0.0f);
        return out;
    }
};

// Horizontal box blur: needs `radius` extra pixels left and right of its output.
class BlurXNode : public Node, public Probe {
public:
    BlurXNode(std::string name, int radius) : Node(std::move(name)), radius_(radius) {}
    const char* typeName() const override { return "BlurX"; }
    int maxInputs() const override { return 1; }

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const> in) const override {
        ImageInfo info = *in[0];
        const Box& d = info.dataWindow;
        if (!d.empty()) info.dataWindow = {d.x0 - radius_, d.y0, d.x1 + radius_, d.y1};
        return info;
    }
    Box inputRegion(const RenderContext&, int, const Box& r) const override {
        return {r.x0 - radius_, r.y0, r.x1 + radius_, r.y1};
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const> in) const override {
        record(region);
        const Image& src = *in[0];
        Image out(region, src.channels());
        const float norm = 1.0f / (2 * radius_ + 1);
        for (int y = region.y0; y < region.y1; ++y)
            for (int x = region.x0; x < region.x1; ++x)
                for (int c = 0; c < out.numChannels(); ++c) {
                    float sum = 0;
                    for (int k = -radius_; k <= radius_; ++k) sum += src.at(x + k, y, c);
                    out.pixel(x, y)[c] = sum * norm;
                }
        return out;
    }

private:
    int radius_;
};

// Deliberately breaks the compute() contract.
class BrokenNode : public Node {
public:
    using Node::Node;
    const char* typeName() const override { return "Broken"; }
    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const>) const override {
        return {kFormat, kFormat, kRGBA};
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const>) const override {
        return Image(region.padded(1), kRGBA);
    }
};

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

static void testRegionOfInterest() {
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, Box{10, 0, 20, 32});
    auto& blur = g.create<BlurXNode>("Blur1", 2);
    g.connect(c, blur, 0);

    Engine e;
    const Box region{5, 4, 12, 6};
    Image img = e.render(blur, {}, region);

    CHECK(img.region() == region);
    // Blur's data window starts at x=8, so it computes only x 8..12. It needs
    // 2 more pixels each side (6..14), clipped to the constant's box (10..20).
    CHECK(blur.requested == (Box{8, 4, 12, 6}));
    CHECK(c.requested == (Box{10, 4, 14, 6}));

    // Pixels: x=7 sees x5..9 (all outside, 0); x=10 sees 8..12 (3 inside); x=11 sees 4 inside.
    CHECK(near(img.pixel(7, 5)[0], 0.0f));
    CHECK(near(img.pixel(10, 5)[0], 3.0f / 5));
    CHECK(near(img.pixel(11, 5)[1], 4.0f / 5));

    CHECK(e.info(blur, {}).dataWindow == (Box{8, 0, 22, 32}));
}

static void testSharedInputComputedOnce() {
    // Constant feeds both Blur and Add; Add also reads Blur:
    //   Constant ──► Blur ──► Add(A)
    //       └───────────────► Add(B)
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 0.5f, kFormat);
    auto& blur = g.create<BlurXNode>("Blur1", 3);
    auto& add = g.create<AddNode>("Add1");
    g.connect(c, blur, 0);
    g.connect(blur, add, 0);
    g.connect(c, add, 1);

    Engine e;
    const Box region{20, 10, 30, 12};
    Image img = e.render(add, {}, region);

    CHECK(c.computedOnce());
    CHECK(blur.computedOnce());
    CHECK(add.computedOnce());
    // Union of Blur's padded request and Add's direct request.
    CHECK(c.requested == (Box{17, 10, 33, 12}));
    CHECK(e.lastStats().nodesComputed == 3);
    CHECK(e.lastStats().pixelsComputed == 32 + 20 + 20);
    CHECK(near(img.pixel(25, 11)[3], 1.0f));  // blur(0.5) + 0.5
}

// Moves its input by `dx` pixels. Used to make two consumers want different
// parts of the same input.
class ShiftXNode : public Node {
public:
    ShiftXNode(std::string name, int dx) : Node(std::move(name)), dx_(dx) {}
    const char* typeName() const override { return "ShiftX"; }
    int maxInputs() const override { return 1; }
    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const> in) const override {
        ImageInfo info = *in[0];
        const Box& d = info.dataWindow;
        if (!d.empty()) info.dataWindow = {d.x0 + dx_, d.y0, d.x1 + dx_, d.y1};
        return info;
    }
    Box inputRegion(const RenderContext&, int, const Box& r) const override {
        return {r.x0 - dx_, r.y0, r.x1 - dx_, r.y1};
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const> in) const override {
        Image out(region, in[0]->channels());
        for (int y = region.y0; y < region.y1; ++y)
            for (int x = region.x0; x < region.x1; ++x)
                for (int c = 0; c < out.numChannels(); ++c) out.pixel(x, y)[c] = in[0]->at(x - dx_, y, c);
        return out;
    }

private:
    int dx_;
};

static void testDisjointRequestsAreCombined() {
    // Constant ──► ShiftX(+20) ──► Add(A)
    //     └──────► ShiftX(-20) ──► Add(B)
    // Rendering x 30..34 needs the constant at x 10..14 (left branch) and
    // 50..54 (right branch): two separate areas. Both must be computed.
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, kFormat);
    auto& right = g.create<ShiftXNode>("Right", 20);
    auto& left = g.create<ShiftXNode>("Left", -20);
    auto& add = g.create<AddNode>("Add1");
    g.connect(c, right, 0);
    g.connect(c, left, 0);
    g.connect(right, add, 0);
    g.connect(left, add, 1);

    Engine e;
    Image img = e.render(add, {}, {30, 5, 34, 6});
    CHECK(c.computedOnce());
    CHECK(c.requested == (Box{10, 5, 54, 6}));  // the union of both requests
    CHECK(near(img.pixel(30, 5)[0], 2.0f));
    CHECK(near(img.pixel(33, 5)[0], 2.0f));
}

static void testClipsToDataWindow() {
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, Box{10, 10, 20, 20});
    auto& blur = g.create<BlurXNode>("Blur1", 1);
    g.connect(c, blur, 0);

    Engine e;
    const Box region{0, 0, 1000, 1000};
    Image img = e.render(blur, {}, region);
    CHECK(img.region() == region);  // caller still gets exactly what they asked for
    CHECK(c.requested == (Box{10, 10, 20, 20}));
    CHECK(blur.requested == (Box{9, 10, 21, 20}));
    CHECK(e.lastStats().pixelsComputed == 100 + 120);
    CHECK(near(img.pixel(15, 15)[0], 1.0f));
    CHECK(near(img.pixel(500, 500)[0], 0.0f));

    // Entirely outside: nothing is computed at all.
    e.render(blur, {}, {100, 100, 200, 200});
    CHECK(e.lastStats().nodesComputed == 0);
}

static void testOptionalAndMissingInputs() {
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 2.0f, kFormat);
    auto& add = g.create<AddNode>("Add1");
    Engine e;

    std::string err = test::errorFrom([&] { e.render(add, {}, {0, 0, 1, 1}); });
    CHECK(err.find("Add1") != std::string::npos);
    CHECK(err.find("input 0 is not connected") != std::string::npos);

    g.connect(c, add, 0);  // B left empty: it's optional
    CHECK(near(e.render(add, {}, {0, 0, 1, 1}).pixel(0, 0)[0], 2.0f));
}

static void testCycles() {
    Graph g;
    auto& a = g.create<BlurXNode>("BlurA", 1);
    auto& b = g.create<BlurXNode>("BlurB", 1);
    g.connect(a, b, 0);
    CHECK(!test::errorFrom([&] { g.connect(b, a, 0); }).empty());
    CHECK(!test::errorFrom([&] { g.connect(a, a, 0); }).empty());
    CHECK(a.input(0) == nullptr);

    // Bypassing Graph with setInput: the engine still refuses to loop forever.
    a.setInput(0, &b);
    Engine e;
    CHECK(test::errorFrom([&] { e.render(b, {}, {0, 0, 1, 1}); }).find("cycle") != std::string::npos);
}

static void testGraphBookkeeping() {
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, kFormat);
    auto& add = g.create<AddNode>("Add1");
    g.connect(c, add, 0);
    g.connect(c, add, 1);

    CHECK(g.find("Add1") == &add);
    CHECK(g.find("Nope") == nullptr);
    CHECK(!test::errorFrom([&] { g.create<AddNode>("Add1"); }).empty());
    CHECK(!test::errorFrom([&] { g.connect(c, add, 2); }).empty());
    CHECK(g.consumers(c).size() == 1);

    g.remove(c);
    CHECK(add.input(0) == nullptr && add.input(1) == nullptr);
    CHECK(g.nodes().size() == 1);
}

static void testContractChecks() {
    Graph g;
    auto& broken = g.create<BrokenNode>("Broken1");
    Engine e;
    CHECK(test::errorFrom([&] { e.render(broken, {}, {0, 0, 4, 4}); }).find("Broken1") != std::string::npos);

    // Empty requests are fine and produce empty images.
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, kFormat);
    CHECK(e.render(c, {}, Box{}).region().empty());
}

static void testDeepChain() {
    // Long chains must not overflow the stack while planning.
    Graph g;
    Node* prev = &g.create<ConstantNode>("Constant1", 1.0f, kFormat);
    for (int i = 0; i < 5000; ++i) {
        auto& add = g.create<AddNode>("Add" + std::to_string(i));
        g.connect(*prev, add, 0);
        prev = &add;
    }
    Engine e;
    CHECK(near(e.render(*prev, {}, {0, 0, 1, 1}).pixel(0, 0)[0], 1.0f));
    CHECK(e.lastStats().nodesComputed == 5001);
}

// Throws a plain std::exception (as OIIO or the standard library might) from
// whichever hook `stage` names.
class ThrowingNode : public Node {
public:
    ThrowingNode(std::string name, std::string stage) : Node(std::move(name)), stage_(std::move(stage)) {}
    const char* typeName() const override { return "Throwing"; }
    int maxInputs() const override { return 1; }
    bool inputOptional(int) const override { return true; }
    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const>) const override {
        if (stage_ == "info") throw std::runtime_error("boom in info");
        return {kFormat, kFormat, kRGBA};
    }
    Box inputRegion(const RenderContext&, int, const Box& r) const override {
        if (stage_ == "region") throw std::runtime_error("boom in region");
        return r;
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const>) const override {
        if (stage_ == "render") throw std::bad_alloc();
        return Image(region, kRGBA);
    }

private:
    std::string stage_;
};

static void testForeignExceptionsNameTheNode() {
    Engine e;
    for (const char* stage : {"info", "region", "render"}) {
        Graph g;
        auto& c = g.create<ConstantNode>("Constant1", 1.0f, kFormat);
        auto& bad = g.create<ThrowingNode>("Thrower", stage);
        auto& add = g.create<AddNode>("Add1");
        g.connect(c, bad, 0);
        g.connect(bad, add, 0);
        const std::string err = test::errorFrom([&] { e.render(add, {}, {0, 0, 4, 4}); });
        CHECK(err.find("Thrower: " + std::string(stage) + " failed") != std::string::npos);
        if (err.empty()) std::fprintf(stderr, "  stage %s did not throw\n", stage);
    }
}

static void testGraphMisuse() {
    Graph g, other;
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, kFormat);
    auto& add = g.create<AddNode>("Add1");
    auto& stranger = other.create<ConstantNode>("Stranger", 1.0f, kFormat);

    CHECK_THROWS(g.create<AddNode>(""), "name cannot be empty");
    CHECK_THROWS(g.connect(stranger, add, 0), "does not belong to this graph");
    CHECK_THROWS(g.connect(c, add, -1), "out of range");
    CHECK_THROWS(g.remove(stranger), "does not belong to this graph");

    g.connect(c, add, 0);
    g.connect(c, add, 1);
    g.disconnect(add, 1);
    CHECK(add.input(0) == &c && add.input(1) == nullptr);
    g.disconnect(add, 0);
    CHECK(add.input(0) == nullptr);
    CHECK(g.consumers(c).empty());
    CHECK_THROWS(g.disconnect(stranger, 0), "does not belong to this graph");
    CHECK_THROWS(add.requireInput(0), "Add1: input 0 is not connected");
}

static void testParamAccess() {
    Graph g;
    auto& n = g.create<AddNode>("Add1");
    n.setParam("count", 3);
    n.setParam("ratio", 0.5);
    n.setParam("label", std::string("hi"));
    n.setParam("offset", Vec2{1, 2});
    n.setParam("tint", Color{0.1, 0.2, 0.3, 0.4});

    CHECK(n.param<int>("count") == 3);
    CHECK(n.param<int>("missing", 7) == 7);
    CHECK_THROWS(n.param<int>("missing"), "Add1: missing parameter 'missing'");
    CHECK_THROWS(n.param<int>("label"), "parameter 'label' has the wrong type");

    CHECK(n.numberParam("count", 0) == 3.0);  // int accepted as a number
    CHECK(n.numberParam("ratio", 0) == 0.5);
    CHECK(n.numberParam("missing", 9) == 9.0);
    CHECK_THROWS(n.numberParam("label", 0), "parameter 'label' must be a number");

    CHECK((n.vec2Param("offset", {}) == Vec2{1, 2}));
    CHECK((n.vec2Param("ratio", {}) == Vec2{0.5, 0.5}));  // one number for both
    CHECK((n.vec2Param("missing", {3, 4}) == Vec2{3, 4}));

    CHECK((n.colorParam("tint", 0) == Color{0.1, 0.2, 0.3, 0.4}));
    CHECK((n.colorParam("count", 0) == Color{3, 3, 3, 3}));
    CHECK((n.colorParam("missing", 1) == Color{1, 1, 1, 1}));
    CHECK_THROWS(n.colorParam("offset", 0), "must be a number");
}

// ---------------------------------------------------------------- tiles and threads

static void testTileGrid() {
    // Tiles sit on a fixed grid anchored at (0, 0), including for negative coordinates.
    const auto tiles = tilesOf({-5, 3, 10, 9}, 4);
    const std::vector<Box> expected{
        {-5, 3, -4, 4}, {-4, 3, 0, 4}, {0, 3, 4, 4}, {4, 3, 8, 4}, {8, 3, 10, 4},
        {-5, 4, -4, 8}, {-4, 4, 0, 8}, {0, 4, 4, 8}, {4, 4, 8, 8}, {8, 4, 10, 8},
        {-5, 8, -4, 9}, {-4, 8, 0, 9}, {0, 8, 4, 9}, {4, 8, 8, 9}, {8, 8, 10, 9},
    };
    CHECK(tiles == expected);
    CHECK((tilesOf({0, 0, 64, 64}, 64) == std::vector<Box>{{0, 0, 64, 64}}));
    CHECK(tilesOf({}, 64).empty());
    CHECK_THROWS(tilesOf({0, 0, 1, 1}, 0), "tile size must be at least 1");
    CHECK_THROWS(Engine(EngineOptions{1, 0}), "tile size must be at least 1");
}

static void testThreadPool() {
    ThreadPool pool(4);
    CHECK(pool.threads() == 4);
    CHECK(ThreadPool(1).threads() == 1);
    CHECK(ThreadPool(0).threads() >= 1);

    // Every index runs exactly once.
    std::vector<std::atomic<int>> hits(1000);
    pool.parallelFor(hits.size(), [&](std::size_t i) { ++hits[i]; });
    bool allOnce = true;
    for (auto& h : hits) allOnce = allOnce && h == 1;
    CHECK(allOnce);

    // Work really is spread over more than one thread.
    std::mutex m;
    std::vector<std::thread::id> ids;
    pool.parallelFor(64, [&](std::size_t) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        std::lock_guard lock(m);
        if (std::find(ids.begin(), ids.end(), std::this_thread::get_id()) == ids.end())
            ids.push_back(std::this_thread::get_id());
    });
    CHECK(ids.size() > 1);

    // Exceptions reach the caller, and the pool still works afterwards.
    CHECK_THROWS(pool.parallelFor(100, [](std::size_t i) {
        if (i == 37) throw Error("tile 37 broke");
    }), "tile 37 broke");
    std::atomic<int> count = 0;
    pool.parallelFor(10, [&](std::size_t) { ++count; });
    CHECK(count == 10);

    // parallelFor inside parallelFor doesn't deadlock, even with every worker busy.
    std::atomic<int> inner = 0;
    pool.parallelFor(8, [&](std::size_t) { pool.parallelFor(8, [&](std::size_t) { ++inner; }); });
    CHECK(inner == 64);
}

static void testTiledRenderMatchesUntiled() {
    // A blur chain with a shared input, rendered with different tile sizes and
    // thread counts, gives identical pixels, and each node is still computed once.
    auto build = [](Graph& g) -> Node& {
        auto& c = g.create<ConstantNode>("Constant1", 0.25f, Box{3, 2, 61, 30});
        auto& blur = g.create<BlurXNode>("Blur1", 3);
        auto& add = g.create<AddNode>("Add1");
        g.connect(c, blur, 0);
        g.connect(blur, add, 0);
        g.connect(c, add, 1);
        return add;
    };
    const Box region{-4, -4, 70, 40};

    Graph g0;
    const Image ref = Engine(EngineOptions{1, 1 << 20}).render(build(g0), {}, region);
    for (const EngineOptions opts : {EngineOptions{1, 5}, EngineOptions{4, 1}, EngineOptions{4, 16},
                                     EngineOptions{8, 64}}) {
        Graph g;
        Node& out = build(g);
        Engine e(opts);
        const Image img = e.render(out, {}, region);
        bool same = img.region() == ref.region();
        for (int y = region.y0; same && y < region.y1; ++y)
            for (int x = region.x0; same && x < region.x1; ++x)
                for (int ch = 0; ch < 4; ++ch) same = same && img.pixel(x, y)[ch] == ref.pixel(x, y)[ch];
        CHECK(same);
        auto& c = static_cast<ConstantNode&>(*g.find("Constant1"));
        CHECK(c.computedOnce());
        CHECK(e.lastStats().nodesComputed == 3);
        if (opts.tileSize == 1) CHECK(e.lastStats().tilesComputed == e.lastStats().pixelsComputed);
    }
}

// A ConstantNode that asks to be computed in one piece.
class WholeRegionNode : public ConstantNode {
public:
    using ConstantNode::ConstantNode;
    bool supportsTiles() const override { return false; }
};

static void testNodesCanOptOutOfTiles() {
    Graph g;
    auto& whole = g.create<WholeRegionNode>("Whole", 1.0f, kFormat);
    auto& add = g.create<AddNode>("Add1");
    g.connect(whole, add, 0);
    Engine e(EngineOptions{4, 4});
    e.render(add, {}, kFormat);
    CHECK(whole.computeCalls == 1);
    CHECK(whole.requested == kFormat);
    CHECK(add.computeCalls == (64 / 4) * (32 / 4));
    CHECK(e.lastStats().tilesComputed == 1 + (64 / 4) * (32 / 4));
}

static void testErrorsFromTilesNameTheNode() {
    // An exception thrown in a worker thread still comes back to render(),
    // carrying the node's name, and the engine is usable afterwards.
    Graph g;
    auto& c = g.create<ConstantNode>("Constant1", 1.0f, kFormat);
    auto& bad = g.create<ThrowingNode>("Thrower", "render");
    g.connect(c, bad, 0);
    Engine e(EngineOptions{4, 4});
    CHECK_THROWS(e.render(bad, {}, kFormat), "Thrower: render failed");
    CHECK(near(e.render(c, {}, kFormat).pixel(5, 5)[0], 1.0f));
}

// ---------------------------------------------------------------- tile cache

static bool samePixels(const Image& a, const Image& b) {
    if (a.region() != b.region() || a.channels() != b.channels()) return false;
    return std::memcmp(a.data(), b.data(), TileCache::bytesOf(a)) == 0;
}

static void testTileCacheLru() {
    auto tile = [](int x) { return std::make_shared<const Image>(Box{x, 0, x + 4, 4}, kRGBA); };
    const std::size_t tileBytes = 4 * 4 * 4 * sizeof(float);
    TileCache cache(3 * tileBytes);

    cache.put({1, {0, 0, 4, 4}}, tile(0));
    cache.put({1, {4, 0, 8, 4}}, tile(4));
    cache.put({2, {0, 0, 4, 4}}, tile(0));  // same region, different node state
    CHECK(cache.size() == 3 && cache.bytesUsed() == 3 * tileBytes);
    CHECK(cache.get({1, {0, 0, 4, 4}}) != nullptr);
    CHECK(cache.get({3, {0, 0, 4, 4}}) == nullptr);

    // Adding a fourth evicts the least recently used: {1, 4..8} (the
    // {1, 0..4} tile was just used by get()).
    cache.put({3, {0, 0, 4, 4}}, tile(0));
    CHECK(cache.size() == 3);
    CHECK(cache.get({1, {4, 0, 8, 4}}) == nullptr);
    CHECK(cache.get({1, {0, 0, 4, 4}}) != nullptr);

    // Re-adding a key doesn't double count.
    cache.put({3, {0, 0, 4, 4}}, tile(0));
    CHECK(cache.bytesUsed() == 3 * tileBytes);

    // A tile bigger than the whole limit isn't stored, and doesn't evict anything.
    cache.put({9, {0, 0, 100, 100}}, std::make_shared<const Image>(Box{0, 0, 100, 100}, kRGBA));
    CHECK(cache.get({9, {0, 0, 100, 100}}) == nullptr && cache.size() == 3);

    // Tiles handed out stay valid after eviction.
    auto held = cache.get({2, {0, 0, 4, 4}});
    cache.setMaxBytes(tileBytes);
    CHECK(cache.size() == 1 && cache.bytesUsed() == tileBytes);
    CHECK(held && held->region() == (Box{0, 0, 4, 4}));

    cache.clear();
    CHECK(cache.size() == 0 && cache.bytesUsed() == 0);
}

// Constant1 + Constant2, blurred: enough to see what an edit recomputes.
struct CacheGraph {
    Graph g;
    ConstantNode& c1 = g.create<ConstantNode>("Constant1", 0.5f, Box{2, 2, 60, 30});
    ConstantNode& c2 = g.create<ConstantNode>("Constant2", 0.25f, Box{0, 0, 40, 20});
    AddNode& add = g.create<AddNode>("Add1");
    BlurXNode& blur = g.create<BlurXNode>("Blur1", 2);
    CacheGraph() {
        g.connect(c1, add, 0);
        g.connect(c2, add, 1);
        g.connect(add, blur, 0);
    }
    void resetProbes() {
        for (const Probe* p : {static_cast<const Probe*>(&c1), static_cast<const Probe*>(&c2),
                               static_cast<const Probe*>(&add), static_cast<const Probe*>(&blur)})
            p->resetProbe();
    }
};

static void testCacheReusesTiles() {
    CacheGraph cg;
    Engine e(EngineOptions{.threads = 4, .tileSize = 16, .cacheBytes = 64 << 20});
    const Image first = e.render(cg.blur, {}, kFormat);
    CHECK(e.lastStats().tilesFromCache == 0);
    CHECK(e.cacheBytesUsed() > 0);

    // Rendering again computes nothing at all.
    cg.resetProbes();
    const Image again = e.render(cg.blur, {}, kFormat);
    CHECK(samePixels(first, again));
    CHECK(e.lastStats().tilesComputed == 0 && e.lastStats().nodesComputed == 0);
    CHECK(e.lastStats().tilesFromCache == 4 * 2);  // Blur1's 64x32 in 16px tiles
    CHECK(cg.c1.computeCalls == 0 && cg.add.computeCalls == 0 && cg.blur.computeCalls == 0);

    // A region made of whole cached tiles is also free.
    const Image part = e.render(cg.blur, {}, {16, 0, 48, 32});
    CHECK(e.lastStats().tilesComputed == 0);
    CHECK(samePixels(part, cropped(first, {16, 0, 48, 32})));

    // Matches an uncached render exactly.
    CacheGraph fresh;
    CHECK(samePixels(first, Engine(EngineOptions{1, 1 << 20}).render(fresh.blur, {}, kFormat)));
}

static void testCacheRecomputesOnlyDownstreamOfAnEdit() {
    CacheGraph cg;
    Engine e(EngineOptions{.threads = 4, .tileSize = 16, .cacheBytes = 64 << 20});
    e.render(cg.blur, {}, kFormat);

    // Edit Constant2: it, Add1 and Blur1 recompute; Constant1 doesn't.
    cg.resetProbes();
    cg.c2.setParam("value", 1.0);
    const Image edited = e.render(cg.blur, {}, kFormat);
    CHECK(cg.c1.computeCalls == 0);
    CHECK(cg.c2.computeCalls > 0 && cg.add.computeCalls > 0 && cg.blur.computeCalls > 0);

    CacheGraph fresh;
    fresh.c2.setParam("value", 1.0);
    CHECK(samePixels(edited, Engine(EngineOptions{1, 1 << 20}).render(fresh.blur, {}, kFormat)));

    // Undoing the edit brings back the original tiles: nothing recomputes.
    cg.c2.setParam("value", 0.25);
    e.render(cg.blur, {}, kFormat);
    CHECK(e.lastStats().tilesComputed == 0);

    // Reconnecting inputs changes the hash too.
    cg.g.connect(cg.c1, cg.add, 1);
    cg.resetProbes();
    e.render(cg.blur, {}, kFormat);
    CHECK(cg.c1.computeCalls == 0 && cg.add.computeCalls > 0);

    // So does the frame: nothing is shared between frames.
    e.render(cg.blur, {2.0}, kFormat);
    CHECK(e.lastStats().tilesFromCache == 0);
}

static void testCacheKeepsNodesApart() {
    // Two nodes of the same type with identical parameters still have their
    // own tiles, because state outside parameters (here, the pixel function)
    // can differ.
    Graph g;
    auto& a = g.create<test::SourceNode>("A", kFormat, kRGBA, [](int, int, int) { return 1.0f; }, kFormat);
    auto& b = g.create<test::SourceNode>("B", kFormat, kRGBA, [](int, int, int) { return 2.0f; }, kFormat);
    Engine e(EngineOptions{.cacheBytes = 64 << 20});
    CHECK(near(e.render(a, {}, kFormat).pixel(3, 3)[0], 1.0f));
    CHECK(near(e.render(b, {}, kFormat).pixel(3, 3)[0], 2.0f));
}

static void testCacheLimits() {
    // A cache far too small to hold a render still gives correct pixels.
    CacheGraph cg;
    Engine e(EngineOptions{.threads = 4, .tileSize = 8, .cacheBytes = 3 * 8 * 8 * 4 * sizeof(float)});
    CacheGraph fresh;
    const Image ref = Engine(EngineOptions{1, 1 << 20}).render(fresh.blur, {}, kFormat);
    for (int i = 0; i < 3; ++i) CHECK(samePixels(e.render(cg.blur, {}, kFormat), ref));
    CHECK(e.cacheBytesUsed() <= 3 * 8 * 8 * 4 * sizeof(float));

    // Turning the cache off frees it; turning it back on starts empty.
    e.setCacheBytes(0);
    CHECK(e.cacheBytesUsed() == 0);
    e.render(cg.blur, {}, kFormat);
    CHECK(e.lastStats().tilesFromCache == 0);
    e.setCacheBytes(64 << 20);
    e.render(cg.blur, {}, kFormat);
    e.render(cg.blur, {}, kFormat);
    CHECK(e.lastStats().tilesComputed == 0);
    e.clearCache();
    CHECK(e.cacheBytesUsed() == 0);
}

int main() {
    try {
        testRegionOfInterest();
        testSharedInputComputedOnce();
        testDisjointRequestsAreCombined();
        testClipsToDataWindow();
        testOptionalAndMissingInputs();
        testCycles();
        testGraphBookkeeping();
        testContractChecks();
        testDeepChain();
        testForeignExceptionsNameTheNode();
        testGraphMisuse();
        testParamAccess();
        testTileGrid();
        testThreadPool();
        testTiledRenderMatchesUntiled();
        testNodesCanOptOutOfTiles();
        testErrorsFromTilesNameTheNode();
        testTileCacheLru();
        testCacheReusesTiles();
        testCacheRecomputesOnlyDownstreamOfAnEdit();
        testCacheKeepsNodesApart();
        testCacheLimits();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    return test::finish("engine");
}
