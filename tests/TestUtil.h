// Shared helpers for the puke test programs. Deliberately tiny: no framework,
// just checks that count failures and keep going.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "puke/graph/Node.h"

namespace test {

inline int failures = 0;

// Returns the process exit code and prints a summary.
inline int finish(const char* suite) {
    if (failures == 0) {
        std::printf("all %s tests passed\n", suite);
        return 0;
    }
    std::fprintf(stderr, "%s: %d check(s) failed\n", suite, failures);
    return 1;
}

// Runs `f` and returns the puke::Error message it threw, or "" if none.
template <typename F>
std::string errorFrom(F&& f) {
    try {
        f();
    } catch (const puke::Error& e) {
        return e.what();
    }
    return {};
}

}  // namespace test

#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++test::failures;                                                         \
        }                                                                             \
    } while (0)

#define CHECK_NEAR(a, b)                                                              \
    do {                                                                              \
        const double va_ = (a), vb_ = (b);                                            \
        if (!(std::fabs(va_ - vb_) < 1e-4)) {                                         \
            std::fprintf(stderr, "%s:%d: CHECK_NEAR failed: %s = %g, %s = %g\n",      \
                         __FILE__, __LINE__, #a, va_, #b, vb_);                       \
            ++test::failures;                                                         \
        }                                                                             \
    } while (0)

// Checks that `expr` throws puke::Error whose message contains `substr`.
#define CHECK_THROWS(expr, substr)                                                    \
    do {                                                                              \
        const std::string msg_ = test::errorFrom([&] { (void)(expr); });              \
        if (msg_.find(substr) == std::string::npos) {                                 \
            std::fprintf(stderr, "%s:%d: CHECK_THROWS failed: %s\n  expected error containing \"%s\", got \"%s\"\n", \
                         __FILE__, __LINE__, #expr, std::string(substr).c_str(), msg_.c_str()); \
            ++test::failures;                                                         \
        }                                                                             \
    } while (0)

namespace test {

using PixelFn = std::function<float(int x, int y, int c)>;

// A node whose pixels come from a function, inside `box` (zero outside).
class SourceNode : public puke::Node {
public:
    SourceNode(std::string name, puke::Box box, puke::ChannelList channels, PixelFn fn,
               puke::Box format = {0, 0, 32, 32})
        : Node(std::move(name)), box_(box), format_(format), channels_(std::move(channels)), fn_(std::move(fn)) {}

    const char* typeName() const override { return "Source"; }
    puke::ImageInfo computeInfo(const puke::RenderContext&,
                                std::span<const puke::ImageInfo* const>) const override {
        return {box_, format_, channels_};
    }
    puke::Image compute(const puke::RenderContext&, const puke::Box& region,
                        std::span<const puke::Image* const>) const override {
        {
            std::lock_guard lock(probeMutex_);  // the engine calls this from several threads
            requested = requested.unite(region);
            ++computeCalls;
        }
        puke::Image out(region, channels_);
        for (int y = region.y0; y < region.y1; ++y)
            for (int x = region.x0; x < region.x1; ++x)
                for (int c = 0; c < out.numChannels(); ++c) out.pixel(x, y)[c] = fn_(x, y, c);
        return out;
    }

    // Test instrumentation only; real nodes must not mutate during compute().
    // `requested` is the bounding box of every region asked for.
    mutable puke::Box requested;
    mutable int computeCalls = 0;
    void resetProbe() const {
        std::lock_guard lock(probeMutex_);
        requested = {};
        computeCalls = 0;
    }

private:
    mutable std::mutex probeMutex_;
    puke::Box box_, format_;
    puke::ChannelList channels_;
    PixelFn fn_;
};

struct ExrOptions {
    puke::Box dataWindow{0, 0, 32, 32};
    puke::Box displayWindow{0, 0, 32, 32};
    puke::ChannelList channels{"R", "G", "B", "A"};
    OIIO::TypeDesc type = OIIO::TypeDesc::FLOAT;
    int tileSize = 0;  // 0 = scanline file
};

// Writes an image file with OIIO directly, independent of the code under test.
inline void writeImage(const std::string& path, const ExrOptions& o, const PixelFn& fn) {
    const puke::Box& d = o.dataWindow;
    const int nch = static_cast<int>(o.channels.size());
    OIIO::ImageSpec spec(d.width(), d.height(), nch, o.type);
    spec.x = d.x0;
    spec.y = d.y0;
    spec.full_x = o.displayWindow.x0;
    spec.full_y = o.displayWindow.y0;
    spec.full_width = o.displayWindow.width();
    spec.full_height = o.displayWindow.height();
    spec.channelnames.assign(o.channels.begin(), o.channels.end());
    spec.alpha_channel = -1;
    for (int c = 0; c < nch; ++c)
        if (o.channels[c] == "A") spec.alpha_channel = c;
    if (o.tileSize > 0) spec.tile_width = spec.tile_height = o.tileSize;

    std::vector<float> px(static_cast<std::size_t>(d.area()) * nch);
    for (int y = d.y0; y < d.y1; ++y)
        for (int x = d.x0; x < d.x1; ++x)
            for (int c = 0; c < nch; ++c)
                px[(static_cast<std::size_t>(y - d.y0) * d.width() + (x - d.x0)) * nch + c] = fn(x, y, c);

    auto out = OIIO::ImageOutput::create(path);
    if (!out || !out->open(path, spec) || !out->write_image(OIIO::TypeDesc::FLOAT, px.data()) ||
        !out->close()) {
        std::fprintf(stderr, "test setup: could not write %s\n", path.c_str());
        std::exit(1);
    }
}

}  // namespace test
