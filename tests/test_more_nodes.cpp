// Crop, Reformat, Shuffle, Premult/Unpremult and Blur.

#include <cmath>
#include <cstdio>
#include <string>

#include "TestUtil.h"
#include "puke/engine/Engine.h"
#include "puke/graph/Graph.h"
#include "puke/nodes/BlurNode.h"
#include "puke/nodes/Registry.h"

using namespace puke;
using test::SourceNode;

namespace {

Node& add(Graph& g, const char* type, const char* name) { return g.add(createNode(type, name)); }

// Renders `node`'s whole data window in one piece and in odd-sized tiles on
// several threads; the two must match exactly. Returns the first.
Image renderChecked(const Node& node, const RenderContext& ctx = {}) {
    Engine whole({.threads = 0, .tileSize = 1 << 20, .cacheBytes = 0});
    Engine tiled({.threads = 4, .tileSize = 7, .cacheBytes = 0});
    const Box dw = whole.info(node, ctx).dataWindow;
    Image a = whole.render(node, ctx, dw), b = tiled.render(node, ctx, dw);
    bool same = a.region() == b.region() && a.channels() == b.channels();
    for (int y = dw.y0; same && y < dw.y1; ++y)
        for (int x = dw.x0; same && x < dw.x1; ++x)
            for (int c = 0; c < a.numChannels(); ++c) same = same && a.pixel(x, y)[c] == b.pixel(x, y)[c];
    CHECK(same);
    return a;
}

// Every pixel's value encodes where it is: x + 100 y, and channel c adds c/10.
float coord(int x, int y, int c) { return static_cast<float>(x + 100 * y) + c / 10.0f; }

}  // namespace

static void testCrop() {
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 32, 32}, ChannelList{"R", "G", "B", "A"}, coord);
    Node& crop = add(g, "Crop", "Crop1");
    g.connect(src, crop, 0);
    crop.setParam("box_min", Vec2{4, 6});
    crop.setParam("box_max", Vec2{20.4, 26});  // rounded to whole pixels
    Engine e;
    ImageInfo info = e.info(crop, {});
    CHECK((info.dataWindow == Box{4, 6, 20, 26}) && (info.displayWindow == Box{0, 0, 32, 32}));
    Image img = renderChecked(crop);
    CHECK(img.pixel(10, 11)[2] == coord(10, 11, 2) && img.pixel(19, 25)[0] == coord(19, 25, 0));
    src.resetProbe();
    e.render(crop, {}, {0, 0, 32, 32});
    CHECK((src.requested == Box{4, 6, 20, 26}));  // nothing outside the box asked for

    // The box as the new frame.
    crop.setParam("reformat", true);
    info = e.info(crop, {});
    CHECK((info.dataWindow == Box{0, 0, 16, 20}) && (info.displayWindow == Box{0, 0, 16, 20}));
    img = renderChecked(crop);
    CHECK(img.pixel(0, 0)[0] == coord(4, 6, 0) && img.pixel(15, 19)[3] == coord(19, 25, 3));

    // A box past the image: only what's there.
    crop.setParam("reformat", false);
    crop.setParam("box_min", Vec2{-10, 20});
    crop.setParam("box_max", Vec2{50, 50});
    CHECK((e.info(crop, {}).dataWindow == Box{0, 20, 32, 32}));
    crop.setParam("box_max", Vec2{-10, 50});
    CHECK_THROWS(e.info(crop, {}), "Crop1: the box is empty");
}

static void testReformat() {
    Graph g;
    // 40x20, valued by pixel centre x (linear, which Catmull-Rom reproduces).
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 40, 20}, ChannelList{"R", "G", "B", "A"},
                                     [](int x, int, int) { return x + 0.5f; }, Box{0, 0, 40, 20});
    Node& rf = add(g, "Reformat", "Reformat1");
    g.connect(src, rf, 0);
    Engine e;
    const RenderContext hd{1, Format{80, 40}};

    // Width (the default), centred: exactly 2x.
    ImageInfo info = e.info(rf, hd);
    CHECK((info.displayWindow == Box{0, 0, 80, 40}));
    CHECK((info.dataWindow == Box{-4, -4, 84, 44}));  // the filter's reach, scaled
    Image img = renderChecked(rf, hd);
    CHECK_NEAR(img.pixel(40, 20)[0], (40 + 0.5f) / 2);
    CHECK_NEAR(img.pixel(13, 7)[1], (13 + 0.5f) / 2);

    // The project format comes from the render.
    CHECK((e.info(rf, {1, Format{20, 10}}).displayWindow == Box{0, 0, 20, 10}));

    // fit / fill / height / distort into 80x80 (input ratios 2 across, 4 down).
    const RenderContext square{1, Format{80, 80}};
    rf.setParam("resize", std::string("fit"));
    CHECK((e.info(rf, square).dataWindow == Box{-4, 16, 84, 64}));  // 2x, centred vertically
    rf.setParam("resize", std::string("fill"));
    CHECK((e.info(rf, square).dataWindow == Box{-48, -8, 128, 88}));  // 4x
    rf.setParam("resize", std::string("height"));
    CHECK((e.info(rf, square).dataWindow == Box{-48, -8, 128, 88}));
    rf.setParam("resize", std::string("distort"));
    CHECK((e.info(rf, square).dataWindow == Box{-4, -8, 84, 88}));  // 2x across, 4x down
    img = renderChecked(rf, square);
    CHECK_NEAR(img.pixel(40, 40)[0], (40 + 0.5f) / 2);

    // No scaling, corners meeting: the input exactly, in the new frame.
    rf.setParam("resize", std::string("none"));
    rf.setParam("center", false);
    info = e.info(rf, square);
    CHECK((info.dataWindow == Box{0, 0, 40, 20}) && (info.displayWindow == Box{0, 0, 80, 80}));
    img = renderChecked(rf, square);
    CHECK(img.pixel(39, 19)[0] == 39.5f);

    // A size of its own, and errors.
    rf.setParam("format", std::string("custom"));
    rf.setParam("size", Vec2{30, 30});
    CHECK((e.info(rf, square).displayWindow == Box{0, 0, 30, 30}));
    rf.setParam("size", Vec2{0, 30});
    CHECK_THROWS(e.info(rf, square), "Reformat1: size must be whole numbers");
    rf.setParam("format", std::string("project"));
    rf.setParam("resize", std::string("stretch"));
    CHECK_THROWS(e.info(rf, square), "Reformat1: resize must be none, width, height, fit, fill or distort");
    // Input infos are only there during a render.
    CHECK_THROWS(rf.inputInfo(0), "Reformat1: inputInfo() is only available while the engine renders this node");
}

static void testShuffle() {
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 16, 16}, ChannelList{"R", "G", "B", "A", "Z"}, coord);
    Node& sh = add(g, "Shuffle", "Shuffle1");
    g.connect(src, sh, 0);
    sh.setParam("red", std::string("B"));
    sh.setParam("green", std::string("1"));
    sh.setParam("blue", std::string("0"));
    sh.setParam("alpha", std::string("R"));
    Engine e;
    CHECK((e.info(sh, {}).channels == ChannelList{"R", "G", "B", "A", "Z"}));
    Image img = renderChecked(sh);
    const float* p = img.pixel(3, 5);
    CHECK(p[0] == coord(3, 5, 2) && p[1] == 1.0f && p[2] == 0.0f && p[3] == coord(3, 5, 0));
    CHECK(p[4] == coord(3, 5, 4));  // Z passes through

    // RGB only: A is added (zero unless filled).
    auto& rgb = g.create<SourceNode>("Rgb", Box{0, 0, 8, 8}, ChannelList{"R", "G", "B"}, coord);
    Node& sh2 = add(g, "Shuffle", "Shuffle2");
    g.connect(rgb, sh2, 0);
    CHECK((e.info(sh2, {}).channels == ChannelList{"R", "G", "B", "A"}));
    CHECK(renderChecked(sh2).pixel(2, 2)[3] == 0.0f);

    // Alpha from a second input, whose data window is bigger.
    auto& matte = g.create<SourceNode>("Matte", Box{-4, 0, 20, 16}, ChannelList{"R", "G", "B", "A"},
                                       [](int x, int, int c) { return c == 3 ? x * 0.01f : 9.0f; });
    sh2.setParam("alpha", std::string("2:A"));
    CHECK_THROWS(e.info(sh2, {}), "Shuffle2: takes a channel from input 2, which isn't connected");
    g.connect(matte, sh2, 1);
    CHECK((e.info(sh2, {}).dataWindow == Box{-4, 0, 20, 16}));
    img = renderChecked(sh2);
    CHECK_NEAR(img.pixel(5, 3)[3], 0.05f);
    CHECK(img.pixel(5, 3)[0] == coord(5, 3, 0));
    CHECK(img.pixel(-2, 3)[0] == 0.0f && img.pixel(-2, 3)[3] == -0.02f);
    // Not used: input 2 isn't rendered.
    sh2.setParam("alpha", std::string("A"));
    matte.resetProbe();
    e.render(sh2, {}, {0, 0, 8, 8});
    CHECK(matte.computeCalls == 0);

    sh2.setParam("alpha", std::string("Z"));
    CHECK_THROWS(e.info(sh2, {}), "Shuffle2: alpha can't come from 'Z'");
}

static void testPremult() {
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 4, 1}, ChannelList{"R", "G", "B", "A", "Z"},
                                     [](int x, int, int c) {
                                         const float px[4][5] = {{0.5f, 0.4f, 0.2f, 0.5f, 3},
                                                                 {0.3f, 0.3f, 0.3f, 0, 3},
                                                                 {1, 1, 1, 1, 3},
                                                                 {0.8f, 0.6f, 0.4f, 2, 3}};
                                         return px[x][c];
                                     });
    Node& pre = add(g, "Premult", "Premult1");
    Node& un = add(g, "Unpremult", "Unpremult1");
    g.connect(src, pre, 0);
    g.connect(pre, un, 0);
    Image a = renderChecked(pre);
    CHECK_NEAR(a.pixel(0, 0)[0], 0.25f);
    CHECK_NEAR(a.pixel(0, 0)[2], 0.1f);
    CHECK(a.pixel(0, 0)[3] == 0.5f && a.pixel(0, 0)[4] == 3.0f);  // alpha and others untouched
    CHECK(a.pixel(1, 0)[0] == 0.0f);
    CHECK_NEAR(a.pixel(3, 0)[1], 1.2f);
    Image b = renderChecked(un);  // and back, except where alpha is 0
    CHECK_NEAR(b.pixel(0, 0)[1], 0.4f);
    CHECK_NEAR(b.pixel(3, 0)[0], 0.8f);
    CHECK(b.pixel(1, 0)[0] == 0.0f);

    // Without alpha: unchanged.
    auto& rgb = g.create<SourceNode>("Rgb", Box{0, 0, 2, 2}, ChannelList{"R", "G", "B"}, coord);
    Node& un2 = add(g, "Unpremult", "Unpremult2");
    g.connect(rgb, un2, 0);
    CHECK(renderChecked(un2).pixel(1, 1)[2] == coord(1, 1, 2));
}

static void testBlur() {
    Graph g;
    // One bright pixel at (10, 10).
    auto& dot = g.create<SourceNode>("Dot", Box{0, 0, 21, 21}, ChannelList{"R", "G", "B", "A"},
                                     [](int x, int y, int) { return x == 10 && y == 10 ? 1.0f : 0.0f; });
    auto& blur = static_cast<BlurNode&>(add(g, "Blur", "Blur1"));
    g.connect(dot, blur, 0);
    Engine e;

    // Size 0: untouched.
    CHECK((e.info(blur, {}).dataWindow == Box{0, 0, 21, 21}));
    CHECK(renderChecked(blur).pixel(10, 10)[0] == 1.0f);

    // Box kernels: size 2 weights the end pixels by half.
    blur.setParam("filter", std::string("box"));
    CHECK((blur.kernel(2) == std::vector<float>{0.25f, 0.5f, 0.25f}));
    CHECK((blur.kernel(3) == std::vector<float>{1 / 3.0f, 1 / 3.0f, 1 / 3.0f}));
    blur.setParam("size", Vec2{3, 0});  // across only
    CHECK((e.info(blur, {}).dataWindow == Box{-1, 0, 22, 21}));
    Image img = renderChecked(blur);
    CHECK_NEAR(img.pixel(9, 10)[0], 1 / 3.0f);
    CHECK_NEAR(img.pixel(11, 10)[1], 1 / 3.0f);
    CHECK(img.pixel(12, 10)[0] == 0.0f && img.pixel(10, 9)[0] == 0.0f);

    // Gaussian: reaches size/2 each way, symmetric, adds up to what went in.
    blur.setParam("filter", std::string("gaussian"));
    blur.setParam("size", 9.0);  // one number: both directions
    CHECK((e.info(blur, {}).dataWindow == Box{-5, -5, 26, 26}));
    img = renderChecked(blur);
    double sum = 0;
    for (int y = 5; y <= 15; ++y)
        for (int x = 5; x <= 15; ++x) sum += img.pixel(x, y)[0];
    CHECK(std::fabs(sum - 1.0) < 1e-5);
    CHECK(img.pixel(10, 10)[0] > img.pixel(11, 10)[0] && img.pixel(11, 10)[0] > img.pixel(13, 10)[0]);
    CHECK(img.pixel(7, 10)[0] == img.pixel(13, 10)[0] && img.pixel(10, 7)[0] == img.pixel(13, 10)[0]);
    CHECK(img.pixel(15, 10)[0] > 0.0f);

    blur.setParam("size", -1.0);
    CHECK_THROWS(e.info(blur, {}), "Blur1: size must be from 0 to 10000");
    blur.setParam("size", 2.0);
    blur.setParam("filter", std::string("triangle"));
    CHECK_THROWS(e.info(blur, {}), "Blur1: filter must be gaussian or box, not 'triangle'");
}

int main() {
    try {
        testCrop();
        testReformat();
        testShuffle();
        testPremult();
        testBlur();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        return 1;
    }
    return test::finish("more nodes");
}
