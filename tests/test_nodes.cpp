// Tests for Grade, Merge and Transform.

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

#include "TestUtil.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/GradeNode.h"
#include "comp/nodes/Registry.h"
#include "comp/nodes/MergeNode.h"
#include "comp/nodes/TransformNode.h"

using namespace comp;

static const Box kFormat{0, 0, 32, 32};
static const ChannelList kRGBA{"R", "G", "B", "A"};

using test::SourceNode;

static SourceNode& solid(Graph& g, const std::string& name, Color c, Box box = kFormat,
                         ChannelList channels = kRGBA) {
    return g.create<SourceNode>(name, box, std::move(channels),
                                [c](int, int, int ch) { return float(c[std::min(ch, 3)]); });
}

// A recognisable non-symmetric pattern.
static float pattern(int x, int y, int c) { return 0.01f * x + 0.37f * y + 0.1f * c; }

// ---------------------------------------------------------------- Grade

static void testGrade() {
    Engine e;
    const Box px{4, 4, 5, 5};

    {   // Defaults: identity, and negative values survive (no surprise clamp).
        Graph g;
        auto& src = solid(g, "Src", {-0.5, 0.25, 2.0, 0.5});
        auto& grade = g.create<GradeNode>("Grade1");
        g.connect(src, grade, 0);
        Image img = e.render(grade, {}, px);
        CHECK_NEAR(img.pixel(4, 4)[0], -0.5);
        CHECK_NEAR(img.pixel(4, 4)[2], 2.0);
    }
    {   // Gain affects RGB but not alpha by default.
        Graph g;
        auto& src = solid(g, "Src", {0.25, 0.5, 0.75, 0.5});
        auto& grade = g.create<GradeNode>("Grade1");
        g.connect(src, grade, 0);
        grade.setParam("gain", 2.0);
        Image img = e.render(grade, {}, px);
        CHECK_NEAR(img.pixel(4, 4)[0], 0.5);
        CHECK_NEAR(img.pixel(4, 4)[2], 1.5);
        CHECK_NEAR(img.pixel(4, 4)[3], 0.5);

        grade.setParam("channels", std::string("rgba"));
        CHECK_NEAR(e.render(grade, {}, px).pixel(4, 4)[3], 1.0);
    }
    {   // Full formula against a hand calculation.
        Graph g;
        auto& src = solid(g, "Src", {0.6, 0.6, 0.6, 1});
        auto& grade = g.create<GradeNode>("Grade1");
        g.connect(src, grade, 0);
        grade.setParam("blackpoint", 0.1);
        grade.setParam("whitepoint", 0.9);
        grade.setParam("lift", 0.05);
        grade.setParam("gain", 1.2);
        grade.setParam("multiply", 1.5);
        grade.setParam("offset", 0.02);
        grade.setParam("gamma", 2.0);
        const double A = 1.5 * (1.2 - 0.05) / (0.9 - 0.1);
        const double B = 0.02 + 0.05 - A * 0.1;
        CHECK_NEAR(e.render(grade, {}, px).pixel(4, 4)[1], std::pow(A * 0.6 + B, 0.5));
    }
    {   // Per-channel values, clamps and mix.
        Graph g;
        auto& src = solid(g, "Src", {0.5, 0.5, 0.5, 1});
        auto& grade = g.create<GradeNode>("Grade1");
        g.connect(src, grade, 0);
        grade.setParam("gain", Color{1, 2, 3, 1});
        Image img = e.render(grade, {}, px);
        CHECK_NEAR(img.pixel(4, 4)[0], 0.5);
        CHECK_NEAR(img.pixel(4, 4)[1], 1.0);
        CHECK_NEAR(img.pixel(4, 4)[2], 1.5);

        grade.setParam("white_clamp", true);
        CHECK_NEAR(e.render(grade, {}, px).pixel(4, 4)[2], 1.0);

        grade.setParam("gain", 1);  // int works too
        grade.setParam("offset", -1.0);
        CHECK_NEAR(e.render(grade, {}, px).pixel(4, 4)[0], 0.0);  // black clamp
        grade.setParam("black_clamp", false);
        CHECK_NEAR(e.render(grade, {}, px).pixel(4, 4)[0], -0.5);

        grade.setParam("offset", 0.5);
        grade.setParam("mix", 0.5);
        CHECK_NEAR(e.render(grade, {}, px).pixel(4, 4)[0], 0.75);
    }
    {   // Lifting black doesn't grow the data window.
        Graph g;
        auto& src = solid(g, "Src", {0, 0, 0, 0}, Box{10, 10, 20, 20});
        auto& grade = g.create<GradeNode>("Grade1");
        g.connect(src, grade, 0);
        grade.setParam("lift", 0.5);
        Image img = e.render(grade, {}, kFormat);
        CHECK_NEAR(img.pixel(15, 15)[0], 0.5);
        CHECK_NEAR(img.pixel(5, 5)[0], 0.0);
        CHECK(e.info(grade, {}).dataWindow == (Box{10, 10, 20, 20}));
    }
}

// ---------------------------------------------------------------- Merge

static float mergeOne(const std::string& op, Color A, Color B, int channel) {
    Graph g;
    auto& b = solid(g, "B", B);
    auto& a = solid(g, "A", A);
    auto& merge = g.create<MergeNode>("Merge1");
    g.connect(b, merge, 0);
    g.connect(a, merge, 1);
    merge.setParam("operation", op);
    Engine e;
    return e.render(merge, {}, {0, 0, 1, 1}).pixel(0, 0)[channel];
}

static void testMergeOperations() {
    // A: 0.4 red at alpha 0.5. B: 0.8 red at alpha 1.
    const Color A{0.4, 0, 0, 0.5}, B{0.8, 0, 0, 1};
    const double a = 0.5, b = 1.0, Av = 0.4, Bv = 0.8;
    struct Case {
        const char* op;
        double expected;
    } cases[] = {
        {"over", Av + Bv * (1 - a)},  {"under", Av * (1 - b) + Bv}, {"plus", Av + Bv},
        {"from", Bv - Av},            {"minus", Av - Bv},           {"multiply", Av * Bv},
        {"screen", Av + Bv - Av * Bv}, {"difference", std::fabs(Av - Bv)},
        {"max", Bv},                  {"min", Av},                  {"average", (Av + Bv) / 2},
        {"copy", Av},                 {"in", Av * b},               {"out", Av * (1 - b)},
        {"atop", Av * b + Bv * (1 - a)}, {"xor", Av * (1 - b) + Bv * (1 - a)},
        {"mask", Bv * a},             {"stencil", Bv * (1 - a)},
    };
    for (const auto& c : cases) {
        const float got = mergeOne(c.op, A, B, 0);
        if (std::fabs(got - c.expected) > 1e-5) {
            std::fprintf(stderr, "merge %s: got %g, expected %g\n", c.op, got, c.expected);
            ++test::failures;
        }
    }
    // Alpha follows the same formula: over gives a + b(1-a).
    CHECK_NEAR(mergeOne("over", A, B, 3), 1.0);
    CHECK_NEAR(mergeOne("over", A, {0, 0, 0, 0.5}, 3), 0.75);
}

static void testMergeChannelsAndBBox() {
    Engine e;
    Graph g;
    auto& b = solid(g, "B", {0.2, 0.2, 0.2, 1}, Box{0, 0, 10, 10});
    // A has RGB + depth but no alpha, in a different box.
    auto& a = solid(g, "A", {0.5, 0.5, 0.5, 7}, Box{5, 5, 20, 20}, {"R", "G", "B", "Z"});
    auto& merge = g.create<MergeNode>("Merge1");
    g.connect(b, merge, 0);

    // A unconnected: B passes through.
    CHECK(e.info(merge, {}).channels == kRGBA);
    CHECK_NEAR(e.render(merge, {}, {1, 1, 2, 2}).pixel(1, 1)[0], 0.2);

    g.connect(a, merge, 1);
    const ImageInfo info = e.info(merge, {});
    CHECK((info.channels == ChannelList{"R", "G", "B", "A", "Z"}));
    CHECK(info.dataWindow == (Box{0, 0, 20, 20}));

    Image img = e.render(merge, {}, kFormat);
    // Overlap: A has no alpha, so it counts as 0 and "over" adds.
    CHECK_NEAR(img.pixel(7, 7)[0], 0.7);
    CHECK_NEAR(img.pixel(7, 7)[3], 1.0);
    CHECK_NEAR(img.pixel(7, 7)[4], 7.0);  // Z from A
    CHECK_NEAR(img.pixel(2, 2)[0], 0.2);  // B only
    CHECK_NEAR(img.pixel(2, 2)[4], 0.0);  // no Z there
    CHECK_NEAR(img.pixel(15, 15)[0], 0.5);  // A only
    CHECK_NEAR(img.pixel(25, 25)[0], 0.0);  // neither

    merge.setParam("bbox", std::string("intersection"));
    CHECK(e.info(merge, {}).dataWindow == (Box{5, 5, 10, 10}));
    CHECK_NEAR(e.render(merge, {}, kFormat).pixel(15, 15)[0], 0.0);
    merge.setParam("bbox", std::string("B"));
    CHECK(e.info(merge, {}).dataWindow == (Box{0, 0, 10, 10}));

    merge.setParam("bbox", std::string("union"));
    merge.setParam("mix", 0.5);
    CHECK_NEAR(e.render(merge, {}, {7, 7, 8, 8}).pixel(7, 7)[0], 0.45);

    merge.setParam("operation", std::string("bogus"));
    bool threw = false;
    try {
        e.info(merge, {});
    } catch (const Error&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------- Transform

static void testTransformExactCases() {
    Engine e;
    for (const char* filter : {"nearest", "bilinear", "cubic"}) {
        Graph g;
        auto& src = g.create<SourceNode>("Src", kFormat, kRGBA, pattern);
        auto& xf = g.create<TransformNode>("Transform1");
        g.connect(src, xf, 0);
        xf.setParam("filter", std::string(filter));

        // Identity.
        Image img = e.render(xf, {}, kFormat);
        CHECK_NEAR(img.pixel(13, 21)[2], pattern(13, 21, 2));

        // Integer translation is exact with every filter.
        xf.setParam("translate", Vec2{3, -2});
        img = e.render(xf, {}, kFormat);
        float maxErr = 0;
        for (int y = 2; y < 30; ++y)
            for (int x = 3; x < 32; ++x)
                maxErr = std::max(maxErr, std::fabs(img.pixel(x, y)[1] - pattern(x - 3, y + 2, 1)));
        CHECK(maxErr < 1e-5f);
        if (maxErr >= 1e-5f) std::fprintf(stderr, "  (filter %s, max error %g)\n", filter, maxErr);
    }
}

static void testTransformGeometry() {
    Engine e;
    Graph g;
    // A single white pixel at (15, 10).
    auto& src = g.create<SourceNode>("Src", kFormat, kRGBA,
                                     [](int x, int y, int) { return (x == 15 && y == 10) ? 1.0f : 0.0f; });
    auto& xf = g.create<TransformNode>("Transform1");
    g.connect(src, xf, 0);
    xf.setParam("filter", std::string("nearest"));
    xf.setParam("center", Vec2{10.5, 10.5});  // centre of pixel (10, 10)

    // 90 degrees counter-clockwise on screen: 5 px right of centre ends up 5 px above it.
    xf.setParam("rotate", 90);
    Image img = e.render(xf, {}, kFormat);
    CHECK_NEAR(img.pixel(10, 5)[0], 1.0);
    CHECK_NEAR(img.pixel(15, 10)[0], 0.0);

    // Scale 2 about the centre: the pixel's area [15,16)x[10,11) maps to
    // [19.5,21.5)x[9.5,11.5), which holds the centres of pixels 19-20 x 9-10.
    xf.setParam("rotate", 0);
    xf.setParam("scale", 2.0);
    img = e.render(xf, {}, kFormat);
    CHECK_NEAR(img.pixel(19, 9)[0], 1.0);
    CHECK_NEAR(img.pixel(20, 10)[0], 1.0);
    CHECK_NEAR(img.pixel(21, 10)[0], 0.0);
    CHECK_NEAR(img.pixel(18, 10)[0], 0.0);

    // Data window: 32x32 scaled 2x about (10.5, 10.5) -> [-10.5, 53.5] -> [-11, 54).
    CHECK(e.info(xf, {}).dataWindow == (Box{-11, -11, 54, 54}));

    // invert undoes the transform.
    xf.setParam("scale", 1.0);
    xf.setParam("translate", Vec2{4, 0});
    xf.setParam("invert", true);
    CHECK_NEAR(e.render(xf, {}, kFormat).pixel(11, 10)[0], 1.0);
}

static void testTransformFiltering() {
    Engine e;
    Graph g;
    auto& src = g.create<SourceNode>("Src", kFormat, kRGBA, pattern);
    auto& xf = g.create<TransformNode>("Transform1");
    g.connect(src, xf, 0);

    // Half-pixel shift: bilinear averages neighbours; cubic is exact on linear ramps.
    xf.setParam("translate", Vec2{0.5, 0});
    xf.setParam("filter", std::string("bilinear"));
    Image img = e.render(xf, {}, kFormat);
    CHECK_NEAR(img.pixel(10, 10)[0], (pattern(9, 10, 0) + pattern(10, 10, 0)) / 2);
    xf.setParam("filter", std::string("cubic"));
    img = e.render(xf, {}, kFormat);
    CHECK_NEAR(img.pixel(10, 10)[0], (pattern(9, 10, 0) + pattern(10, 10, 0)) / 2);

    // Zero scale collapses to nothing instead of crashing.
    xf.setParam("scale", 0.0);
    CHECK(e.info(xf, {}).dataWindow.empty());
    CHECK_NEAR(e.render(xf, {}, kFormat).pixel(5, 5)[0], 0.0);
}

static void testRotationWithFilters() {
    Engine e;
    const Box big{0, 0, 64, 64};

    for (const char* filter : {"bilinear", "cubic"}) {
        // 90 degrees about a pixel corner maps pixel centres onto pixel centres,
        // so even a non-linear image must come through exactly.
        {
            Graph g;
            auto bumpy = [](int x, int y, int c) { return 0.001f * x * x + 0.03f * y + c; };
            auto& src = g.create<SourceNode>("Src", big, kRGBA, bumpy, big);
            auto& xf = g.create<TransformNode>("Transform1");
            g.connect(src, xf, 0);
            xf.setParam("filter", std::string(filter));
            xf.setParam("center", Vec2{32, 32});
            xf.setParam("rotate", 90);
            Image img = e.render(xf, {}, big);
            float maxErr = 0;
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x)  // output (x, y) comes from input (63 - y, x)
                    maxErr = std::max(maxErr, std::fabs(img.pixel(x, y)[1] - bumpy(63 - y, x, 1)));
            if (maxErr > 1e-4f) {
                std::fprintf(stderr, "rotate 90 (%s): max error %g\n", filter, maxErr);
                ++test::failures;
            }
        }
        // Both filters reproduce linear ramps exactly, so an arbitrary angle can
        // be checked against the analytic answer away from the edges.
        {
            Graph g;
            auto& src = g.create<SourceNode>("Src", big, kRGBA, pattern, big);
            auto& xf = g.create<TransformNode>("Transform1");
            g.connect(src, xf, 0);
            xf.setParam("filter", std::string(filter));
            xf.setParam("center", Vec2{32, 32});
            xf.setParam("rotate", 30.0);
            xf.setParam("scale", Vec2{1.25, 0.8});
            const Affine inv = xf.matrix().inverse();
            Image img = e.render(xf, {}, big);
            float maxErr = 0;
            for (int y = 22; y < 42; ++y)
                for (int x = 22; x < 42; ++x) {
                    const auto [u, v] = inv.apply(x + 0.5, y + 0.5);
                    // pattern() is linear, so its value at a continuous position is exact.
                    const double want = 0.01 * (u - 0.5) + 0.37 * (v - 0.5) + 0.1 * 2;
                    maxErr = std::max(maxErr, float(std::fabs(img.pixel(x, y)[2] - want)));
                }
            if (maxErr > 1e-4f) {
                std::fprintf(stderr, "rotate 30 (%s): max error %g\n", filter, maxErr);
                ++test::failures;
            }
        }
    }
}

static void testBadParameters() {
    Engine e;
    Graph g;
    auto& src = solid(g, "Src", {0.5, 0.5, 0.5, 1});

    auto& xf = g.create<TransformNode>("Transform1");
    g.connect(src, xf, 0);
    xf.setParam("filter", std::string("lanczos"));
    CHECK_THROWS(e.info(xf, {}), "filter must be nearest, bilinear or cubic");
    xf.setParam("filter", std::string("cubic"));
    xf.setParam("scale", 0.0);
    xf.setParam("invert", true);
    CHECK_THROWS(e.info(xf, {}), "cannot invert");
    xf.setParam("rotate", std::string("ninety"));
    CHECK_THROWS(e.info(xf, {}), "'rotate' must be a number");

    auto& grade = g.create<GradeNode>("Grade1");
    g.connect(src, grade, 0);
    grade.setParam("channels", std::string("rg"));
    CHECK_THROWS(e.render(grade, {}, {0, 0, 1, 1}), "channels must be \"rgb\" or \"rgba\"");
    grade.setParam("channels", std::string("rgb"));
    grade.setParam("gamma", 0.0);
    CHECK_THROWS(e.render(grade, {}, {0, 0, 1, 1}), "gamma must be greater than 0");
    grade.setParam("gamma", 1.0);
    grade.setParam("black_clamp", 1);  // int instead of bool
    CHECK_THROWS(e.render(grade, {}, {0, 0, 1, 1}), "'black_clamp' has the wrong type");

    auto& merge = g.create<MergeNode>("Merge1");
    g.connect(src, merge, 0);
    g.connect(src, merge, 1);
    merge.setParam("bbox", std::string("both"));
    CHECK_THROWS(e.info(merge, {}), "bbox must be union, intersection, A or B");
}

static void testTransformRegions() {
    Engine e;
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 2000, 1000}, kRGBA, pattern);
    auto& xf = g.create<TransformNode>("Transform1");
    g.connect(src, xf, 0);
    xf.setParam("filter", std::string("bilinear"));

    // Translating by 100 asks for the matching input area, plus filter padding.
    xf.setParam("translate", Vec2{100, 0});
    e.render(xf, {}, {500, 500, 600, 600});
    CHECK(src.requested == (Box{398, 498, 502, 602}));

    // Shrinking to 1% doesn't request a 100x larger region than exists:
    // the request is clipped to the input's data window.
    xf.setParam("translate", Vec2{0, 0});
    xf.setParam("scale", 0.01);
    src.resetProbe();
    e.render(xf, {}, {0, 0, 2000, 1000});
    CHECK(src.requested == (Box{0, 0, 2000, 1000}));
    CHECK(e.lastStats().pixelsComputed < 2'100'000);
}

static void testChain() {
    // Read-less mini comp: grade a foreground, move it, put it over a background.
    Engine e;
    Graph g;
    auto& bg = solid(g, "BG", {0.1, 0.2, 0.3, 1});
    auto& fg = solid(g, "FG", {0.5, 0.5, 0.5, 1}, Box{0, 0, 4, 4});
    auto& grade = g.create<GradeNode>("Grade1");
    auto& xf = g.create<TransformNode>("Transform1");
    auto& merge = g.create<MergeNode>("Merge1");
    g.connect(fg, grade, 0);
    g.connect(grade, xf, 0);
    g.connect(bg, merge, 0);
    g.connect(xf, merge, 1);
    grade.setParam("gain", Color{2, 1, 0, 1});
    xf.setParam("translate", Vec2{10, 10});

    Image img = e.render(merge, {}, kFormat);
    CHECK_NEAR(img.pixel(11, 11)[0], 1.0);  // graded fg, opaque over bg
    CHECK_NEAR(img.pixel(11, 11)[2], 0.0);
    CHECK_NEAR(img.pixel(1, 1)[2], 0.3);    // bg where fg used to be
    CHECK(e.lastStats().nodesComputed == 5);
}

// Every built-in node type describes its parameters, and the described
// defaults are the real ones: setting each parameter to its default changes
// nothing.
static void testParamSpecs() {
    for (const auto& type : std::vector<std::string>{"ColorSpace", "Grade", "Merge", "Transform"}) {
        Graph g;
        auto& src = g.create<SourceNode>("Src", Box{-2, -1, 20, 12}, ChannelList{"R", "G", "B", "A"},
                                         [](int x, int y, int c) { return 0.05f * x + 0.03f * y + 0.1f * c; });
        Node& plain = g.add(createNode(type, "Plain"));
        Node& explicitDefaults = g.add(createNode(type, "Explicit"));
        g.connect(src, plain, 0);
        g.connect(src, explicitDefaults, 0);
        if (type == "Merge") {
            g.connect(src, plain, 1);
            g.connect(src, explicitDefaults, 1);
        }
        const auto specs = plain.paramSpecs();
        CHECK(!specs.empty());
        for (const auto& s : specs) {
            if (s.optional) continue;
            CHECK(!s.name.empty() && !s.hint.empty());
            if (s.kind == ParamSpec::Kind::Choice)
                CHECK(s.openChoice || std::find(s.options.begin(), s.options.end(),
                                                std::get<std::string>(s.defaultValue)) != s.options.end());
            explicitDefaults.setParam(s.name, s.defaultValue);
        }
        Engine e;
        const Box region{-4, -4, 24, 16};
        const Image a = e.render(plain, {}, region), b = e.render(explicitDefaults, {}, region);
        CHECK(std::equal(a.data(), a.data() + region.area() * a.numChannels(), b.data()));
    }
    // Read and Write: described, with a file parameter first.
    for (const auto& type : {"Read", "Write"}) {
        const auto specs = createNode(type, "X")->paramSpecs();
        CHECK(!specs.empty() && specs[0].name == "file" && specs[0].kind == ParamSpec::Kind::File);
    }
}

static void testTakeInsertRename() {
    Graph g;
    auto& a = g.add(createNode("Grade", "A"));
    auto& b = g.add(createNode("Grade", "B"));
    auto& c = g.add(createNode("Grade", "C"));
    g.connect(a, b, 0);
    g.connect(b, c, 0);

    // take() hands the same node back, disconnected both ways...
    std::shared_ptr<Node> taken = g.take(b);
    CHECK(taken.get() == &b && g.nodes().size() == 2 && !g.find("B"));
    CHECK(c.input(0) == nullptr && b.input(0) == nullptr);
    // ...and insert() puts it back where it was, ready to reconnect.
    CHECK(&g.insert(taken, 1) == &b && g.indexOf(b) == 1 && g.nodes()[1].get() == &b);
    g.connect(a, b, 0);
    g.connect(b, c, 0);
    CHECK(c.input(0) == &b);
    CHECK_THROWS(g.insert(std::shared_ptr<Node>(createNode("Grade", "A")), 9), "a node named 'A' already exists");
    CHECK(g.indexOf(b) == 1);

    g.rename(b, "Hero");
    CHECK(b.name() == "Hero" && g.find("Hero") == &b && !g.find("B"));
    g.rename(b, "Hero");  // to its own name: fine
    CHECK_THROWS(g.rename(b, "A"), "a node named 'A' already exists");
    CHECK_THROWS(g.rename(b, ""), "node name cannot be empty");
    Graph other;
    CHECK_THROWS(other.rename(b, "X"), "does not belong to this graph");
    CHECK_THROWS(other.indexOf(b), "does not belong to this graph");
}

static void testBackdrop() {
    // Only for the editor: no inputs, an empty output, described parameters.
    Graph g;
    Node& b = g.add(createNode("Backdrop", "Backdrop1"));
    CHECK(b.maxInputs() == 0 && b.paramSpecs().size() == 3);
    Engine e;
    const ImageInfo info = e.info(b, RenderContext{});
    CHECK(info.dataWindow.area() == 0 && info.channels.empty());
    CHECK(e.render(b, RenderContext{}, Box{0, 0, 4, 4}).channels().empty());
}

static void testConstant() {
    Graph g;
    Node& c = g.add(createNode("Constant", "Constant1"));
    c.setParam("color", Color{0.25, 0.5, 1.0, 1.0});
    CHECK(c.maxInputs() == 0);

    // The project's format, from the render.
    Engine e({.threads = 4, .tileSize = 64, .cacheBytes = 64 << 20});
    RenderContext hd{1, Format{1920, 1080}}, small{1, Format{100, 50}};
    ImageInfo info = e.info(c, hd);
    CHECK((info.displayWindow == Box{0, 0, 1920, 1080}) && info.dataWindow == info.displayWindow);
    CHECK((info.channels == ChannelList{"R", "G", "B", "A"}));
    Image img = e.render(c, small, e.info(c, small).dataWindow);
    CHECK((img.region() == Box{0, 0, 100, 50}));
    for (int y : {0, 49})
        for (int x : {0, 63, 64, 99}) {  // across tile edges
            const float* p = img.pixel(x, y);
            CHECK(p[0] == 0.25f && p[1] == 0.5f && p[2] == 1.0f && p[3] == 1.0f);
        }
    // The format is part of the cache key: same node, same parameters,
    // different format, different result.
    CHECK((e.render(c, hd, {0, 0, 8, 8}).region() == Box{0, 0, 8, 8}));
    CHECK((e.info(c, small).displayWindow == Box{0, 0, 100, 50}));

    // A size of its own.
    c.setParam("format", std::string("custom"));
    c.setParam("size", Vec2{640, 480});
    CHECK((e.info(c, hd).displayWindow == Box{0, 0, 640, 480}));
    c.setParam("size", Vec2{640.5, 480});
    CHECK_THROWS(e.info(c, hd), "Constant1: size must be whole numbers from 1 to 65536");
    c.setParam("size", Vec2{0, 480});
    CHECK_THROWS(e.info(c, hd), "Constant1: size must be whole numbers");
    c.setParam("format", std::string("HD"));
    CHECK_THROWS(e.info(c, hd), "Constant1: unknown format 'HD' (use project or custom)");
}

static void testRegistry() {
    // Built-in types first, then any OFX plugins installed on this machine.
    const std::vector<std::string> builtin{"Backdrop", "Blur", "ColorSpace", "Constant", "Crop", "Grade", "Merge", "Premult", "Read", "Reformat", "Shuffle", "Transform", "Unpremult", "Write"};
    const auto all = nodeTypes();
    CHECK(all.size() >= builtin.size() && std::equal(builtin.begin(), builtin.end(), all.begin()));
    for (std::size_t i = builtin.size(); i < all.size(); ++i) CHECK(all[i].rfind("ofx:", 0) == 0);
    const auto& types = builtin;
    Graph g;
    for (const auto& t : types) {
        Node& n = g.add(createNode(t, t + "1"));
        CHECK(n.typeName() == t);
        CHECK(g.find(t + "1") == &n);
    }
    CHECK_THROWS(createNode("Defocus", "Defocus1"), "unknown node type 'Defocus' (built-in types: Backdrop, Blur, ColorSpace, Constant, Crop, Grade");
    CHECK_THROWS(g.add(createNode("Grade", "Grade1")), "a node named 'Grade1' already exists");
    CHECK_THROWS(g.add(nullptr), "cannot add a null node");
    CHECK(g.nodes().size() == types.size());
}

int main() {
    try {
        testRegistry();
        testBackdrop();
        testConstant();
        testTakeInsertRename();
        testParamSpecs();
        testGrade();
        testMergeOperations();
        testMergeChannelsAndBBox();
        testTransformExactCases();
        testTransformGeometry();
        testTransformFiltering();
        testRotationWithFilters();
        testBadParameters();
        testTransformRegions();
        testChain();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    return test::finish("node");
}
