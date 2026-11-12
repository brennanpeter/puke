#include "comp/nodes/GradeNode.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace comp {

Image GradeNode::compute(const RenderContext&, const Box& region,
                         std::span<const Image* const> inputs) const {
    Image out = cropped(*inputs[0], region);

    const Color blackpoint = colorParam("blackpoint", 0.0);
    const Color whitepoint = colorParam("whitepoint", 1.0);
    const Color lift = colorParam("lift", 0.0);
    const Color gain = colorParam("gain", 1.0);
    const Color multiply = colorParam("multiply", 1.0);
    const Color offset = colorParam("offset", 0.0);
    const Color gamma = colorParam("gamma", 1.0);
    const bool blackClamp = param<bool>("black_clamp", true);
    const bool whiteClamp = param<bool>("white_clamp", false);
    const float mix = static_cast<float>(numberParam("mix", 1.0));

    const std::string channels = param<std::string>("channels", "rgb");
    if (channels != "rgb" && channels != "rgba")
        throw Error(name() + ": channels must be \"rgb\" or \"rgba\", not \"" + channels + "\"");
    const char* names[] = {"R", "G", "B", "A"};
    const int numColor = channels == "rgba" ? 4 : 3;

    // Precompute A, B and 1/gamma for each graded channel present in the image.
    struct Op {
        int index;
        float a, b, invGamma;
    };
    std::vector<Op> ops;
    for (int k = 0; k < numColor; ++k) {
        const int idx = out.channelIndex(names[k]);
        if (idx < 0) continue;
        if (gamma[k] <= 0) throw Error(name() + ": gamma must be greater than 0");
        double range = whitepoint[k] - blackpoint[k];
        if (std::fabs(range) < 1e-9) range = range < 0 ? -1e-9 : 1e-9;  // behaves like a hard step
        const double A = multiply[k] * (gain[k] - lift[k]) / range;
        const double B = offset[k] + lift[k] - A * blackpoint[k];
        // An identity grade leaves the channel alone entirely, so adding a
        // default Grade never clamps negative values by surprise.
        if (A == 1.0 && B == 0.0 && gamma[k] == 1.0) continue;
        ops.push_back({idx, float(A), float(B), float(1.0 / gamma[k])});
    }
    if (ops.empty()) return out;

    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += out.numChannels()) {
            for (const Op& op : ops) {
                const float in = px[op.index];
                float v = op.a * in + op.b;
                if (op.invGamma != 1.0f && v > 0) v = std::pow(v, op.invGamma);
                if (blackClamp) v = std::max(v, 0.0f);
                if (whiteClamp) v = std::min(v, 1.0f);
                px[op.index] = in + (v - in) * mix;
            }
        }
    }
    return out;
}

}  // namespace comp
