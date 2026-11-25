#include "comp/nodes/PremultNode.h"

namespace comp {

Image PremultNode::compute(const RenderContext&, const Box& region, std::span<const Image* const> inputs) const {
    Image out = cropped(*inputs[0], region);
    const int a = out.channelIndex("A");
    if (a < 0) return out;
    int rgb[3];
    int count = 0;
    for (const char* c : {"R", "G", "B"})
        if (int i = out.channelIndex(c); i >= 0) rgb[count++] = i;

    const int n = out.numChannels();
    const bool div = divide();
    float* px = out.data();
    for (long long i = 0, end = region.area(); i < end; ++i, px += n) {
        const float alpha = px[a];
        if (div) {
            if (alpha == 0.0f) continue;
            for (int k = 0; k < count; ++k) px[rgb[k]] /= alpha;
        } else {
            for (int k = 0; k < count; ++k) px[rgb[k]] *= alpha;
        }
    }
    return out;
}

}  // namespace comp
