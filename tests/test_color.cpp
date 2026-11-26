// Tests for color management: the built-in config, loading configs, and the
// ColorSpace node. Expected values come from published references, not from
// puke itself.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "TestUtil.h"
#include "puke/color/ColorConfig.h"
#include "puke/engine/Engine.h"
#include "puke/graph/Graph.h"
#include "puke/nodes/ColorSpaceNode.h"

using namespace puke;
using test::SourceNode;
namespace fs = std::filesystem;

static fs::path g_dir;

static bool close(float a, float b, float tol = 1e-5f) { return std::fabs(a - b) <= tol; }

// One RGBA pixel converted with the built-in config.
static std::array<float, 4> convertPixel(const std::string& from, const std::string& to, float r, float g, float b,
                                         float a = 1.0f) {
    Image img({0, 0, 1, 1}, {"R", "G", "B", "A"});
    float* p = img.pixel(0, 0);
    p[0] = r, p[1] = g, p[2] = b, p[3] = a;
    ColorConfig::builtin()->convert(img, from, to);
    return {p[0], p[1], p[2], p[3]};
}

// A minimal config of our own, to test loading from a file.
static const char* kTinyConfig = R"(ocio_profile_version: 2
roles:
  scene_linear: lin
  matte_paint: lin
  default: lin
file_rules:
  - !<Rule> {name: Default, colorspace: default}
displays:
  d:
    - !<View> {name: v, colorspace: lin}
colorspaces:
  - !<ColorSpace> {name: lin}
  - !<ColorSpace>
    name: double
    to_scene_reference: !<MatrixTransform> {matrix: [2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1]}
)";

static void testBuiltinConfig() {
    const auto c = ColorConfig::builtin();
    CHECK(c->source() == "built-in");
    CHECK(c->workingSpace() == "ACEScg");
    for (const char* name : {"ACES2065-1", "ACEScg", "ACEScct", "Linear Rec.709 (sRGB)", "sRGB - Texture", "Raw",
                             "sRGB - Display", "Rec.1886 Rec.709 - Display", "CIE-XYZ-D65"})
        CHECK(c->has(name));
    CHECK(c->has("scene_linear") && c->has("matte_paint") && !c->has("nope") && !c->has(""));
    CHECK(c->resolve("matte_paint") == "sRGB - Texture");
    CHECK(c->resolve("compositing_log") == "ACEScct");
    CHECK_THROWS(c->resolve("Rec709"), "unknown color space 'Rec709' in OCIO config 'built-in' (known: ");
    CHECK(c->isNoOp("ACEScg", "scene_linear") && c->isNoOp("Raw", "ACEScct"));
    CHECK(!c->isNoOp("ACEScg", "ACEScct"));
    CHECK(ColorConfig::builtin() == c);  // loaded once
}

static void testKnownValues() {
    // sRGB curve: 0.5 encoded is 0.214041 linear.
    auto p = convertPixel("sRGB - Texture", "Linear Rec.709 (sRGB)", 0.5f, 0.5f, 0.5f);
    CHECK(close(p[0], 0.214041f) && close(p[2], 0.214041f));
    // Rec.709 red in ACEScg (AP1): first column of the Rec.709 -> AP1 matrix.
    p = convertPixel("Linear Rec.709 (sRGB)", "ACEScg", 1, 0, 0);
    CHECK(close(p[0], 0.613097f) && close(p[1], 0.070194f) && close(p[2], 0.020616f));
    // ACEScg red in ACES2065-1 (AP0): first column of the AP1 -> AP0 matrix.
    p = convertPixel("ACEScg", "ACES2065-1", 1, 0, 0);
    CHECK(close(p[0], 0.695452f) && close(p[1], 0.044795f) && close(p[2], -0.005526f));
    // ACEScct: 18% grey encodes to 0.4135884.
    p = convertPixel("ACEScg", "ACEScct", 0.18f, 0.18f, 0.18f);
    CHECK(close(p[1], 0.413588f));
    // White stays white, sRGB to linear; data is never touched.
    p = convertPixel("matte_paint", "scene_linear", 1, 1, 1);
    CHECK(close(p[0], 1) && close(p[1], 1) && close(p[2], 1));
    p = convertPixel("Raw", "ACEScct", 0.3f, 0.6f, 0.9f);
    CHECK(p[0] == 0.3f && p[1] == 0.6f && p[2] == 0.9f);
}

static void testChannelsAndAlpha() {
    // Premultiplied color: unpremultiplied, converted, premultiplied again.
    // Alpha itself is never changed.
    auto p = convertPixel("sRGB - Texture", "Linear Rec.709 (sRGB)", 0.25f, 0.25f, 0.25f, 0.5f);
    CHECK(close(p[0], 0.214041f * 0.5f) && p[3] == 0.5f);
    // Transparent black stays put, even into a log space (ACEScct maps 0 to
    // about 0.073). Alpha-0 pixels with color (glows) are converted as is.
    p = convertPixel("ACEScg", "ACEScct", 0, 0, 0, 0);
    CHECK(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 0);
    p = convertPixel("ACEScg", "ACEScct", 0, 0, 0, 1);
    CHECK(close(p[0], 0.0729055f));
    p = convertPixel("ACEScg", "ACEScct", 0.18f, 0.18f, 0.18f, 0);
    CHECK(close(p[0], 0.413588f) && p[3] == 0);

    // Extra channels pass through; a lone Y is converted as grey.
    Image img({0, 0, 1, 1}, {"R", "G", "B", "Z"});
    float* q = img.pixel(0, 0);
    q[0] = q[1] = q[2] = 0.5f, q[3] = 7.0f;
    ColorConfig::builtin()->convert(img, "sRGB - Texture", "Linear Rec.709 (sRGB)");
    CHECK(close(q[0], 0.214041f) && q[3] == 7.0f);

    Image grey({0, 0, 1, 1}, {"Y"});
    grey.pixel(0, 0)[0] = 0.5f;
    ColorConfig::builtin()->convert(grey, "sRGB - Texture", "Linear Rec.709 (sRGB)");
    CHECK(close(grey.pixel(0, 0)[0], 0.214041f));

    Image depth({0, 0, 1, 1}, {"Z"});
    ColorConfig::builtin()->convert(depth, "ACEScg", "scene_linear");  // no-op: fine
    CHECK_THROWS(ColorConfig::builtin()->convert(depth, "ACEScg", "ACEScct"), "no R, G and B channels (or Y)");

    // `area` limits the conversion.
    Image two({0, 0, 2, 1}, {"R", "G", "B"});
    for (float& v : std::span(two.data(), 6)) v = 0.5f;
    ColorConfig::builtin()->convert(two, "sRGB - Texture", "Linear Rec.709 (sRGB)", {1, 0, 2, 1});
    CHECK(two.pixel(0, 0)[0] == 0.5f && close(two.pixel(1, 0)[0], 0.214041f));
}

static void testConfigFiles() {
    const std::string path = (g_dir / "tiny.ocio").string();
    std::ofstream(path) << kTinyConfig;
    const auto tiny = ColorConfig::fromFile(path);
    CHECK(tiny->source() == path && tiny->workingSpace() == "lin");
    CHECK((tiny->colorSpaces() == std::vector<std::string>{"lin", "double"}));
    CHECK(tiny->hash() != ColorConfig::builtin()->hash());

    CHECK_THROWS(ColorConfig::fromFile((g_dir / "missing.ocio").string()), "OCIO config '");
    std::ofstream(g_dir / "bad.ocio") << "ocio_profile_version: 2\ncolorspaces: [oops\n";
    CHECK_THROWS(ColorConfig::fromFile((g_dir / "bad.ocio").string()), "bad.ocio': ");

    // $OCIO picks the config; unset or empty means the built-in one.
    setenv("OCIO", path.c_str(), 1);
    CHECK(ColorConfig::fromEnvironment()->source() == path);
    setenv("OCIO", "", 1);
    CHECK(ColorConfig::fromEnvironment() == ColorConfig::builtin());
    unsetenv("OCIO");
    CHECK(ColorConfig::fromEnvironment() == ColorConfig::builtin());

    // setCurrent(nullptr) reloads from the environment on next use.
    setenv("OCIO", path.c_str(), 1);
    ColorConfig::setCurrent(nullptr);
    CHECK(ColorConfig::current()->source() == path);
    unsetenv("OCIO");
    ColorConfig::setCurrent(nullptr);
    CHECK(ColorConfig::current() == ColorConfig::builtin());
}

static void testDisplays() {
    const auto c = ColorConfig::builtin();
    CHECK((c->displays() == std::vector<std::string>{"sRGB - Display", "Rec.1886 Rec.709 - Display"}));
    CHECK(c->defaultDisplay() == "sRGB - Display");
    CHECK((c->views("sRGB - Display") == std::vector<std::string>{"ACES 1.0 - SDR Video", "Un-tone-mapped", "Raw"}));
    CHECK(c->defaultView("sRGB - Display") == "ACES 1.0 - SDR Video");

    auto show = [&](const std::string& display, const std::string& view, float v, float a = 1.0f) {
        Image img({0, 0, 1, 1}, {"R", "G", "B", "A"});
        float* p = img.pixel(0, 0);
        p[0] = p[1] = p[2] = v, p[3] = a;
        c->convertToDisplay(img, display, view);
        return std::array<float, 4>{p[0], p[1], p[2], p[3]};
    };
    // Un-tone-mapped: 18% grey through the sRGB curve is 0.4613561.
    CHECK(close(show("sRGB - Display", "Un-tone-mapped", 0.18f)[1], 0.461356f, 1e-4f));
    // Raw: untouched. Alpha is never touched, and color isn't unpremultiplied.
    CHECK(show("sRGB - Display", "Raw", 0.18f)[0] == 0.18f);
    CHECK(close(show("sRGB - Display", "Un-tone-mapped", 0.18f, 0.5f)[1], 0.461356f, 1e-4f));
    CHECK(show("sRGB - Display", "Un-tone-mapped", 0.18f, 0.5f)[3] == 0.5f);
    // The ACES view tone maps: mid grey lands darker than un-tone-mapped,
    // and bright values roll off below 1 instead of clipping.
    const float aces = show("sRGB - Display", "ACES 1.0 - SDR Video", 0.18f)[1];
    CHECK(aces > 0.3f && aces < 0.46f);
    CHECK(show("sRGB - Display", "ACES 1.0 - SDR Video", 4.0f)[1] < 1.0f);

    Image img({0, 0, 1, 1}, {"R", "G", "B"});
    CHECK_THROWS(c->convertToDisplay(img, "Nope", "Raw"), "unknown display 'Nope'");
    CHECK_THROWS(c->convertToDisplay(img, "sRGB - Display", "Nope"), "has no view 'Nope'");
}

static void testColorSpaceNode() {
    // An offset data window, an alpha that varies, and an extra channel.
    const Box window{-3, 2, 13, 10};
    Graph g;
    auto& src = g.create<SourceNode>("Src", window, ChannelList{"R", "G", "B", "A", "Z"}, [](int x, int y, int c) {
        if (c == 3) return 0.25f + 0.05f * (x + 3);
        if (c == 4) return 100.0f + x;
        return 0.01f * (x + 3) + 0.02f * y + 0.1f * c;
    });
    auto& cs = g.create<ColorSpaceNode>("ColorSpace1");
    g.connect(src, cs, 0);

    Engine e(EngineOptions{.threads = 4, .tileSize = 4});
    const Box region{-6, 0, 16, 12};
    const Image input = e.render(src, {}, region);

    // Defaults (scene_linear to scene_linear): unchanged.
    Image out = e.render(cs, {}, region);
    CHECK(std::equal(out.data(), out.data() + region.area() * 5, input.data()));

    // To ACEScct: matches converting the input directly, inside the data
    // window only (ACEScct turns 0 into a negative number).
    cs.setParam("out", std::string("ACEScct"));
    out = e.render(cs, {}, region);
    Image expected = input;
    ColorConfig::builtin()->convert(expected, "scene_linear", "ACEScct", window);
    bool same = true;
    for (int y = region.y0; y < region.y1; ++y)
        for (int x = region.x0; x < region.x1; ++x)
            for (int c = 0; c < 5; ++c) same = same && out.pixel(x, y)[c] == expected.pixel(x, y)[c];
    CHECK(same);
    CHECK(out.pixel(-5, 1)[0] == 0.0f && out.pixel(0, 5)[4] == 100.0f);
    CHECK(e.info(cs, {}).dataWindow == window);

    // And back again.
    auto& back = g.create<ColorSpaceNode>("ColorSpace2");
    back.setParam("in", std::string("compositing_log"));  // a role works too
    g.connect(cs, back, 0);
    const Image round = e.render(back, {}, region);
    float worst = 0;
    for (int y = window.y0; y < window.y1; ++y)
        for (int x = window.x0; x < window.x1; ++x)
            for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs(round.pixel(x, y)[c] - input.pixel(x, y)[c]));
    CHECK(worst < 1e-5f);

    cs.setParam("out", std::string("nope"));
    CHECK_THROWS(e.render(cs, {}, region), "ColorSpace1: unknown color space 'nope'");
}

static void testCacheFollowsConfig() {
    // Changing the current config drops cached tiles of color nodes.
    const std::string path = (g_dir / "tiny.ocio").string();
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 4, 4}, ChannelList{"R", "G", "B"},
                                     [](int, int, int) { return 0.25f; });
    auto& cs = g.create<ColorSpaceNode>("CS");
    cs.setParam("in", std::string("double"));  // exists only in the tiny config
    g.connect(src, cs, 0);
    Engine e(EngineOptions{.cacheBytes = 1 << 20});

    ColorConfig::setCurrent(ColorConfig::fromFile(path));
    CHECK(close(e.render(cs, {}, {0, 0, 4, 4}).pixel(1, 1)[0], 0.5f));
    e.render(cs, {}, {0, 0, 4, 4});
    CHECK(e.lastStats().tilesComputed == 0);  // cached

    ColorConfig::setCurrent(ColorConfig::builtin());
    CHECK_THROWS(e.render(cs, {}, {0, 0, 4, 4}), "CS: unknown color space 'double'");  // not a stale hit
    ColorConfig::setCurrent(nullptr);
}

int main() {
    g_dir = fs::current_path() / "test_color_tmp";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    try {
        testBuiltinConfig();
        testKnownValues();
        testChannelsAndAlpha();
        testConfigFiles();
        testDisplays();
        testColorSpaceNode();
        testCacheFollowsConfig();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }
    if (test::failures == 0) fs::remove_all(g_dir);
    return test::finish("color");
}
