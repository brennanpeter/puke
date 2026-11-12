#include "comp/nodes/MergeNode.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace comp {
namespace {

enum class Op {
    Over, Under, Plus, From, Minus, Multiply, Screen, Difference,
    Max, Min, Average, Copy, In, Out, Atop, Xor, Mask, Stencil
};

Op parseOp(const Node& node, const std::string& s) {
    static const std::pair<std::string_view, Op> table[] = {
        {"over", Op::Over},         {"under", Op::Under},   {"plus", Op::Plus},
        {"from", Op::From},         {"minus", Op::Minus},   {"multiply", Op::Multiply},
        {"screen", Op::Screen},     {"difference", Op::Difference},
        {"max", Op::Max},           {"min", Op::Min},       {"average", Op::Average},
        {"copy", Op::Copy},         {"in", Op::In},         {"out", Op::Out},
        {"atop", Op::Atop},         {"xor", Op::Xor},       {"mask", Op::Mask},
        {"stencil", Op::Stencil},
    };
    for (const auto& [name, op] : table)
        if (name == s) return op;
    throw Error(node.name() + ": unknown merge operation '" + s + "'");
}

inline float apply(Op op, float A, float B, float a, float b) {
    switch (op) {
        case Op::Over: return A + B * (1 - a);
        case Op::Under: return A * (1 - b) + B;
        case Op::Plus: return A + B;
        case Op::From: return B - A;
        case Op::Minus: return A - B;
        case Op::Multiply: return A * B;
        case Op::Screen: return A + B - A * B;
        case Op::Difference: return std::fabs(A - B);
        case Op::Max: return std::max(A, B);
        case Op::Min: return std::min(A, B);
        case Op::Average: return (A + B) * 0.5f;
        case Op::Copy: return A;
        case Op::In: return A * b;
        case Op::Out: return A * (1 - b);
        case Op::Atop: return A * b + B * (1 - a);
        case Op::Xor: return A * (1 - b) + B * (1 - a);
        case Op::Mask: return B * a;
        case Op::Stencil: return B * (1 - a);
    }
    return B;
}

ChannelList mergedChannels(const ChannelList& b, const ChannelList& a) {
    ChannelList out = b;
    for (const auto& ch : a)
        if (std::find(out.begin(), out.end(), ch) == out.end()) out.push_back(ch);
    return out;
}

int indexOf(const ChannelList& list, const std::string& name) {
    auto it = std::find(list.begin(), list.end(), name);
    return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

}  // namespace

ImageInfo MergeNode::computeInfo(const RenderContext&, std::span<const ImageInfo* const> inputs) const {
    const ImageInfo& B = *inputs[0];
    if (!inputs[1]) return B;
    const ImageInfo& A = *inputs[1];
    parseOp(*this, param<std::string>("operation", "over"));  // report bad names early

    ImageInfo info;
    info.displayWindow = B.displayWindow;
    info.channels = mergedChannels(B.channels, A.channels);

    const std::string bbox = param<std::string>("bbox", "union");
    if (bbox == "union") info.dataWindow = A.dataWindow.unite(B.dataWindow);
    else if (bbox == "intersection") info.dataWindow = A.dataWindow.intersect(B.dataWindow);
    else if (bbox == "A") info.dataWindow = A.dataWindow;
    else if (bbox == "B") info.dataWindow = B.dataWindow;
    else throw Error(name() + ": bbox must be union, intersection, A or B, not '" + bbox + "'");
    return info;
}

Image MergeNode::compute(const RenderContext&, const Box& region,
                         std::span<const Image* const> inputs) const {
    const Image& B = *inputs[0];
    if (!inputs[1]) return cropped(B, region);
    const Image& A = *inputs[1];

    const Op op = parseOp(*this, param<std::string>("operation", "over"));
    const float mix = static_cast<float>(numberParam("mix", 1.0));

    const ChannelList channels = mergedChannels(B.channels(), A.channels());
    const int n = static_cast<int>(channels.size());
    std::vector<int> ia(n), ib(n);
    for (int c = 0; c < n; ++c) {
        ia[c] = indexOf(A.channels(), channels[c]);
        ib[c] = indexOf(B.channels(), channels[c]);
    }
    const int aAlpha = A.channelIndex("A");
    const int bAlpha = B.channelIndex("A");

    Image out(region, channels);
    // TODO: row-wise fast paths; at() bounds-checks every read.
    for (int y = region.y0; y < region.y1; ++y) {
        float* px = out.pixel(region.x0, y);
        for (int x = region.x0; x < region.x1; ++x, px += n) {
            const float a = aAlpha >= 0 ? A.at(x, y, aAlpha) : 0.0f;
            const float b = bAlpha >= 0 ? B.at(x, y, bAlpha) : 0.0f;
            for (int c = 0; c < n; ++c) {
                const float Av = ia[c] >= 0 ? A.at(x, y, ia[c]) : 0.0f;
                const float Bv = ib[c] >= 0 ? B.at(x, y, ib[c]) : 0.0f;
                const float v = apply(op, Av, Bv, a, b);
                px[c] = Bv + (v - Bv) * mix;
            }
        }
    }
    return out;
}

}  // namespace comp
