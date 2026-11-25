// Path (SVG path data) and the Roto node.

#include <cmath>
#include <cstdio>
#include <string>

#include "TestUtil.h"
#include "comp/core/Path.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/Registry.h"
#include "comp/nodes/RotoNode.h"

using namespace comp;
using test::SourceNode;

namespace {

float at(const std::vector<float>& cov, const Box& r, int x, int y) {
    return cov[static_cast<std::size_t>(y - r.y0) * r.width() + (x - r.x0)];
}

// Whole and tiled renders must match exactly.
Image renderChecked(const Node& node, const RenderContext& ctx = {1, Format{40, 30}}) {
    Engine whole({.threads = 0, .tileSize = 1 << 20, .cacheBytes = 0});
    Engine tiled({.threads = 4, .tileSize = 7, .cacheBytes = 0});
    const Box dw = whole.info(node, ctx).dataWindow;
    Image a = whole.render(node, ctx, dw), b = tiled.render(node, ctx, dw);
    CHECK(a.region() == b.region() &&
          std::equal(a.data(), a.data() + a.region().area() * a.numChannels(), b.data()));
    return a;
}

}  // namespace

static void testPathText() {
    const Path p = Path::parse("M 10 10 L 90 10 C 100 40, 100 60 90 90 L 10 90 Z");
    CHECK(p.curves().size() == 1 && p.curves()[0].size() == 4);
    const Path::Knot& k = p.curves()[0][1];  // (90,10): straight in, curved out
    CHECK((k.point == Path::Point{90, 10}) && (k.in == Path::Point{90, 10}) && (k.out == Path::Point{100, 40}));
    CHECK((p.curves()[0][2].in == Path::Point{100, 60}));
    CHECK(p.str() == "M 10 10 L 90 10 C 100 40 100 60 90 90 L 10 90 Z");
    CHECK(Path::parse(p.str()) == p);

    // A curved closing segment back onto the start: one knot, not two.
    const Path round = Path::parse("M 0 0 C 5 -5 15 -5 20 0 C 25 5 25 15 20 20 C 0 25 -5 5 0 0 Z");
    CHECK(round.curves()[0].size() == 3);
    CHECK((round.curves()[0][0].in == Path::Point{-5, 5}));
    CHECK(Path::parse(round.str()) == round);

    // Several curves; implicit repeats; no Z; exponents; signs.
    const Path two = Path::parse("M0,0 10,0 10,10 M 20 20 L 3e1 20 +30 -1.5e1");
    CHECK(two.curves().size() == 2 && two.curves()[0].size() == 3 && two.curves()[1].size() == 3);
    CHECK((two.curves()[1][2].point == Path::Point{30, -15}));
    CHECK(Path::parse("").empty() && Path::parse("  ").curves().empty());
    CHECK(Path::parse("M 0.1 0.2 L 1 2 Z").str() == "M 0.1 0.2 L 1 2 Z");  // shortest exact numbers

    CHECK_THROWS(Path::parse("M 0 0 Q 1 1 2 2"), "path: unsupported command 'Q' (only absolute M, L, C and Z are)");
    CHECK_THROWS(Path::parse("M 0 0 l 1 1"), "unsupported command 'l'");
    CHECK_THROWS(Path::parse("M 0 0 L 1"), "path: expected a number for 'L'");
    CHECK_THROWS(Path::parse("L 1 1"), "path: 'L' before any 'M'");
    CHECK_THROWS(Path::parse("5 5"), "path: a number where a command");
    CHECK_THROWS(Path::parse("M 0 0 Z 4 4"), "path: a number where a command");

    // Polygons: straight segments as they are, curves in short pieces.
    const auto polys = p.polygons(1.0);
    CHECK(polys.size() == 1 && polys[0].size() > 40);
    CHECK((polys[0][0] == Path::Point{10, 10}) && (polys[0][1] == Path::Point{90, 10}));
    CHECK((p.bounds() == Box{10, 10, 100, 90}));
    CHECK(Path().bounds().empty());
}

static void testRasterize() {
    const Box r{0, 0, 20, 20};
    // On pixel edges: exactly 1 inside, 0 outside.
    auto cov = rasterize({{{2, 2}, {8, 2}, {8, 8}, {2, 8}}}, 0, r);
    CHECK(at(cov, r, 2, 2) == 1.0f && at(cov, r, 7, 7) == 1.0f && at(cov, r, 1, 5) == 0.0f && at(cov, r, 8, 5) == 0.0f);
    // Through pixel centres: half.
    cov = rasterize({{{2.5, 2}, {8, 2}, {8, 8}, {2.5, 8}}}, 0, r);
    CHECK_NEAR(at(cov, r, 2, 5), 0.5f);

    // Feather: soft outside the edge, solid inside, gone past it.
    cov = rasterize({{{0, 0}, {10, 0}, {10, 10}, {0, 10}}}, 4, r);
    CHECK(at(cov, r, 9, 5) == 1.0f);
    float last = 1.0f;
    for (int x = 10; x <= 14; ++x) {
        CHECK(at(cov, r, x, 5) < last);
        last = at(cov, r, x, 5);
    }
    CHECK_NEAR(at(cov, r, 12, 5), 1 - (0.6 * 0.6 * (3 - 1.2)));  // s = 2.5: t = 3/5
    CHECK(at(cov, r, 15, 5) == 0.0f);

    // Nonzero fill: an inner loop running the other way is a hole; the same
    // way, it's just filled.
    const std::vector<Path::Point> outer{{0, 0}, {20, 0}, {20, 20}, {0, 20}};
    cov = rasterize({outer, {{5, 5}, {5, 15}, {15, 15}, {15, 5}}}, 0, r);
    CHECK(at(cov, r, 10, 10) == 0.0f && at(cov, r, 2, 10) == 1.0f);
    cov = rasterize({outer, {{5, 5}, {15, 5}, {15, 15}, {5, 15}}}, 0, r);
    CHECK(at(cov, r, 10, 10) == 1.0f);
}

static void testRotoNode() {
    Graph g;
    Node& roto = g.add(createNode("Roto", "Roto1"));
    Engine e;
    const RenderContext ctx{1, Format{40, 30}};

    // No shape: nothing.
    ImageInfo info = e.info(roto, ctx);
    CHECK((info.displayWindow == Box{0, 0, 40, 30}) && info.dataWindow.empty());
    CHECK((info.channels == ChannelList{"R", "G", "B", "A"}));

    roto.setParam("shape", std::string("M 10 5 L 30 5 L 30 25 L 10 25 Z"));
    roto.setParam("feather", 2.0);
    CHECK((e.info(roto, ctx).dataWindow == Box{7, 2, 33, 28}));  // bounds + feather + 1
    Image img = renderChecked(roto);
    CHECK(img.pixel(20, 15)[3] == 1.0f && img.pixel(20, 15)[0] == 0.0f);  // alpha only
    CHECK(img.pixel(31, 15)[3] > 0.0f && img.pixel(31, 15)[3] < 1.0f);

    roto.setParam("opacity", 0.5);
    roto.setParam("output", std::string("rgba"));
    roto.setParam("color", Color{1, 0.5, 0, 0.5});
    img = renderChecked(roto);
    const float* p = img.pixel(20, 15);  // matte 0.5 x color alpha 0.5
    CHECK(p[3] == 0.25f && p[0] == 0.25f && p[1] == 0.125f && p[2] == 0.0f);

    // Over a bg: alpha over its alpha, the rest passes through (alpha mode).
    auto& bg = g.create<SourceNode>("Bg", Box{0, 0, 40, 30}, ChannelList{"R", "G", "B", "A", "Z"},
                                    [](int, int, int c) { return c == 3 ? 0.5f : 0.2f; }, Box{0, 0, 40, 30});
    g.connect(bg, roto, 0);
    roto.setParam("output", std::string("alpha"));
    roto.setParam("opacity", 1.0);
    info = e.info(roto, ctx);
    CHECK((info.channels == ChannelList{"R", "G", "B", "A", "Z"}) && (info.dataWindow == Box{0, 0, 40, 30}));
    img = renderChecked(roto);
    CHECK(img.pixel(20, 15)[3] == 1.0f && img.pixel(20, 15)[0] == 0.2f && img.pixel(20, 15)[4] == 0.2f);
    CHECK(img.pixel(1, 1)[3] == 0.5f);

    // Inverted: the matte outside, over the whole frame.
    g.disconnect(roto, 0);
    roto.setParam("invert", true);
    CHECK((e.info(roto, ctx).dataWindow == Box{0, 0, 40, 30}));
    img = renderChecked(roto);
    CHECK(img.pixel(1, 1)[3] == 1.0f && img.pixel(20, 15)[3] == 0.0f);

    roto.setParam("shape", std::string("M 0 0 Q 1 1 2 2"));
    CHECK_THROWS(e.info(roto, ctx), "Roto1: path: unsupported command 'Q'");
    roto.setParam("shape", std::string(""));
    roto.setParam("output", std::string("rgb"));
    CHECK_THROWS(e.info(roto, ctx), "Roto1: output must be alpha or rgba, not 'rgb'");
}

int main() {
    try {
        testPathText();
        testRasterize();
        testRotoNode();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        return 1;
    }
    return test::finish("roto");
}
