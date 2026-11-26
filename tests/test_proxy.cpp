// Proxy rendering (RenderContext::scale): a render at half size should look
// like the full-size render shrunk down.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>

#include <unistd.h>

#include "TestUtil.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/Registry.h"

namespace fs = std::filesystem;
using namespace comp;

namespace {

fs::path g_dir;
const Box kData{-8, -6, 136, 102};  // overscan around the frame
const Box kFrame{0, 0, 128, 96};

// Smooth, so shrinking and filtering at different sizes agree closely.
float plate(int x, int y, int c) {
    if (c == 3) return 1.0f;
    return static_cast<float>(0.5 + 0.35 * std::sin(x * 0.07 + c) * std::cos(y * 0.09) + 0.002 * x);
}

Node& add(Graph& g, const char* type, const char* name) { return g.add(createNode(type, name)); }

Node& read(Graph& g) {
    Node& r = add(g, "Read", "Read1");
    r.setParam("file", (g_dir / "plate.exr").string());
    return r;
}

// Renders `node` at full size and at `scale`; the proxy render must cover
// the full one's windows scaled, and match its pixels shrunk down to within
// `tol` (the largest difference in any channel).
//
// With `interior`, only pixels well inside the image count (alpha solid all
// around): at a hard edge (a rotated border, the data window's edge) a step
// resampled at two sizes legitimately differs.
void compare(const char* what, const Node& node, double tol, double scale = 0.5, bool interior = false,
             Format format = {128, 96}) {
    Engine e;
    const RenderContext full{1, format, 1.0}, proxy{1, format, scale};
    const ImageInfo fi = e.info(node, full), pi = e.info(node, proxy);
    if (!(pi.displayWindow == fi.displayWindow.scaled(scale))) {
        std::fprintf(stderr, "%s: display window not scaled\n", what);
        ++test::failures;
    }
    const Box region = fi.dataWindow.unite(fi.displayWindow);
    const Image big = e.render(node, full, region);
    const Box small = region.scaled(scale);
    const Image shrunk = downscale(big, small, scale);
    const Image got = e.render(node, proxy, small);
    double worst = 0;
    int wx = 0, wy = 0;
    // Skip the outermost ring: partial pixels at the region's edge differ in
    // how much they cover.
    for (int y = small.y0 + 1; y < small.y1 - 1; ++y)
        for (int x = small.x0 + 1; x < small.x1 - 1; ++x)
            for (int c = 0; c < got.numChannels(); ++c) {
                if (interior) {
                    bool solid = true;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) solid = solid && shrunk.at(x + dx, y + dy, 3) > 0.999f;
                    if (!solid) continue;
                }
                if (std::fabs(got.pixel(x, y)[c] - shrunk.pixel(x, y)[c]) > worst)
                    worst = std::fabs(got.pixel(x, y)[c] - shrunk.pixel(x, y)[c]), wx = x, wy = y;
            }
    if (!(worst <= tol)) {
        std::fprintf(stderr, "%s: proxy differs from the shrunk full render by %g (allowed %g) at %d,%d in %d,%d-%d,%d\n", what,
                     worst, tol, wx, wy, small.x0, small.y0, small.x1, small.y1);
        ++test::failures;
    }
}

}  // namespace

static void testDownscaleAndBoxes() {
    // Box::scaled rounds outward.
    CHECK((Box{1, 1, 5, 5}.scaled(0.5) == Box{0, 0, 3, 3}));
    CHECK((Box{-3, 0, 4, 2}.scaled(0.5) == Box{-2, 0, 2, 1}));
    CHECK((kFrame.scaled(0.25) == Box{0, 0, 32, 24}));
    CHECK((kFrame.scaled(1) == kFrame));
    // Area averages, partial pixels weighted.
    Image src(Box{0, 0, 4, 4}, {"R"});  // each row 0 1 2 3
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) src.pixel(x, y)[0] = float(x);
    const Image half = downscale(src, Box{0, 0, 2, 1}, 0.5);
    CHECK(half.pixel(0, 0)[0] == 0.5f && half.pixel(1, 0)[0] == 2.5f);
    const Image third = downscale(src, Box{0, 0, 2, 1}, 1 / 1.5);  // each covers 1.5 pixels
    CHECK_NEAR(third.pixel(0, 0)[0], (0 * 1 + 1 * 0.5) / 1.5);
    CHECK_NEAR(third.pixel(1, 0)[0], (1 * 0.5 + 2 * 1) / 1.5);

    Graph g;
    Node& r = read(g);
    Engine e;
    CHECK_THROWS(e.info(r, {1, {}, 0.0}), "render scale must be more than 0 and at most 1");
    CHECK_THROWS(e.render(r, {1, {}, 1.5}, {0, 0, 1, 1}), "render scale must be more than 0 and at most 1");
}

static void testNodes() {
    Graph g;
    Node& r = read(g);
    {
        Engine e;
        const ImageInfo info = e.info(r, {1, {}, 0.5});
        CHECK((info.dataWindow == Box{-4, -3, 68, 51}) && (info.displayWindow == Box{0, 0, 64, 48}));
    }
    compare("Read", r, 1e-6);
    compare("Read at 1/4", r, 1e-6, 0.25);
    compare("Read at 0.3", r, 1e-6, 0.3);

    // Pixel-wise: exactly as the shrunk full render.
    Node& grade = add(g, "Grade", "Grade1");
    g.connect(r, grade, 0);
    grade.setParam("multiply", 1.5);
    compare("Grade", grade, 1e-5);

    // Whole-pixel distances at half size stay exact.
    Node& crop = add(g, "Crop", "Crop1");
    g.connect(r, crop, 0);
    crop.setParam("box_min", Vec2{10, 8});
    crop.setParam("box_max", Vec2{100, 80});
    compare("Crop", crop, 1e-6);
    crop.setParam("reformat", true);
    compare("Crop reformat", crop, 1e-6);

    Node& c = add(g, "Constant", "Constant1");
    c.setParam("color", Color{0.2, 0.4, 0.6, 1});
    compare("Constant", c, 1e-6);

    // Filtering at another size: close, not exact.
    Node& xf = add(g, "Transform", "Transform1");
    g.connect(r, xf, 0);
    xf.setParam("translate", Vec2{12, -6});
    xf.setParam("rotate", 10.0);
    xf.setParam("scale", Vec2{0.8, 0.8});
    xf.setParam("center", Vec2{64, 48});
    compare("Transform", xf, 0.002, 0.5, true);

    Node& rf = add(g, "Reformat", "Reformat1");
    g.connect(r, rf, 0);
    rf.setParam("format", std::string("custom"));
    rf.setParam("size", Vec2{96, 96});
    rf.setParam("resize", std::string("fit"));
    compare("Reformat", rf, 0.002, 0.5, true);

    Node& blur = add(g, "Blur", "Blur1");
    g.connect(r, blur, 0);
    blur.setParam("size", Vec2{12, 8});
    compare("Blur", blur, 0.002, 0.5, true);

    // Roto: exact inside and out; the edge is antialiased at each size, so
    // differs a little there.
    Node& roto = add(g, "Roto", "Roto1");
    g.connect(r, roto, 0);
    roto.setParam("shape", std::string("M 20 20 C 60 0 110 20 100 60 L 40 80 C 20 70 10 40 20 20 Z"));
    roto.setParam("feather", 8.0);
    roto.setParam("output", std::string("rgba"));  // so the matte shows in RGB (the plate's alpha is solid)
    roto.setParam("color", Color{1, 0.2, 0.1, 1});
    compare("Roto", roto, 0.03);

    // The scale is part of the cache key.
    Engine cached({.threads = 2, .tileSize = 16, .cacheBytes = 1 << 24});
    const Image a = cached.render(grade, {1, {}, 1.0}, {0, 0, 4, 4});
    const Image b = cached.render(grade, {1, {}, 0.5}, {0, 0, 4, 4});
    CHECK(a.pixel(3, 3)[0] != b.pixel(3, 3)[0]);
}

int main() {
    g_dir = fs::temp_directory_path() / ("comp_proxy_" + std::to_string(::getpid()));
    fs::create_directories(g_dir);
    test::ExrOptions o;
    o.dataWindow = kData;
    o.displayWindow = kFrame;
    test::writeImage((g_dir / "plate.exr").string(), o, plate);
    try {
        testDownscaleAndBoxes();
        testNodes();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    fs::remove_all(g_dir);
    return test::finish("proxy");
}
