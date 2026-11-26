#pragma once

#include <map>
#include <tuple>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "OfxInternal.h"

namespace comp::ofx {

// A comp node running one instance of an OFX image effect. Created through
// comp::ofx::createNode() (or the registry, as "ofx:<identifier>").
//
// How each Node hook maps onto OFX:
//   computeInfo  -> kOfxImageEffectActionGetRegionOfDefinition
//   inputRegion  -> kOfxImageEffectActionGetRegionsOfInterest
//   compute      -> begin sequence render, render, end sequence render, for one
//                   tile (or the whole region if the plugin doesn't do tiles)
//
// Images are float RGBA (or Alpha, for clips that only take alpha), copied
// into OFX's bottom-up row order and back. y is flipped around the input's
// display window: OFX y = flipY - comp y, flipY = display y0 + y1.
class OfxNode : public Node {
public:
    OfxNode(std::string name, Plugin& plugin);
    ~OfxNode() override;

    const char* typeName() const override { return typeName_.c_str(); }
    int maxInputs() const override { return static_cast<int>(inputClips_.size()); }
    bool inputOptional(int index) const override;
    std::string inputLabel(int index) const override {
        return index >= 0 && index < maxInputs() ? inputClips_[index]->name : std::string();
    }

    ImageInfo computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const override;
    Box inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const override;
    Image compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const override;
    bool supportsTiles() const override;
    void appendHash(Hasher& h, const RenderContext& ctx) const override;
    // From the plugin's parameter descriptions (secret ones left out).
    std::vector<ParamSpec> paramSpecs() const override;

    // Called by the suites (OfxHost.cpp).
    static OfxStatus clipImage(Clip& clip, double time, const OfxRectD* region, OfxPropertySetHandle* out);
    static OfxStatus clipRegionOfDefinition(Clip& clip, double time, OfxRectD* bounds);
    // A parameter's current value: numbers (per its dimension) or text.
    // False, with a message for the plugin's error, if the stored value
    // doesn't fit the parameter.
    static bool paramValue(const Param& param, std::vector<double>& numbers, std::string& text);
    static void setValueFromPlugin(const Param& param, ParamValue value);

private:
    // What the info pass worked out, per frame, for the region and render
    // passes that follow it (their Node hooks don't get input infos).
    struct FrameInfo {
        std::vector<ImageInfo> inputs;
        std::vector<bool> connected;
        ImageInfo output;
        int flipY = 0;           // pixel coordinates (at the render scale): OFX y = flipY - comp y
        double scale = 1;        // the render scale
        int flipCanonical = 0;   // canonical (full-size) coordinates' flip
        // comp pixels <-> OFX canonical coordinates.
        OfxRectI toCanonical(const Box& pixels) const;
        Box fromCanonical(const OfxRectD& canonical) const;
    };
    FrameInfo frameInfo(const RenderContext& ctx) const;
    void fillContext(CallContext& call, const FrameInfo& fi, double time) const;
    std::string who() const;

    Plugin& plugin_;
    std::string typeName_;
    std::unique_ptr<Effect> instance_;
    std::vector<Clip*> inputClips_;  // comp input i feeds inputClips_[i]
    Clip* outputClip_ = nullptr;

    mutable std::mutex actionMutex_;  // instance-safe plugins: one action at a time
    mutable std::mutex infoMutex_;
    // By (frame, scale, format width, height): renders at different scales or
    // formats (a proxy viewer beside a full-size render) mustn't share one.
    using FrameKey = std::tuple<double, double, int, int>;
    static FrameKey keyOf(const RenderContext& ctx) { return {ctx.frame, ctx.scale, ctx.format.width, ctx.format.height}; }
    mutable std::map<FrameKey, FrameInfo> frameInfos_;
    mutable std::mutex pluginValuesMutex_;
    mutable std::map<std::string, ParamValue> pluginValues_;  // values the plugin set itself
};

}  // namespace comp::ofx
