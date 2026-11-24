// Tests for Read and Write. Run from the build directory (ctest does this).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "TestUtil.h"
#include "comp/core/FramePattern.h"
#include "comp/engine/Engine.h"
#include "comp/engine/Snapshot.h"
#include "comp/graph/Graph.h"
#include "comp/color/ColorConfig.h"
#include "comp/nodes/ReadNode.h"
#include "comp/nodes/Registry.h"
#include "comp/nodes/WriteNode.h"

namespace OIIO = OIIO_NAMESPACE;
namespace fs = std::filesystem;
using namespace comp;
using test::SourceNode;

static Engine g_engine;
static fs::path g_dir;  // scratch folder for this run

static std::string tmp(const std::string& name) { return (g_dir / name).string(); }

// A recognisable value for every pixel and channel, varying by frame.
static float testValue(int x, int y, int c, int frame) {
    return 0.01f * x + 0.001f * y + 0.1f * c + frame;
}

// Values in [0, 1), for formats that can't store more.
static float unitValue(int x, int y, int c) {
    return std::fmod(0.013f * x + 0.029f * y + 0.21f * c, 1.0f);
}

// Valid premultiplied RGBA in [0, 1] (colour never exceeds alpha). PNG stores
// colour unpremultiplied, so invalid premultiplied data wouldn't round-trip.
static float premultValue(int x, int y, int c) {
    const float a = 0.25f + 0.75f * unitValue(x, y, 3);
    return c < 3 ? unitValue(x, y, c) * a : a;
}

// Overscan-style test frame: data window larger than and offset from the display window.
static void makeTestFrame(const std::string& path, int frame) {
    test::ExrOptions o;
    o.dataWindow = {-4, -2, 20, 10};
    o.displayWindow = {0, 0, 16, 8};
    test::writeImage(path, o, [frame](int x, int y, int c) { return testValue(x, y, c, frame); });
}

static OIIO::ImageSpec fileSpec(const std::string& path) {
    auto in = OIIO::ImageInput::open(path);
    if (!in) return {};
    return in->spec();
}

// ---------------------------------------------------------------- frame patterns

static void testFramePattern() {
    CHECK(expandFramePattern("plate.####.exr", 42) == "plate.0042.exr");
    CHECK(expandFramePattern("plate.%04d.exr", 42) == "plate.0042.exr");
    CHECK(expandFramePattern("plate.%d.exr", 42) == "plate.42.exr");
    CHECK(expandFramePattern("plate.@@@.exr", 7) == "plate.007.exr");
    CHECK(expandFramePattern("plate.####.exr", 12345) == "plate.12345.exr");
    CHECK(expandFramePattern("plate.####.exr", -5) == "plate.-005.exr");
    CHECK(expandFramePattern("still.exr", 5) == "still.exr");
    CHECK(!hasFrameToken("still.exr"));
    CHECK(hasFrameToken("a.%d.exr"));
}

// ---------------------------------------------------------------- Read

static void testRoundTrip() {
    makeTestFrame(tmp("in.0001.exr"), 1);
    makeTestFrame(tmp("in.0002.exr"), 2);

    Graph g;
    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", tmp("in.####.exr"));
    read.setParam("first", 1);
    read.setParam("last", 2);

    auto& write = g.create<WriteNode>("Write1");
    write.setParam("file", tmp("out/out.####.exr"));
    write.setParam("datatype", std::string("float"));
    write.setParam("compression", std::string("zip"));
    g.connect(read, write, 0);

    for (int f = 1; f <= 2; ++f) write.execute(g_engine, {static_cast<double>(f)});

    // Read opts out of tiling: it opens the file once, however small the tiles.
    Engine tiny(EngineOptions{4, 2});
    tiny.render(read, {1.0}, {-4, -2, 20, 10});
    CHECK(tiny.lastStats().tilesComputed == 1);

    // Re-read what was written and compare every pixel.
    auto& check = g.create<ReadNode>("Check");
    check.setParam("file", tmp("out/out.####.exr"));
    for (int f = 1; f <= 2; ++f) {
        RenderContext ctx{static_cast<double>(f)};
        auto info = g_engine.info(check, ctx);
        CHECK((info.dataWindow == Box{-4, -2, 20, 10}));
        CHECK((info.displayWindow == Box{0, 0, 16, 8}));
        CHECK(info.channels.size() == 4 && info.channels[3] == "A");

        auto img = g_engine.render(check, ctx, info.dataWindow);
        float maxErr = 0;
        for (int y = -2; y < 10; ++y)
            for (int x = -4; x < 20; ++x)
                for (int c = 0; c < 4; ++c)
                    maxErr = std::max(maxErr, std::fabs(img.pixel(x, y)[c] - testValue(x, y, c, f)));
        CHECK(maxErr == 0.0f);
        CHECK(!fs::exists(expandFramePattern(tmp("out/out.####.exr.partial"), f)));
    }
}

static void testRegionsAndRange() {
    Graph g;
    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", tmp("in.####.exr"));
    read.setParam("first", 1);
    read.setParam("last", 2);

    // A region partly outside the data window: inside matches, outside is zero.
    Box region{15, 5, 30, 20};
    auto img = g_engine.render(read, {1.0}, region);
    CHECK(img.region() == region);
    CHECK(img.pixel(15, 5)[0] == testValue(15, 5, 0, 1));
    CHECK(img.pixel(19, 9)[2] == testValue(19, 9, 2, 1));
    CHECK(img.pixel(20, 9)[0] == 0.0f);
    CHECK(img.pixel(25, 15)[3] == 0.0f);

    // Fractional frames go to the nearest whole frame.
    CHECK(g_engine.render(read, {1.4}, {0, 0, 1, 1}).pixel(0, 0)[0] == testValue(0, 0, 0, 1));
    CHECK(g_engine.render(read, {1.6}, {0, 0, 1, 1}).pixel(0, 0)[0] == testValue(0, 0, 0, 2));

    // "hold": frame 10 shows frame 2, frame -3 shows frame 1.
    CHECK(g_engine.render(read, {10.0}, {0, 0, 1, 1}).pixel(0, 0)[0] == testValue(0, 0, 0, 2));
    CHECK(g_engine.render(read, {-3.0}, {0, 0, 1, 1}).pixel(0, 0)[0] == testValue(0, 0, 0, 1));

    // "black": empty data window, zero pixels, but the format and channels are kept.
    read.setParam("outside", std::string("black"));
    const ImageInfo info = g_engine.info(read, {10.0});
    CHECK(info.dataWindow.empty());
    CHECK((info.displayWindow == Box{0, 0, 16, 8}));
    CHECK(info.channels.size() == 4);
    CHECK(g_engine.render(read, {10.0}, {0, 0, 1, 1}).pixel(0, 0)[0] == 0.0f);

    read.setParam("outside", std::string("error"));
    CHECK_THROWS(g_engine.render(read, {10.0}, {0, 0, 1, 1}), "frame 10 is outside 1-2");

    read.setParam("outside", std::string("blak"));
    CHECK_THROWS(g_engine.info(read, {10.0}), "outside must be hold, black or error");
}

static void testTiledRead() {
    // Tiled files take a separate code path. Use tiles that don't divide the
    // image evenly and an offset data window.
    test::ExrOptions o;
    o.dataWindow = {-5, -3, 45, 37};
    o.displayWindow = {0, 0, 40, 34};
    o.tileSize = 16;
    test::writeImage(tmp("tiled.exr"), o, [](int x, int y, int c) { return testValue(x, y, c, 0); });
    CHECK(fileSpec(tmp("tiled.exr")).tile_width == 16);

    Graph g;
    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", tmp("tiled.exr"));

    for (Box region : {Box{3, 10, 20, 22}, Box{-5, -3, 45, 37}, Box{40, 30, 60, 50}, Box{-20, 36, 0, 40}}) {
        Image img = g_engine.render(read, {}, region);
        CHECK(img.region() == region);
        float maxErr = 0;
        for (int y = region.y0; y < region.y1; ++y)
            for (int x = region.x0; x < region.x1; ++x)
                for (int c = 0; c < 4; ++c) {
                    const float want = o.dataWindow.contains(x, y) ? testValue(x, y, c, 0) : 0.0f;
                    maxErr = std::max(maxErr, std::fabs(img.pixel(x, y)[c] - want));
                }
        CHECK(maxErr == 0.0f);
    }
}

static void testExtraChannels() {
    // Depth and render passes must survive Read -> Write, names and order intact.
    test::ExrOptions o;
    o.channels = {"R", "G", "B", "A", "Z", "diffuse.R", "diffuse.G", "diffuse.B"};
    test::writeImage(tmp("aovs.exr"), o, [](int x, int y, int c) { return testValue(x, y, c, 3); });

    Graph g;
    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", tmp("aovs.exr"));
    auto& write = g.create<WriteNode>("Write1");
    write.setParam("file", tmp("aovs_out.exr"));
    write.setParam("datatype", std::string("float"));
    g.connect(read, write, 0);
    write.execute(g_engine, {});

    auto& check = g.create<ReadNode>("Check");
    check.setParam("file", tmp("aovs_out.exr"));
    const ImageInfo info = g_engine.info(check, {});
    CHECK(info.channels == o.channels);
    Image img = g_engine.render(check, {}, info.dataWindow);
    CHECK(img.pixel(7, 9)[img.channelIndex("Z")] == testValue(7, 9, 4, 3));
    CHECK(img.pixel(7, 9)[img.channelIndex("diffuse.B")] == testValue(7, 9, 7, 3));
}

static void testReadErrors() {
    Graph g;
    auto& missing = g.create<ReadNode>("Missing");
    missing.setParam("file", tmp("nope.exr"));
    CHECK_THROWS(g_engine.info(missing, {}), "Missing: cannot open");

    auto& noFile = g.create<ReadNode>("NoFile");
    CHECK_THROWS(g_engine.info(noFile, {}), "missing parameter 'file'");

    std::ofstream(tmp("garbage.exr")) << "this is not an image";
    auto& garbage = g.create<ReadNode>("Garbage");
    garbage.setParam("file", tmp("garbage.exr"));
    CHECK_THROWS(g_engine.info(garbage, {}), "Garbage: cannot open");
}

// ---------------------------------------------------------------- Write

static void testDatatypes() {
    struct Case {
        const char* datatype;  // nullptr = leave the parameter unset
        const char* file;
        OIIO::TypeDesc expected;
        float tolerance;
    } cases[] = {
        {"float", "dt_float.exr", OIIO::TypeDesc::FLOAT, 0.0f},
        {"half", "dt_half.exr", OIIO::TypeDesc::HALF, 1e-3f},
        {nullptr, "dt_default.exr", OIIO::TypeDesc::HALF, 1e-3f},  // default is half
        {"uint16", "dt_uint16.tif", OIIO::TypeDesc::UINT16, 1.0f / 65535},
        {"uint8", "dt_uint8.png", OIIO::TypeDesc::UINT8, 2.0f / 255},  // unpremultiply + requantise
    };

    for (const Case& c : cases) {
        Graph g;
        auto& src = g.create<SourceNode>("Src", Box{0, 0, 32, 32}, ChannelList{"R", "G", "B", "A"}, premultValue);
        auto& write = g.create<WriteNode>("Write1");
        write.setParam("file", tmp(c.file));
        if (c.datatype) write.setParam("datatype", std::string(c.datatype));
        write.setParam("colorspace", std::string("Raw"));  // this checks quantisation, not color
        g.connect(src, write, 0);
        write.execute(g_engine, {});

        CHECK(fileSpec(tmp(c.file)).format == c.expected);

        auto& read = g.create<ReadNode>("Read1");
        read.setParam("file", tmp(c.file));
        read.setParam("colorspace", std::string("Raw"));
        Image img = g_engine.render(read, {}, {0, 0, 32, 32});
        float maxErr = 0;
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 32; ++x)
                for (int ch = 0; ch < 4; ++ch)
                    maxErr = std::max(maxErr, std::fabs(img.pixel(x, y)[ch] - premultValue(x, y, ch)));
        // Quantisation error is at most half a step; allow a full step (two for PNG).
        if (maxErr > c.tolerance) {
            std::fprintf(stderr, "datatype %s: max error %g > %g\n", c.file, maxErr, c.tolerance);
            ++test::failures;
        }
    }

    // Integer formats clamp to [0, 1]. (Raw: this checks clamping, not color.)
    Graph g;
    auto& bright = g.create<SourceNode>("Bright", Box{0, 0, 4, 4}, ChannelList{"R", "G", "B"},
                                        [](int, int, int c) { return c == 0 ? 1.5f : -0.5f; });
    auto& write = g.create<WriteNode>("Write1");
    write.setParam("file", tmp("clamp.png"));
    write.setParam("datatype", std::string("uint8"));
    write.setParam("colorspace", std::string("Raw"));
    g.connect(bright, write, 0);
    write.execute(g_engine, {});
    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", tmp("clamp.png"));
    read.setParam("colorspace", std::string("Raw"));
    Image img = g_engine.render(read, {}, {0, 0, 1, 1});
    CHECK(img.pixel(0, 0)[0] == 1.0f);
    CHECK(img.pixel(0, 0)[1] == 0.0f);

    write.setParam("datatype", std::string("int7"));
    CHECK_THROWS(write.execute(g_engine, {}), "unknown datatype 'int7'");
}

static void testEmptyImage() {
    // Nothing to write (e.g. a Read past its range set to black): a single
    // black pixel at the display window's origin, so the frame still exists.
    Graph g;
    auto& empty = g.create<SourceNode>("Empty", Box{}, ChannelList{"R", "G", "B", "A"},
                                       [](int, int, int) { return 1.0f; }, Box{0, 0, 64, 48});
    auto& write = g.create<WriteNode>("Write1");
    write.setParam("file", tmp("empty.exr"));
    g.connect(empty, write, 0);
    write.execute(g_engine, {});

    const OIIO::ImageSpec spec = fileSpec(tmp("empty.exr"));
    CHECK(spec.width == 1 && spec.height == 1);
    CHECK(spec.full_width == 64 && spec.full_height == 48);
    CHECK(empty.computeCalls == 0);  // nothing was asked of the source

    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", tmp("empty.exr"));
    CHECK(g_engine.render(read, {}, {0, 0, 1, 1}).pixel(0, 0)[0] == 0.0f);
}

static void testWriteErrors() {
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{-2, -2, 10, 10}, ChannelList{"R", "G", "B"},
                                     [](int, int, int) { return 0.5f; });
    auto& write = g.create<WriteNode>("Write1");
    g.connect(src, write, 0);

    // PNG has no data windows, so an offset one can't be stored.
    write.setParam("file", tmp("offset.png"));
    CHECK_THROWS(write.execute(g_engine, {}), "cannot store an offset data window");
    CHECK(!fs::exists(tmp("offset.png")) && !fs::exists(tmp("offset.png.partial")));

    write.setParam("file", tmp("file.notaformat"));
    CHECK_THROWS(write.execute(g_engine, {}), "no writer");

    // The output folder can't be created because a file is in the way.
    std::ofstream(tmp("blocker")) << "x";
    write.setParam("file", tmp("blocker/out.exr"));
    CHECK_THROWS(write.execute(g_engine, {}), "cannot create directory");

    // Opening the temporary file fails: a non-empty folder sits at its path.
    fs::create_directories(tmp("busy.exr.partial"));
    std::ofstream(tmp("busy.exr.partial/keep")) << "x";
    write.setParam("file", tmp("busy.exr"));
    CHECK_THROWS(write.execute(g_engine, {}), "failed writing");
    CHECK(!fs::exists(tmp("busy.exr")));
    CHECK(fs::exists(tmp("busy.exr.partial/keep")));  // other people's files are left alone

    // Writing works but the final rename fails: a non-empty folder has the
    // output's name. The temporary file must not be left behind.
    fs::create_directories(tmp("taken.exr"));
    std::ofstream(tmp("taken.exr/keep")) << "x";
    write.setParam("file", tmp("taken.exr"));
    CHECK_THROWS(write.execute(g_engine, {}), "cannot move finished file into place");
    CHECK(!fs::exists(tmp("taken.exr.partial")));

    // Unconnected input is reported by the Write node's name.
    auto& lonely = g.create<WriteNode>("Lonely");
    lonely.setParam("file", tmp("x.exr"));
    CHECK_THROWS(lonely.execute(g_engine, {}), "Lonely: input 0 is not connected");
}

static void testWriteIsPassThrough() {
    // A Write in the middle of a graph passes its input through unchanged.
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 8, 8}, ChannelList{"R"},
                                     [](int x, int y, int) { return float(x + 10 * y); });
    auto& write = g.create<WriteNode>("Write1");
    g.connect(src, write, 0);
    CHECK(g_engine.info(write, {}).dataWindow == (Box{0, 0, 8, 8}));
    Image img = g_engine.render(write, {}, {-1, -1, 9, 9});
    CHECK(img.pixel(3, 5)[0] == 53.0f);
    CHECK(img.pixel(-1, -1)[0] == 0.0f);
}

static void testCacheSeesFileChanges() {
    // Overwriting a file invalidates its cached pixels, even at the same path.
    const std::string path = tmp("cached.exr");
    makeTestFrame(path, 1);
    Graph g;
    auto& read = g.create<ReadNode>("Read1");
    read.setParam("file", path);

    Engine e(EngineOptions{.cacheBytes = 64 << 20});
    const Box region{-4, -2, 20, 10};
    CHECK_NEAR(e.render(read, {}, region).pixel(3, 2)[1], testValue(3, 2, 1, 1));
    e.render(read, {}, region);
    CHECK(e.lastStats().tilesFromCache == 1 && e.lastStats().tilesComputed == 0);

    makeTestFrame(path, 7);
    // Make sure the timestamp moves even on filesystems with coarse mtimes.
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    CHECK_NEAR(e.render(read, {}, region).pixel(3, 2)[1], testValue(3, 2, 1, 7));
    CHECK(e.lastStats().tilesComputed == 1);
}

// ---------------------------------------------------------------- color

// Opaque colors inside the sRGB gamut, in the working space (ACEScg): made
// in linear Rec.709 and converted, so they survive an sRGB file unclipped.
static Image inGamutImage(const Box& box) {
    Image img(box, {"R", "G", "B", "A"});
    for (int y = box.y0; y < box.y1; ++y)
        for (int x = box.x0; x < box.x1; ++x) {
            float* p = img.pixel(x, y);
            p[0] = 0.02f + 0.03f * (x - box.x0);
            p[1] = 0.05f + 0.04f * (y - box.y0);
            p[2] = 0.5f;
            p[3] = 1.0f;
        }
    ColorConfig::builtin()->convert(img, "Linear Rec.709 (sRGB)", "ACEScg");
    return img;
}

static float maxDiff(const Image& a, const Image& b) {
    float m = 0;
    for (int y = a.region().y0; y < a.region().y1; ++y)
        for (int x = a.region().x0; x < a.region().x1; ++x)
            for (int c = 0; c < a.numChannels(); ++c) m = std::max(m, std::fabs(a.pixel(x, y)[c] - b.at(x, y, c)));
    return m;
}

static void testColorSpaces() {
    const Box box{0, 0, 8, 8};
    const Image working = inGamutImage(box);
    Image asSrgb = working;
    ColorConfig::builtin()->convert(asSrgb, "ACEScg", "sRGB - Texture");

    Graph g;
    auto& src = g.create<SourceNode>("Src", box, ChannelList{"R", "G", "B", "A"},
                                     [&](int x, int y, int c) { return working.pixel(x, y)[c]; });
    auto& write = g.create<WriteNode>("Write1");
    g.connect(src, write, 0);
    auto& read = g.create<ReadNode>("Read1");
    auto& raw = g.create<ReadNode>("Raw1");
    raw.setParam("colorspace", std::string("Raw"));

    auto useFile = [&](const std::string& file) {
        write.setParam("file", tmp(file));
        read.setParam("file", tmp(file));
        raw.setParam("file", tmp(file));
        write.execute(g_engine, {});
    };

    // Integer files are sRGB by default, both ways: the file holds sRGB
    // values, and reading it gives back the working-space pixels.
    write.setParam("datatype", std::string("uint16"));
    useFile("color16.tif");
    CHECK(maxDiff(g_engine.render(raw, {}, box), asSrgb) < 2.0f / 65535);
    CHECK(maxDiff(g_engine.render(read, {}, box), working) < 1e-3f);

    // "auto" goes by what the format stores: PNG asked for half writes
    // integers, so it's sRGB too.
    write.setParam("datatype", std::string("half"));
    useFile("color_half.png");
    CHECK(maxDiff(g_engine.render(raw, {}, box), asSrgb) < 1.0f / 255);
    CHECK(maxDiff(g_engine.render(read, {}, box), working) < 0.02f);

    // Float files are already in the working space: stored as is.
    useFile("color.exr");
    CHECK(maxDiff(g_engine.render(raw, {}, box), working) < 1e-3f);

    // An explicit color space: write ACEScct, read it back as ACEScct.
    write.setParam("colorspace", std::string("ACEScct"));
    read.setParam("colorspace", std::string("ACEScct"));
    useFile("color_cct.exr");
    Image asCct = working;
    ColorConfig::builtin()->convert(asCct, "ACEScg", "ACEScct");
    CHECK(maxDiff(g_engine.render(raw, {}, box), asCct) < 1e-3f);
    CHECK(maxDiff(g_engine.render(read, {}, box), working) < 2e-3f);

    // Converting only inside the data window: ACEScct maps 0 to a negative
    // value, but the padding around the image stays 0.
    const Image padded = g_engine.render(read, {}, {-4, -4, 12, 12});
    CHECK(padded.pixel(-2, -2)[0] == 0.0f && padded.pixel(10, 3)[1] == 0.0f);

    // Unknown color spaces fail, naming the node and listing what exists.
    read.setParam("colorspace", std::string("Rec.709ish"));
    CHECK_THROWS(g_engine.render(read, {}, box), "Read1: unknown color space 'Rec.709ish'");
    CHECK_THROWS(g_engine.render(read, {}, box), "ACEScg");
    write.setParam("colorspace", std::string("nope"));
    CHECK_THROWS(write.execute(g_engine, {}), "Write1: unknown color space 'nope'");
    CHECK(!fs::exists(tmp("color_cct.exr.partial")));
}

// A Write run from a snapshot writes the graph as it was when the snapshot
// was taken, its own parameters (the file name) included.
static void testWriteFromSnapshot() {
    Graph g;
    auto& src = g.create<SourceNode>("Src", Box{0, 0, 8, 4}, ChannelList{"R", "G", "B", "A"},
                                     [](int, int, int c) { return c == 3 ? 1.0f : 0.25f; });
    auto& grade = g.add(createNode("Grade", "Grade1"));
    auto& write = g.create<WriteNode>("Write1");
    g.connect(src, grade, 0);
    g.connect(grade, write, 0);
    grade.setParam("multiply", 2.0);
    write.setParam("file", tmp("snap/before.exr"));
    write.setParam("datatype", std::string("float"));

    const Snapshot snap = Snapshot::of(write);
    grade.setParam("multiply", 3.0);  // edits after the snapshot...
    write.setParam("file", tmp("snap/after.exr"));
    g.disconnect(grade, 0);
    CHECK(write.execute(g_engine, snap, {}) == tmp("snap/before.exr"));  // ...don't show
    CHECK(!fs::exists(tmp("snap/after.exr")));
    auto& check = g.create<ReadNode>("Check");
    check.setParam("file", tmp("snap/before.exr"));
    CHECK(g_engine.render(check, {}, {0, 0, 8, 4}).pixel(3, 2)[0] == 0.5f);

    CHECK_THROWS(write.execute(g_engine, Snapshot::of(check), {}), "Write1: the snapshot is of another node");
    // upstream() is the part feeding an input.
    const Snapshot up = snap.upstream(0);
    CHECK(up.output().node == &grade && up.entries().size() == 2 && up.entries()[0].node == &src);
    CHECK(up.output().inputs == std::vector<int>{0});
    CHECK_THROWS(up.upstream(0).upstream(0), "Src: input 0 is not connected");
}

int main() {
    g_dir = fs::current_path() / "test_read_write_tmp";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    try {
        testFramePattern();
        testRoundTrip();
        testRegionsAndRange();
        testTiledRead();
        testExtraChannels();
        testReadErrors();
        testDatatypes();
        testEmptyImage();
        testWriteErrors();
        testWriteIsPassThrough();
        testCacheSeesFileChanges();
        testColorSpaces();
        testWriteFromSnapshot();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }

    if (test::failures == 0) fs::remove_all(g_dir);
    else std::fprintf(stderr, "test files kept in %s\n", g_dir.c_str());
    return test::finish("read/write");
}
