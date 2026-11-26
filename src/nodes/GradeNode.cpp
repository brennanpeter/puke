#include "puke/nodes/GradeNode.h"

#include "Specs.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace puke {

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

std::vector<ParamSpec> GradeNode::paramSpecs() const {
    using namespace specs;
    auto color = [](const char* name, double v, const char* hint, double min, double max) {
        return make(name, Kind::Color, Color{v, v, v, v}, hint, min, max);
    };
    return {
        color("blackpoint", 0, "Input value that maps to lift", -1, 1),
        color("whitepoint", 1, "Input value that maps to gain", 0, 4),
        color("lift", 0, "Output for the blackpoint", -1, 1),
        color("gain", 1, "Output for the whitepoint", 0, 4),
        color("multiply", 1, "Multiplies the result", 0, 4),
        color("offset", 0, "Added to the result", -1, 1),
        color("gamma", 1, "Applied last, to positive values", 0.2, 5),
        choice("channels", {"rgb", "rgba"}, "rgb", "Channels to grade"),
        make("black_clamp", Kind::Bool, true, "Clamp results below 0"),
        make("white_clamp", Kind::Bool, false, "Clamp results above 1"),
        make("mix", Kind::Double, 1.0, "Blend between input (0) and result (1)"),
    };
}

}  // namespace puke
