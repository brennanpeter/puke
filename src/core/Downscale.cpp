#include <cmath>
#include <vector>

#include "puke/core/Image.h"

namespace puke {

namespace {

// For output pixel i, the source pixels it covers and how much of each,
// normalized to sum to 1.
struct Span {
    int first;
    std::vector<float> w;
};

std::vector<Span> spans(int out0, int out1, double scale) {
    std::vector<Span> out;
    const double step = 1 / scale;
    for (int i = out0; i < out1; ++i) {
        const double a = i * step, b = (i + 1) * step;
        Span s{static_cast<int>(std::floor(a)), {}};
        for (int x = s.first; x < b; ++x)
            s.w.push_back(static_cast<float>((std::min(b, x + 1.0) - std::max(a, double(x))) * scale));
        out.push_back(std::move(s));
    }
    return out;
}

}  // namespace

Image downscale(const Image& src, const Box& region, double scale) {
    if (scale == 1) return cropped(src, region);
    const int n = src.numChannels();
    const auto xs = spans(region.x0, region.x1, scale), ys = spans(region.y0, region.y1, scale);
    // Across first, into the source rows the output rows need.
    const int r0 = ys.front().first, r1 = ys.back().first + static_cast<int>(ys.back().w.size());
    Image across(Box{region.x0, r0, region.x1, r1}, src.channels());
    for (int y = r0; y < r1; ++y) {
        float* px = across.pixel(region.x0, y);
        for (const Span& s : xs) {
            for (std::size_t k = 0; k < s.w.size(); ++k)
                for (int c = 0; c < n; ++c) px[c] += s.w[k] * src.at(s.first + static_cast<int>(k), y, c);
            px += n;
        }
    }
    Image out(region, src.channels());
    for (int i = 0; i < region.height(); ++i) {
        const Span& s = ys[i];
        float* px = out.pixel(region.x0, region.y0 + i);
        for (std::size_t k = 0; k < s.w.size(); ++k) {
            const float* in = across.pixel(region.x0, s.first + static_cast<int>(k));
            for (int j = 0; j < region.width() * n; ++j) px[j] += s.w[k] * in[j];
        }
    }
    return out;
}

}  // namespace puke
