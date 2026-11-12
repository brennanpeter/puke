// Golden-image tests: render small comps and compare them with reference EXRs
// stored in tests/golden/.
//
//   test_golden            compare against the references (what ctest runs)
//   test_golden --update   re-render and overwrite the references
//   test_golden NAME...    only run the named cases (works with --update too)
//
// Only use --update when an output change is intended, then look at the new
// references (e.g. `iv tests/golden/*.exr`) before committing them.
// On a mismatch, the actual render is written to golden_failures/ in the build
// folder; compare with `idiff golden_failures/NAME.exr tests/golden/NAME.exr`.
//
// References are float EXRs read and written with OpenImageIO directly, so a
// bug in comp's own Read or Write can't hide itself.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "TestUtil.h"
#include "comp/engine/Engine.h"
#include "comp/graph/Graph.h"
#include "comp/nodes/GradeNode.h"
#include "comp/nodes/MergeNode.h"
#include "comp/nodes/ReadNode.h"
#include "comp/nodes/TransformNode.h"

namespace fs = std::filesystem;
namespace OIIO = OIIO_NAMESPACE;
using namespace comp;

// Largest per-channel difference allowed. Tight enough to catch real changes,
// loose enough for last-bit differences between maths libraries.
constexpr float kTolerance = 1e-5f;

static fs::path g_inputs;  // generated input images

// ---------------------------------------------------------------- inputs

static const Box kFormat{0, 0, 96, 64};

// Background plate with overscan: an opaque two-way gradient plus some texture.
static void makeBackground(const std::string& path) {
    test::ExrOptions o;
    o.dataWindow = kFormat.padded(8);
    o.displayWindow = kFormat;
    test::writeImage(path, o, [](int x, int y, int c) {
        if (c == 3) return 1.0f;
        const float fx = x / 96.0f, fy = y / 64.0f;
        const float base[] = {0.1f + 0.5f * fx, 0.2f + 0.3f * fy, 0.6f - 0.4f * fx * fy};
        return base[c] + 0.05f * std::sin(0.7f * x + 1.3f * c) * std::cos(0.5f * y);
    });
}

// Foreground element: a soft-edged premultiplied disc with stripes, and depth.
static void makeForeground(const std::string& path) {
    test::ExrOptions o;
    o.dataWindow = {12, 6, 72, 58};
    o.displayWindow = kFormat;
    o.channels = {"R", "G", "B", "A", "Z"};
    test::writeImage(path, o, [](int x, int y, int c) {
        const float dx = x + 0.5f - 42, dy = y + 0.5f - 32;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float alpha = std::clamp((24.0f - r) / 4.0f, 0.0f, 1.0f);
        if (c == 3) return alpha;
        if (c == 4) return 10.0f + 0.1f * x;  // depth, not premultiplied
        const float stripe = 0.5f + 0.5f * std::sin(0.6f * (x + y) + 2.0f * c);
        const float colour[] = {0.9f * stripe, 0.4f + 0.3f * stripe, 0.2f};
        return colour[c] * alpha;
    });
}

// ---------------------------------------------------------------- cases

struct Case {
    std::string name;
    // Builds the graph and returns the node to render.
    std::function<Node&(Graph&)> build;
};

static ReadNode& read(Graph& g, const std::string& name, const std::string& file) {
    auto& r = g.create<ReadNode>(name);
    r.setParam("file", (g_inputs / file).string());
    return r;
}

static TransformNode& transform(Graph& g, Node& input, const std::string& filter) {
    auto& xf = g.create<TransformNode>("Transform1");
    g.connect(input, xf, 0);
    xf.setParam("center", Vec2{42, 32});
    xf.setParam("rotate", 17.0);
    xf.setParam("scale", Vec2{1.3, 0.7});
    xf.setParam("translate", Vec2{6.25, -3.5});
    xf.setParam("filter", filter);
    return xf;
}

static MergeNode& merge(Graph& g, Node& b, Node& a, const std::string& op) {
    auto& m = g.create<MergeNode>("Merge_" + op);
    g.connect(b, m, 0);
    g.connect(a, m, 1);
    m.setParam("operation", op);
    return m;
}

static std::vector<Case> cases() {
    return {
        {"read_overscan", [](Graph& g) -> Node& { return read(g, "BG", "bg.exr"); }},

        {"grade", [](Graph& g) -> Node& {
             auto& grade = g.create<GradeNode>("Grade1");
             g.connect(read(g, "FG", "fg.exr"), grade, 0);
             grade.setParam("blackpoint", Color{0.02, 0.0, 0.01, 0});
             grade.setParam("whitepoint", Color{0.9, 1.0, 0.95, 1});
             grade.setParam("lift", Color{0.01, 0.02, 0.05, 0});
             grade.setParam("gain", Color{1.4, 1.1, 0.7, 1});
             grade.setParam("multiply", 1.1);
             grade.setParam("offset", Color{0.0, -0.02, 0.01, 0});
             grade.setParam("gamma", Color{1.2, 0.9, 1.0, 1});
             return grade;
         }},

        {"transform_nearest", [](Graph& g) -> Node& { return transform(g, read(g, "FG", "fg.exr"), "nearest"); }},
        {"transform_bilinear", [](Graph& g) -> Node& { return transform(g, read(g, "FG", "fg.exr"), "bilinear"); }},
        {"transform_cubic", [](Graph& g) -> Node& { return transform(g, read(g, "FG", "fg.exr"), "cubic"); }},

        {"merge_over", [](Graph& g) -> Node& {
             return merge(g, read(g, "BG", "bg.exr"), read(g, "FG", "fg.exr"), "over");
         }},
        {"merge_screen", [](Graph& g) -> Node& {
             return merge(g, read(g, "BG", "bg.exr"), read(g, "FG", "fg.exr"), "screen");
         }},
        {"merge_stencil", [](Graph& g) -> Node& {
             return merge(g, read(g, "BG", "bg.exr"), read(g, "FG", "fg.exr"), "stencil");
         }},

        // One element used twice, moved far left and far right. (Region handling
        // for shared inputs is checked precisely in test_engine; here both
        // branches need the whole element.)
        {"shared_input", [](Graph& g) -> Node& {
             auto& fg = read(g, "FG", "fg.exr");
             auto& left = g.create<TransformNode>("Left");
             auto& right = g.create<TransformNode>("Right");
             g.connect(fg, left, 0);
             g.connect(fg, right, 0);
             left.setParam("translate", Vec2{-30.5, 4});
             right.setParam("translate", Vec2{30.5, -4});
             return merge(g, right, left, "over");
         }},

        // A small but realistic comp: grade an element, move it, put it over the
        // plate, then add a shrunken, rotated copy on top. The element feeds two
        // branches, so the engine has to share its result.
        {"comp_chain", [](Graph& g) -> Node& {
             auto& bg = read(g, "BG", "bg.exr");
             auto& fg = read(g, "FG", "fg.exr");
             auto& grade = g.create<GradeNode>("Grade1");
             g.connect(fg, grade, 0);
             grade.setParam("gain", Color{1.2, 0.9, 0.8, 1});
             auto& move = transform(g, grade, "cubic");
             auto& over = merge(g, bg, move, "over");

             auto& small = g.create<TransformNode>("Small");
             g.connect(fg, small, 0);
             small.setParam("center", Vec2{42, 32});
             small.setParam("scale", 0.35);
             small.setParam("rotate", -40.0);
             small.setParam("translate", Vec2{-28, 18});
             small.setParam("filter", std::string("bilinear"));
             return merge(g, over, small, "plus");
         }},
    };
}

// ---------------------------------------------------------------- EXR I/O

static void writeExr(const fs::path& path, const Image& img, const Box& display) {
    const Box& r = img.region();
    OIIO::ImageSpec spec(r.width(), r.height(), img.numChannels(), OIIO::TypeDesc::FLOAT);
    spec.x = r.x0;
    spec.y = r.y0;
    spec.full_x = display.x0;
    spec.full_y = display.y0;
    spec.full_width = display.width();
    spec.full_height = display.height();
    spec.channelnames.assign(img.channels().begin(), img.channels().end());
    spec.attribute("compression", "zip");
    fs::create_directories(path.parent_path());
    auto out = OIIO::ImageOutput::create(path.string());
    if (!out || !out->open(path.string(), spec) || !out->write_image(OIIO::TypeDesc::FLOAT, img.data()) ||
        !out->close())
        throw Error("cannot write " + path.string());
}

static bool readExr(const fs::path& path, Image& img) {
    auto in = OIIO::ImageInput::open(path.string());
    if (!in) return false;
    const auto& s = in->spec();
    img = Image({s.x, s.y, s.x + s.width, s.y + s.height},
                ChannelList(s.channelnames.begin(), s.channelnames.end()));
    return in->read_image(0, 0, 0, s.nchannels, OIIO::TypeDesc::FLOAT, img.data());
}

// ---------------------------------------------------------------- runner

static bool runCase(const Case& c, bool update) {
    Graph g;
    Node& output = c.build(g);
    Engine engine;
    const ImageInfo info = engine.info(output, {});
    // Cover both the overscan and the full frame, so changes on either show up.
    const Box region = info.dataWindow.unite(info.displayWindow);
    const Image actual = engine.render(output, {}, region);

    const fs::path refPath = fs::path(COMP_GOLDEN_DIR) / (c.name + ".exr");
    if (update) {
        writeExr(refPath, actual, info.displayWindow);
        std::printf("updated %s\n", refPath.string().c_str());
        return true;
    }

    Image ref;
    if (!readExr(refPath, ref)) {
        std::fprintf(stderr, "%s: no reference image at %s (run test_golden --update %s)\n",
                     c.name.c_str(), refPath.string().c_str(), c.name.c_str());
        return false;
    }

    std::string problem;
    if (ref.region() != actual.region()) problem = "rendered region differs from the reference";
    else if (ref.channels() != actual.channels()) problem = "channel list differs from the reference";

    if (problem.empty()) {
        float worst = 0;
        int worstX = 0, worstY = 0, worstC = 0;
        long long bad = 0;
        for (int y = region.y0; y < region.y1; ++y)
            for (int x = region.x0; x < region.x1; ++x)
                for (int ch = 0; ch < actual.numChannels(); ++ch) {
                    const float d = std::fabs(actual.pixel(x, y)[ch] - ref.pixel(x, y)[ch]);
                    // !(d <= tol) also catches NaN.
                    if (!(d <= kTolerance)) ++bad;
                    if (!(d <= worst)) {
                        worst = d;
                        worstX = x, worstY = y, worstC = ch;
                    }
                }
        if (bad > 0) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                          "%lld values differ by more than %g; worst %g at (%d, %d) channel %s",
                          bad, kTolerance, worst, worstX, worstY, actual.channels()[worstC].c_str());
            problem = buf;
        }
    }

    if (problem.empty()) return true;

    const fs::path failPath = fs::current_path() / "golden_failures" / (c.name + ".exr");
    writeExr(failPath, actual, info.displayWindow);
    std::fprintf(stderr, "%s: %s\n  actual render written to %s\n", c.name.c_str(), problem.c_str(),
                 failPath.string().c_str());
    return false;
}

int main(int argc, char** argv) {
    bool update = false;
    std::vector<std::string> only;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--update") update = true;
        else only.push_back(arg);
    }

    g_inputs = fs::current_path() / "test_golden_inputs";
    fs::remove_all(g_inputs);
    fs::create_directories(g_inputs);
    fs::remove_all(fs::current_path() / "golden_failures");

    try {
        makeBackground((g_inputs / "bg.exr").string());
        makeForeground((g_inputs / "fg.exr").string());

        int ran = 0;
        for (const Case& c : cases()) {
            if (!only.empty() && std::find(only.begin(), only.end(), c.name) == only.end()) continue;
            ++ran;
            if (!runCase(c, update)) ++test::failures;
        }
        if (ran == 0) {
            std::fprintf(stderr, "no matching golden cases\n");
            ++test::failures;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }

    fs::remove_all(g_inputs);
    return test::finish("golden-image");
}
