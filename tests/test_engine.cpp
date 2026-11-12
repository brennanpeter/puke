// Tests for Graph and Engine, using small test-only nodes.

#include <cmath>
#include <cstdio>
#include <string>

#include "TestUtil.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"

using namespace comp;

static const Box kFormat{0, 0, 64, 32};
static const ChannelList kRGBA{"R", "G", "B", "A"};

// Instrumentation shared by the test nodes. mutable because compute() is const;
// real nodes must not do this.
struct Probe {
    mutable int computeCalls = 0;
    mutable Box lastRegion;
};

// Solid colour `value` (all channels) inside `box`, zero outside.
class ConstantNode : public Node, public Probe {
public:
    ConstantNode(std::string name, float value, Box box) : Node(std::move(name)), value_(value), box_(box) {}
    const char* typeName() const override { return "Constant"; }

    ImageInfo computeInfo(const RenderContext&, std::span<const ImageInfo* const>) const override {
        return {box_, kFormat, kRGBA};
    }
    Image compute(const RenderContext&, const Box& region, std::span<const Image* const>) const override {
        ++computeCalls;
        lastRegion = region;
        Image out(region, kRGBA);
        const Box fill = region.intersect(box_);
        for (int y = fill.y0; y < fill.y1; ++y)
            for (int x = fill.x0; x < fill.x1; ++x)
                for (int c = 0; c < 4; ++c) out.pixel(x, y)[c] = value_;
        return out;
    }

private:
    float value_;
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
        ++computeCalls;
        lastRegion = region;
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
        ++computeCalls;
        lastRegion = region;
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
    CHECK(blur.lastRegion == (Box{8, 4, 12, 6}));
    CHECK(c.lastRegion == (Box{10, 4, 14, 6}));

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

    CHECK(c.computeCalls == 1);
    CHECK(blur.computeCalls == 1);
    CHECK(add.computeCalls == 1);
    // Union of Blur's padded request and Add's direct request.
    CHECK(c.lastRegion == (Box{17, 10, 33, 12}));
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
    CHECK(c.computeCalls == 1);
    CHECK(c.lastRegion == (Box{10, 5, 54, 6}));  // the union of both requests
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
    CHECK(c.lastRegion == (Box{10, 10, 20, 20}));
    CHECK(blur.lastRegion == (Box{9, 10, 21, 20}));
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
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    return test::finish("engine");
}
