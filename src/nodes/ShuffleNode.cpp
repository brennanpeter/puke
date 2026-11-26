#include "puke/nodes/ShuffleNode.h"

#include <algorithm>

#include "Specs.h"

namespace puke {

namespace {
const char* const kOut[4] = {"R", "G", "B", "A"};
const char* const kParams[4] = {"red", "green", "blue", "alpha"};
const std::vector<std::string> kSources{"R", "G", "B", "A", "0", "1", "2:R", "2:G", "2:B", "2:A"};
}  // namespace

std::vector<ParamSpec> ShuffleNode::paramSpecs() const {
    using namespace specs;
    std::vector<ParamSpec> out;
    for (int i = 0; i < 4; ++i)
        out.push_back(choice(kParams[i], kSources, kOut[i],
                             std::string("Where the output's ") + kOut[i] + " comes from (2:… is the second input)"));
    return out;
}

std::array<std::string, 4> ShuffleNode::sources() const {
    std::array<std::string, 4> s;
    for (int i = 0; i < 4; ++i) {
        s[i] = param<std::string>(kParams[i], kOut[i]);
        if (std::find(kSources.begin(), kSources.end(), s[i]) == kSources.end())
            throw Error(name() + ": " + kParams[i] + " can't come from '" + s[i] +
                        "' (use R, G, B, A, 0, 1, 2:R, 2:G, 2:B or 2:A)");
    }
    return s;
}

bool ShuffleNode::usesSecond() const {
    for (const auto& s : sources())
        if (s.starts_with("2:")) return true;
    return false;
}

ImageInfo ShuffleNode::computeInfo(const RenderContext&, std::span<const ImageInfo* const> inputs) const {
    ImageInfo info = *inputs[0];
    if (usesSecond()) {
        if (inputs.size() < 2 || !inputs[1])
            throw Error(name() + ": takes a channel from input 2, which isn't connected");
        info.dataWindow = info.dataWindow.unite(inputs[1]->dataWindow);
    }
    // RGBA always, other channels after.
    ChannelList channels{"R", "G", "B", "A"};
    for (const auto& c : inputs[0]->channels)
        if (std::find(channels.begin(), channels.end(), c) == channels.end()) channels.push_back(c);
    info.channels = channels;
    return info;
}

Box ShuffleNode::inputRegion(const RenderContext&, int index, const Box& outputRegion) const {
    if (index == 1 && !usesSecond()) return {};
    return outputRegion;
}

Image ShuffleNode::compute(const RenderContext&, const Box& region, std::span<const Image* const> inputs) const {
    const Image& a = *inputs[0];
    const Image* b = inputs.size() > 1 ? inputs[1] : nullptr;
    const auto srcs = sources();

    ChannelList channels{"R", "G", "B", "A"};
    for (const auto& c : a.channels())
        if (std::find(channels.begin(), channels.end(), c) == channels.end()) channels.push_back(c);
    Image out(region, channels);

    // For each output channel: an image and a channel in it, or a constant.
    struct From {
        const Image* img = nullptr;
        int channel = -1;
        float constant = 0;
    };
    std::vector<From> from(channels.size());
    for (std::size_t c = 0; c < channels.size(); ++c) {
        const std::string s = c < 4 ? srcs[c] : channels[c];
        if (s == "0" || s == "1") from[c].constant = s == "1" ? 1.0f : 0.0f;
        else if (s.starts_with("2:")) from[c] = {b, b ? b->channelIndex(s.substr(2)) : -1, 0};
        else from[c] = {&a, a.channelIndex(s), 0};
    }

    const int n = static_cast<int>(channels.size());
    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += n)
            for (int c = 0; c < n; ++c) {
                const From& f = from[c];
                px[c] = f.img ? (f.channel >= 0 ? f.img->at(x, y, f.channel) : 0.0f) : f.constant;
            }
    }
    return out;
}

}  // namespace puke
