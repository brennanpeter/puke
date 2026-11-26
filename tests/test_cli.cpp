// Runs the real puke_render executable and checks exit codes, messages and output files.
// PUKE_RENDER_PATH is set by CMake to the built binary.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <OpenImageIO/imageio.h>

#include "TestUtil.h"
#include "puke/graph/Graph.h"
#include "puke/engine/Engine.h"
#include "puke/nodes/Registry.h"
#include "puke/color/ColorConfig.h"

#if defined(_WIN32)
#define EXIT_CODE(status) (status)
#else
#include <sys/wait.h>
#define EXIT_CODE(status) (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
#endif

namespace fs = std::filesystem;
namespace OIIO = OIIO_NAMESPACE;

static fs::path g_dir;

struct Result {
    int exitCode;
    std::string output;  // stdout + stderr
};

static Result run(const std::string& args) {
    const fs::path log = g_dir / "log.txt";
    const std::string cmd = "cd \"" + g_dir.string() + "\" && \"" PUKE_RENDER_PATH "\" " + args +
                            " > \"" + log.string() + "\" 2>&1";
    const int status = std::system(cmd.c_str());
    std::ifstream in(log);
    std::stringstream ss;
    ss << in.rdbuf();
    return {EXIT_CODE(status), ss.str()};
}

static float frameValue(int x, int y, int c, int frame) { return 0.001f * x + 0.01f * y + 0.1f * c + frame; }

static void makeSequence() {
    for (int f = 1001; f <= 1003; ++f) {
        test::ExrOptions o;
        o.dataWindow = o.displayWindow = {0, 0, 16, 8};
        test::writeImage((g_dir / ("plate." + std::to_string(f) + ".exr")).string(), o,
                         [f](int x, int y, int c) { return frameValue(x, y, c, f); });
    }
    test::ExrOptions o;
    o.dataWindow = o.displayWindow = {0, 0, 16, 8};
    test::writeImage((g_dir / "still.exr").string(), o, [](int, int, int c) { return 0.25f * c; });
}

static float pixel(const fs::path& file, int x, int y, int c) {
    auto in = OIIO::ImageInput::open(file.string());
    if (!in) return -1;
    const auto& s = in->spec();
    std::vector<float> px(static_cast<std::size_t>(s.width) * s.height * s.nchannels);
    in->read_image(0, 0, 0, s.nchannels, OIIO::TypeDesc::FLOAT, px.data());
    return px[(static_cast<std::size_t>(y - s.y) * s.width + (x - s.x)) * s.nchannels + c];
}

static void testUsageErrors() {
    auto r = run("");
    CHECK(r.exitCode == 2 && r.output.find("usage:") != std::string::npos);

    r = run("a b c");  // too many paths
    CHECK(r.exitCode == 2 && r.output.find("usage:") != std::string::npos);

    r = run("in.exr out.exr --write Write1");  // --write is for projects
    CHECK(r.exitCode == 2 && r.output.find("usage:") != std::string::npos);

    r = run("plate.####.exr out.####.exr --bogus");
    CHECK(r.exitCode == 2 && r.output.find("usage:") != std::string::npos);

    r = run("plate.####.exr out.####.exr --frames");  // flag missing its value
    CHECK(r.exitCode == 2);

    r = run("plate.####.exr out.####.exr --frames 1005-1001");
    CHECK(r.exitCode == 2 && r.output.find("invalid frame range") != std::string::npos);

    r = run("plate.####.exr out.####.exr --frames abc");
    CHECK(r.exitCode == 2 && r.output.find("invalid frame range") != std::string::npos);

    r = run("plate.####.exr out.####.exr --threads -1");
    CHECK(r.exitCode == 2 && r.output.find("invalid thread count") != std::string::npos);

    r = run("plate.####.exr out.####.exr --threads 4x");
    CHECK(r.exitCode == 2 && r.output.find("invalid thread count") != std::string::npos);

    r = run("plate.####.exr single.exr --frames 1001-1003");
    CHECK(r.exitCode == 2 && r.output.find("needs a frame token") != std::string::npos);
    CHECK(!fs::exists(g_dir / "single.exr"));
}

static void testRenderSequence() {
    auto r = run("plate.####.exr out/shot.####.exr --frames 1001-1003 --datatype float --compression zip "
                 "--threads 2");
    CHECK(r.exitCode == 0);
    for (int f = 1001; f <= 1003; ++f) {
        const fs::path file = g_dir / ("out/shot." + std::to_string(f) + ".exr");
        CHECK(fs::exists(file));
        CHECK(pixel(file, 5, 3, 2) == frameValue(5, 3, 2, f));
        CHECK(r.output.find("frame " + std::to_string(f) + " -> out/shot." + std::to_string(f) + ".exr") !=
              std::string::npos);
    }

    // A single frame, using the default frames and datatype (half).
    r = run("still.exr still_out.exr");
    CHECK(r.exitCode == 0);
    auto in = OIIO::ImageInput::open((g_dir / "still_out.exr").string());
    CHECK(in && in->spec().format == OIIO::TypeDesc::HALF);

    // A still image held across a frame range.
    r = run("still.exr held.%04d.png --frames 1-2 --datatype uint8");
    CHECK(r.exitCode == 0);
    CHECK(fs::exists(g_dir / "held.0001.png") && fs::exists(g_dir / "held.0002.png"));
    // 8-bit files are written as sRGB. PNG stores colour unpremultiplied, so
    // out-of-gamut values clip before alpha is applied again on reading:
    // expect clamp(sRGB colour / alpha) * alpha, within two quantisation steps.
    puke::Image expected(puke::Box{0, 0, 1, 1}, {"R", "G", "B", "A"});
    for (int c = 0; c < 4; ++c) expected.pixel(0, 0)[c] = 0.25f * c;  // still.exr
    puke::ColorConfig::builtin()->convert(expected, "scene_linear", "sRGB - Texture");
    for (int c = 0; c < 3; ++c) {
        const float stored = std::clamp(expected.pixel(0, 0)[c] / 0.75f, 0.0f, 1.0f) * 0.75f;
        CHECK(std::fabs(pixel(g_dir / "held.0002.png", 0, 0, c) - stored) < 2.0f / 255);
    }
}

static void testRenderFailures() {
    // Missing frames fail individually; the run continues and exits 1.
    fs::remove(g_dir / "plate.1002.exr");
    auto r = run("plate.####.exr gap/out.####.exr --frames 1001-1003");
    CHECK(r.exitCode == 1);
    CHECK(r.output.find("frame 1002 failed: Read1: cannot open") != std::string::npos);
    CHECK(fs::exists(g_dir / "gap/out.1001.exr"));
    CHECK(!fs::exists(g_dir / "gap/out.1002.exr"));
    CHECK(fs::exists(g_dir / "gap/out.1003.exr"));

    r = run("still.exr bad.exr --datatype int3");
    CHECK(r.exitCode == 1 && r.output.find("unknown datatype") != std::string::npos);
}

static void writeText(const std::string& name, const std::string& text) {
    std::ofstream(g_dir / name) << text;
}

static void testProjects() {
    // Read -> Grade (x2) -> two Writes.
    writeText("shot.puke", R"({
  "format": "puke",
  "version": 1,
  "nodes": [
    {"name": "Plate", "type": "Read", "params": {"file": "plate.####.exr", "first": 1001, "last": 1003}},
    {"name": "Grade1", "type": "Grade", "inputs": ["Plate"], "params": {"multiply": 2.0, "channels": "rgba"}},
    {"name": "Main", "type": "Write", "inputs": ["Grade1"], "params": {"file": "proj/main.####.exr", "datatype": "float"}},
    {"name": "Proxy", "type": "Write", "inputs": ["Plate"], "params": {"file": "proj/proxy.####.exr"}}
  ]
})");
    auto r = run("shot.puke --frames 1001-1002 --threads 2");
    CHECK(r.exitCode == 0);
    for (int f = 1001; f <= 1002; ++f) {
        const std::string n = std::to_string(f);
        CHECK(r.output.find("frame " + n + " Main -> proj/main." + n + ".exr") != std::string::npos);
        CHECK(r.output.find("frame " + n + " Proxy -> proj/proxy." + n + ".exr") != std::string::npos);
        CHECK(std::fabs(pixel(g_dir / ("proj/main." + n + ".exr"), 5, 3, 2) - 2 * frameValue(5, 3, 2, f)) < 1e-3);
    }
    CHECK(!fs::exists(g_dir / "proj/main.1003.exr"));

    // --write picks which Write nodes run.
    r = run("shot.puke --frames 1003 --write Proxy");
    CHECK(r.exitCode == 0);
    CHECK(fs::exists(g_dir / "proj/proxy.1003.exr") && !fs::exists(g_dir / "proj/main.1003.exr"));

    // Without --frames: the project's own range.
    writeText("ranged.puke", R"({"format": "puke", "version": 1, "settings": {"first_frame": 1002, "last_frame": 1003},
  "nodes": [
    {"name": "Plate", "type": "Read", "params": {"file": "plate.####.exr", "first": 1001, "last": 1003}},
    {"name": "W", "type": "Write", "inputs": ["Plate"], "params": {"file": "ranged/out.####.exr"}}]})");
    r = run("ranged.puke");
    CHECK(r.exitCode == 0);
    CHECK(!fs::exists(g_dir / "ranged/out.1001.exr") && fs::exists(g_dir / "ranged/out.1002.exr") &&
          fs::exists(g_dir / "ranged/out.1003.exr"));

    // Generators make the project's format.
    writeText("gen.puke", R"({"format": "puke", "version": 1, "settings": {"format": {"width": 320, "height": 180}},
  "nodes": [
    {"name": "C", "type": "Constant", "params": {"color": [0.5, 0.5, 0.5, 1]}},
    {"name": "W", "type": "Write", "inputs": ["C"], "params": {"file": "gen/c.exr"}}]})");
    r = run("gen.puke");
    CHECK(r.exitCode == 0);
    {
        puke::Graph check;
        puke::Node& rd = check.add(puke::createNode("Read", "R"));
        rd.setParam("file", (g_dir / "gen/c.exr").string());
        CHECK((puke::Engine().info(rd, {}).displayWindow == puke::Box{0, 0, 320, 180}));
    }

    r = run("shot.puke --write Nope");
    CHECK(r.exitCode == 2 && r.output.find("shot.puke: no node named 'Nope'") != std::string::npos);
    r = run("shot.puke --write Grade1");
    CHECK(r.exitCode == 2 && r.output.find("Grade1 is a Grade node, not a Write") != std::string::npos);
    r = run("shot.puke --datatype float");
    CHECK(r.exitCode == 2 && r.output.find("set them on the Write nodes") != std::string::npos);

    writeText("nowrite.puke", R"({"format": "puke", "version": 1, "nodes": [{"name": "R", "type": "Read"}]})");
    r = run("nowrite.puke");
    CHECK(r.exitCode == 2 && r.output.find("the project has no Write nodes") != std::string::npos);

    writeText("still.puke", R"({"format": "puke", "version": 1, "nodes": [
        {"name": "R", "type": "Read", "params": {"file": "still.exr"}},
        {"name": "W", "type": "Write", "inputs": ["R"], "params": {"file": "still_proj.exr"}}]})");
    r = run("still.puke --frames 1-2");
    CHECK(r.exitCode == 2 && r.output.find("W: output needs a frame token") != std::string::npos);

    writeText("broken.puke", R"({"format": "puke", "version": 1, "nodes": [{"name": "B", "type": "Defocus"}]})");
    r = run("broken.puke");
    CHECK(r.exitCode == 2 && r.output.find("broken.puke: node 1 (B): unknown node type 'Defocus'") != std::string::npos);
    r = run("missing.puke");
    CHECK(r.exitCode == 2 && r.output.find("cannot open project 'missing.puke'") != std::string::npos);
}

int main() {
    g_dir = fs::current_path() / "test_cli_tmp";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    try {
        makeSequence();
        testUsageErrors();
        testRenderSequence();
        testProjects();
        testRenderFailures();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }

    if (test::failures == 0) fs::remove_all(g_dir);
    else std::fprintf(stderr, "test files kept in %s\n", g_dir.c_str());
    return test::finish("command-line");
}
