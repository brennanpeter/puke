// Runs the real comp_render executable and checks exit codes, messages and output files.
// COMP_RENDER_PATH is set by CMake to the built binary.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <OpenImageIO/imageio.h>

#include "TestUtil.h"

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
    const std::string cmd = "cd \"" + g_dir.string() + "\" && \"" COMP_RENDER_PATH "\" " + args +
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

    r = run("plate.####.exr out.####.exr --bogus");
    CHECK(r.exitCode == 2 && r.output.find("usage:") != std::string::npos);

    r = run("plate.####.exr out.####.exr --frames");  // flag missing its value
    CHECK(r.exitCode == 2);

    r = run("plate.####.exr out.####.exr --frames 1005-1001");
    CHECK(r.exitCode == 2 && r.output.find("invalid frame range") != std::string::npos);

    r = run("plate.####.exr out.####.exr --frames abc");
    CHECK(r.exitCode == 2 && r.output.find("invalid frame range") != std::string::npos);

    r = run("plate.####.exr single.exr --frames 1001-1003");
    CHECK(r.exitCode == 2 && r.output.find("needs a frame token") != std::string::npos);
    CHECK(!fs::exists(g_dir / "single.exr"));
}

static void testRenderSequence() {
    auto r = run("plate.####.exr out/shot.####.exr --frames 1001-1003 --datatype float --compression zip");
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
    // 8-bit, and PNG stores colour unpremultiplied: allow two quantisation steps.
    CHECK(std::fabs(pixel(g_dir / "held.0002.png", 0, 0, 1) - 0.25f) < 2.0f / 255);
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

int main() {
    g_dir = fs::current_path() / "test_cli_tmp";
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);

    try {
        makeSequence();
        testUsageErrors();
        testRenderSequence();
        testRenderFailures();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "unexpected exception: %s\n", e.what());
        ++test::failures;
    }

    if (test::failures == 0) fs::remove_all(g_dir);
    else std::fprintf(stderr, "test files kept in %s\n", g_dir.c_str());
    return test::finish("command-line");
}
