#include "OfxNode.h"

#include "comp/ofx/OfxHost.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace comp::ofx {

namespace {

const ChannelList kRGBA{"R", "G", "B", "A"};

void setRect(PropertySet& ps, const char* name, const OfxRectI& r) { ps.setInts(name, {r.x1, r.y1, r.x2, r.y2}); }
void setRectD(PropertySet& ps, const char* name, const OfxRectI& r) {
    ps.setDoubles(name, {double(r.x1), double(r.y1), double(r.x2), double(r.y2)});
}
OfxRectD getRectD(const PropertySet& ps, const char* name) {
    const auto v = ps.getDoubles(name);
    if (v.size() < 4) return {0, 0, 0, 0};
    return {v[0], v[1], v[2], v[3]};
}

bool supports(const PropertySet& ps, const char* prop, const char* value) {
    const auto v = ps.getStrings(prop);
    return std::find(v.begin(), v.end(), value) != v.end();
}

// Render-scale-1, single-field arguments shared by the render actions.
void addRenderArgs(PropertySet& args, double time) {
    args.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    args.setInt(kOfxImageEffectPropSequentialRenderStatus, 0, 0);
    args.setInt(kOfxImageEffectPropInteractiveRenderStatus, 0, 0);
    args.setDouble(kOfxPropTime, 0, time);
}

}  // namespace

// ================================================================ setup

OfxNode::OfxNode(std::string name, Plugin& plugin)
    : Node(std::move(name)), plugin_(plugin), typeName_(std::string(kNodeTypePrefix) + plugin.identifier) {
    instance_ = std::make_unique<Effect>();
    instance_->copyFrom(*plugin.contextDescriptor);
    instance_->node = this;

    Effect& e = *instance_;
    e.setString(kOfxImageEffectPropContext, 0, plugin.context);
    e.setInt(kOfxPropIsInteractive, 0, 0);
    e.setInt(kOfxImageEffectInstancePropSequentialRender, 0, 0);
    e.setDoubles(kOfxImageEffectPropProjectSize, {1920, 1080});  // updated from the input on each info pass
    e.setDoubles(kOfxImageEffectPropProjectOffset, {0, 0});
    e.setDoubles(kOfxImageEffectPropProjectExtent, {1920, 1080});
    e.setDouble(kOfxImageEffectPropProjectPixelAspectRatio, 0, 1.0);
    e.setDouble(kOfxImageEffectInstancePropEffectDuration, 0, 1.0);
    e.setDouble(kOfxImageEffectPropFrameRate, 0, 24.0);
    e.setPointer(kOfxPropInstanceData, 0, nullptr);

    // Inputs: the Source clip first (it's input 0 by convention), then the
    // others in the order the plugin defined them.
    for (const auto& c : e.clips) {
        if (c->name == kOfxImageEffectOutputClipName) outputClip_ = c.get();
        else if (c->name == kOfxImageEffectSimpleSourceClipName) inputClips_.insert(inputClips_.begin(), c.get());
        else inputClips_.push_back(c.get());
    }
    for (std::size_t i = 0; i < inputClips_.size(); ++i) inputClips_[i]->input = static_cast<int>(i);

    for (const auto& c : e.clips) {
        const bool rgba = supports(*c, kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA);
        const char* comps = rgba ? kOfxImageComponentRGBA : kOfxImageComponentAlpha;
        c->setInt(kOfxImageClipPropConnected, 0, c.get() == outputClip_ ? 1 : 0);
        c->setString(kOfxImageEffectPropComponents, 0, comps);
        c->setString(kOfxImageClipPropUnmappedComponents, 0, comps);
        c->setString(kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
        c->setString(kOfxImageClipPropUnmappedPixelDepth, 0, kOfxBitDepthFloat);
        c->setString(kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied);
        c->setDouble(kOfxImagePropPixelAspectRatio, 0, 1.0);
        c->setDouble(kOfxImageEffectPropFrameRate, 0, 24.0);
        c->setDouble(kOfxImageEffectPropUnmappedFrameRate, 0, 24.0);
        c->setDoubles(kOfxImageEffectPropFrameRange, {0, 0});
        c->setDoubles(kOfxImageEffectPropUnmappedFrameRange, {0, 0});
        c->setString(kOfxImageClipPropFieldOrder, 0, kOfxImageFieldNone);
        c->setInt(kOfxImageClipPropContinuousSamples, 0, 0);
    }

    plugin_.call(kOfxActionCreateInstance, e.handle(), nullptr, nullptr, who());
}

OfxNode::~OfxNode() {
    try {
        plugin_.call(kOfxActionDestroyInstance, instance_->handle(), nullptr, nullptr, who());
    } catch (...) {  // nothing useful to do about it now
    }
}

std::string OfxNode::who() const { return name() + " (" + plugin_.identifier + ")"; }

bool OfxNode::inputOptional(int index) const {
    if (index < 0 || index >= maxInputs()) return false;
    const Clip& c = *inputClips_[index];
    return c.getInt(kOfxImageClipPropOptional) != 0 || c.getInt(kOfxImageClipPropIsMask) != 0;
}

bool OfxNode::supportsTiles() const {
    if (!plugin_.contextDescriptor->getInt(kOfxImageEffectPropSupportsTiles, 0, 1)) return false;
    for (const auto& c : instance_->clips)
        if (!c->getInt(kOfxImageEffectPropSupportsTiles, 0, 1)) return false;
    return true;
}

void OfxNode::appendHash(Hasher& h, const RenderContext&) const {
    h.add(std::string_view(plugin_.identifier))
        .add(plugin_.api->pluginVersionMajor)
        .add(plugin_.api->pluginVersionMinor)
        .add(std::string_view(plugin_.context));
}

std::vector<ParamSpec> OfxNode::paramSpecs() const {
    using Kind = ParamSpec::Kind;
    std::vector<ParamSpec> out;
    for (const auto& p : instance_->params.params) {
        const std::string& t = p->type;
        if (p->getInt(kOfxParamPropSecret)) continue;
        ParamSpec s;
        s.name = p->name;
        s.label = p->getString(kOfxPropLabel, 0, p->name);
        s.hint = p->getString(kOfxParamPropHint);
        const auto def = p->getDoubles(kOfxParamPropDefault);
        auto d = [&](std::size_t i) { return i < def.size() ? def[i] : 0.0; };
        if (t == kOfxParamTypeBoolean) {
            s.kind = Kind::Bool;
            s.defaultValue = d(0) != 0;
        } else if (t == kOfxParamTypeInteger) {
            s.kind = Kind::Int;
            s.defaultValue = static_cast<int>(d(0));
        } else if (t == kOfxParamTypeDouble) {
            s.kind = Kind::Double;
            s.defaultValue = d(0);
        } else if (t == kOfxParamTypeChoice) {
            s.kind = Kind::Choice;
            s.options = p->getStrings(kOfxParamPropChoiceOption);
            const auto i = static_cast<std::size_t>(d(0));
            s.defaultValue = i < s.options.size() ? s.options[i] : std::string();
        } else if (t == kOfxParamTypeString || t == kOfxParamTypeCustom) {
            const std::string mode = p->getString(kOfxParamPropStringMode);
            s.kind = mode == kOfxParamStringIsFilePath || mode == kOfxParamStringIsDirectoryPath ? Kind::File : Kind::String;
            s.defaultValue = p->getString(kOfxParamPropDefault);
        } else if (t == kOfxParamTypeDouble2D || t == kOfxParamTypeInteger2D) {
            s.kind = Kind::Vec2;
            s.defaultValue = Vec2{d(0), d(1)};
        } else if (t == kOfxParamTypeRGB || t == kOfxParamTypeRGBA || t == kOfxParamTypeDouble3D ||
                   t == kOfxParamTypeInteger3D) {
            s.kind = Kind::Color;
            s.defaultValue = Color{d(0), d(1), d(2), t == kOfxParamTypeRGBA ? d(3) : 1.0};
        } else {
            continue;  // groups, pages, buttons, parametric curves: no value to show
        }
        // Slider range: the plugin's display range, else something sensible.
        const auto lo = p->getDoubles(kOfxParamPropDisplayMin), hi = p->getDoubles(kOfxParamPropDisplayMax);
        s.min = !lo.empty() && lo[0] > -1e9 ? lo[0] : (s.kind == Kind::Int ? 0 : 0.0);
        s.max = !hi.empty() && hi[0] < 1e9 ? hi[0] : (s.kind == Kind::Int ? 100 : 1.0);
        out.push_back(std::move(s));
    }
    return out;
}

void OfxNode::fillContext(CallContext& call, const FrameInfo& fi, double time) const {
    call.node = this;
    call.time = time;
    call.flipY = fi.flipY;
    call.inputInfos = fi.inputs;
    call.connected = fi.connected;
    call.outputInfo = fi.output;
}

OfxNode::FrameInfo OfxNode::frameInfo(const RenderContext& ctx) const {
    std::lock_guard lock(infoMutex_);
    auto it = frameInfos_.find(ctx.frame);
    if (it == frameInfos_.end()) throw Error(who() + ": asked to render before its info pass for this frame");
    return it->second;
}

// ================================================================ engine hooks

ImageInfo OfxNode::computeInfo(const RenderContext& ctx, std::span<const ImageInfo* const> inputs) const {
    FrameInfo fi;
    const ImageInfo* first = nullptr;
    for (const ImageInfo* in : inputs) {
        fi.inputs.push_back(in ? *in : ImageInfo{});
        fi.connected.push_back(in != nullptr);
        if (in && !first) first = in;
    }
    if (!first) throw Error(who() + ": needs at least one input connected");
    const Box display = first->displayWindow;
    fi.flipY = display.y0 + display.y1;

    std::lock_guard lock(actionMutex_);
    std::unique_lock unsafe(plugin_.renderMutex, std::defer_lock);
    if (plugin_.contextDescriptor->getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe)
        unsafe.lock();

    // The project is the input's frame, in OFX coordinates.
    Effect& e = *instance_;
    const OfxRectI project = toOfx(display, fi.flipY);
    e.setDoubles(kOfxImageEffectPropProjectOffset, {double(project.x1), double(project.y1)});
    e.setDoubles(kOfxImageEffectPropProjectSize, {double(project.x2 - project.x1), double(project.y2 - project.y1)});
    e.setDoubles(kOfxImageEffectPropProjectExtent, {double(project.x2), double(project.y2)});
    for (std::size_t i = 0; i < inputClips_.size(); ++i)
        inputClips_[i]->setInt(kOfxImageClipPropConnected, 0, fi.connected[i] ? 1 : 0);
    for (const auto& c : e.clips) {
        c->setDoubles(kOfxImageEffectPropFrameRange, {ctx.frame, ctx.frame});
        c->setDoubles(kOfxImageEffectPropUnmappedFrameRange, {ctx.frame, ctx.frame});
    }

    // Default region of definition: the Source's for filters, the union of
    // the connected inputs' otherwise.
    Box defaultRod;
    for (std::size_t i = 0; i < fi.inputs.size(); ++i)
        if (fi.connected[i]) defaultRod = defaultRod.unite(fi.inputs[i].dataWindow);
    if (plugin_.context == kOfxImageEffectContextFilter && fi.connected[0]) defaultRod = fi.inputs[0].dataWindow;

    CallContext call;
    fillContext(call, fi, ctx.frame);
    ScopedContext scope(&call);
    PropertySet in, out;
    in.setDouble(kOfxPropTime, 0, ctx.frame);
    in.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    setRectD(out, kOfxImageEffectPropRegionOfDefinition, toOfx(defaultRod, fi.flipY));
    plugin_.call(kOfxImageEffectActionGetRegionOfDefinition, e.handle(), &in, &out, who());

    Box rod = fromOfx(getRectD(out, kOfxImageEffectPropRegionOfDefinition), fi.flipY);
    // "Infinite" edges become the frame's edges.
    constexpr int kHuge = 100'000'000;
    if (rod.x0 < -kHuge) rod.x0 = display.x0;
    if (rod.y0 < -kHuge) rod.y0 = display.y0;
    if (rod.x1 > kHuge) rod.x1 = display.x1;
    if (rod.y1 > kHuge) rod.y1 = display.y1;

    fi.output = ImageInfo{rod, display, kRGBA};
    {
        std::lock_guard infoLock(infoMutex_);
        if (frameInfos_.size() > 64) frameInfos_.clear();  // keep it small; the next info pass refills it
        frameInfos_[ctx.frame] = fi;
    }
    return fi.output;
}

Box OfxNode::inputRegion(const RenderContext& ctx, int index, const Box& outputRegion) const {
    const FrameInfo fi = frameInfo(ctx);
    std::lock_guard lock(actionMutex_);
    std::unique_lock unsafe(plugin_.renderMutex, std::defer_lock);
    if (plugin_.contextDescriptor->getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe)
        unsafe.lock();

    CallContext call;
    fillContext(call, fi, ctx.frame);
    ScopedContext scope(&call);
    PropertySet in, out;
    in.setDouble(kOfxPropTime, 0, ctx.frame);
    in.setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    const OfxRectI wanted = toOfx(outputRegion, fi.flipY);
    setRectD(in, kOfxImageEffectPropRegionOfInterest, wanted);
    for (const Clip* c : inputClips_) setRectD(out, ("OfxImageClipPropRoI_" + c->name).c_str(), wanted);
    plugin_.call(kOfxImageEffectActionGetRegionsOfInterest, instance_->handle(), &in, &out, who());
    return fromOfx(getRectD(out, ("OfxImageClipPropRoI_" + inputClips_[index]->name).c_str()), fi.flipY);
}

Image OfxNode::compute(const RenderContext& ctx, const Box& region, std::span<const Image* const> inputs) const {
    const FrameInfo fi = frameInfo(ctx);
    std::vector<float> pixels(static_cast<std::size_t>(region.area()) * 4, 0.0f);

    CallContext call;
    fillContext(call, fi, ctx.frame);
    call.inputs.assign(inputs.begin(), inputs.end());
    call.renderWindow = region;
    call.output = &pixels;

    // Respect the plugin's declared thread safety.
    const std::string safety = plugin_.contextDescriptor->getString(kOfxImageEffectPluginRenderThreadSafety, 0,
                                                                     kOfxImageEffectRenderInstanceSafe);
    std::unique_lock pluginLock(plugin_.renderMutex, std::defer_lock);
    std::unique_lock instanceLock(actionMutex_, std::defer_lock);
    if (safety == kOfxImageEffectRenderUnsafe) pluginLock.lock();
    else if (safety != kOfxImageEffectRenderFullySafe) instanceLock.lock();

    ScopedContext scope(&call);
    PropertySet seq;
    addRenderArgs(seq, ctx.frame);
    seq.setDoubles(kOfxImageEffectPropFrameRange, {ctx.frame, ctx.frame});
    seq.setDouble(kOfxImageEffectPropFrameStep, 0, 1.0);
    seq.setInt(kOfxPropIsInteractive, 0, 0);
    PropertySet args;
    addRenderArgs(args, ctx.frame);
    args.setString(kOfxImageEffectPropFieldToRender, 0, kOfxImageFieldNone);
    args.setInt(kOfxImageEffectPropRenderQualityDraft, 0, 0);
    setRect(args, kOfxImageEffectPropRenderWindow, toOfx(region, fi.flipY));

    const auto handle = instance_->handle();
    plugin_.call(kOfxImageEffectActionBeginSequenceRender, handle, &seq, nullptr, who());
    try {
        plugin_.call(kOfxImageEffectActionRender, handle, &args, nullptr, who());
    } catch (...) {
        try {
            plugin_.call(kOfxImageEffectActionEndSequenceRender, handle, &seq, nullptr, who());
        } catch (...) {
        }
        throw;
    }
    plugin_.call(kOfxImageEffectActionEndSequenceRender, handle, &seq, nullptr, who());

    // OFX rows run bottom-up: the first row is comp's last.
    Image out(region, kRGBA);
    const std::size_t rowFloats = static_cast<std::size_t>(region.width()) * 4;
    for (int y = region.y0; y < region.y1; ++y)
        std::memcpy(out.pixel(region.x0, y), pixels.data() + (region.y1 - 1 - y) * rowFloats,
                    rowFloats * sizeof(float));
    return out;
}

// ================================================================ suite callbacks

OfxStatus OfxNode::clipImage(Clip& clip, double time, const OfxRectD*, OfxPropertySetHandle* out) {
    CallContext* call = currentContext();
    if (!clip.effect || !call || call->node != clip.effect->node || !call->output) {
        noteProblem("clip images can only be fetched while rendering");
        return kOfxStatFailed;
    }
    if (std::fabs(time - call->time) > 1e-6) {
        noteProblem("comp can only provide the frame being rendered (asked for " + std::to_string(time) + ")");
        return kOfxStatFailed;
    }

    const float* data = nullptr;
    Box bounds, rod;
    int nch = 4;
    static const float dummy[4] = {0, 0, 0, 0};  // plugins expect a pointer even for empty images

    if (clip.input < 0) {  // the output clip: the buffer being rendered
        data = call->output->data();
        bounds = call->renderWindow;
        rod = call->outputInfo.dataWindow;
    } else {
        const std::size_t idx = static_cast<std::size_t>(clip.input);
        if (idx >= call->inputs.size() || !call->inputs[idx]) return kOfxStatFailed;  // not connected
        const Image& img = *call->inputs[idx];
        const bool alphaOnly = clip.getString(kOfxImageEffectPropComponents) == kOfxImageComponentAlpha;
        nch = alphaOnly ? 1 : 4;
        bounds = img.region();
        rod = call->inputInfos[idx].dataWindow;

        std::lock_guard lock(call->buffersMutex);
        auto [it, fresh] = call->buffers.try_emplace(&clip);
        std::vector<float>& buf = it->second;
        if (fresh) {
            // comp channels -> RGBA (or A): R/G/B by name (a lone Y as grey),
            // alpha 1 where the image has none.
            auto index = [&](const char* name) { return img.channelIndex(name); };
            int r = index("R"), g = index("G"), b = index("B"), a = index("A");
            if (a < 0) a = index("alpha");
            if (r < 0 && g < 0 && b < 0) r = g = b = index("Y");
            const int src[4] = {r, g, b, a};
            buf.assign(static_cast<std::size_t>(bounds.area()) * nch, 0.0f);
            const int w = bounds.width();
            for (int y = bounds.y0; y < bounds.y1; ++y) {
                float* row = buf.data() + static_cast<std::size_t>(bounds.y1 - 1 - y) * w * nch;
                const float* p = img.pixel(bounds.x0, y);
                for (int x = 0; x < w; ++x, p += img.numChannels()) {
                    if (alphaOnly) {
                        row[x] = a >= 0 ? p[a] : 1.0f;
                    } else {
                        for (int c = 0; c < 3; ++c) row[4 * x + c] = src[c] >= 0 ? p[src[c]] : 0.0f;
                        row[4 * x + 3] = a >= 0 ? p[a] : 1.0f;
                    }
                }
            }
        }
        data = buf.data();
    }

    auto* image = new PropertySet;
    image->setString(kOfxPropType, 0, kOfxTypeImage);
    image->setPointer(kOfxImagePropData, 0, const_cast<float*>(data && bounds.area() > 0 ? data : dummy));
    setRect(*image, kOfxImagePropBounds, toOfx(bounds, call->flipY));
    setRect(*image, kOfxImagePropRegionOfDefinition, toOfx(rod, call->flipY));
    image->setInt(kOfxImagePropRowBytes, 0, bounds.width() * nch * static_cast<int>(sizeof(float)));
    image->setString(kOfxImageEffectPropComponents, 0, nch == 1 ? kOfxImageComponentAlpha : kOfxImageComponentRGBA);
    image->setString(kOfxImageEffectPropPixelDepth, 0, kOfxBitDepthFloat);
    image->setString(kOfxImageEffectPropPreMultiplication, 0, kOfxImagePreMultiplied);
    image->setDouble(kOfxImagePropPixelAspectRatio, 0, 1.0);
    image->setDoubles(kOfxImageEffectPropRenderScale, {1.0, 1.0});
    image->setString(kOfxImagePropField, 0, kOfxImageFieldNone);
    image->setString(kOfxImagePropUniqueIdentifier, 0,
                     clip.name + "@" + std::to_string(time) + "#" + std::to_string(reinterpret_cast<std::uintptr_t>(data)));
    *out = image->handle();
    return kOfxStatOK;
}

OfxStatus OfxNode::clipRegionOfDefinition(Clip& clip, double, OfxRectD* bounds) {
    CallContext* call = currentContext();
    if (!clip.effect || !call || call->node != clip.effect->node) return kOfxStatFailed;
    Box rod;
    if (clip.input < 0) rod = call->outputInfo.dataWindow;
    else if (static_cast<std::size_t>(clip.input) < call->inputInfos.size() && call->connected[clip.input])
        rod = call->inputInfos[clip.input].dataWindow;
    const OfxRectI r = toOfx(rod, call->flipY);
    *bounds = {double(r.x1), double(r.y1), double(r.x2), double(r.y2)};
    return kOfxStatOK;
}

bool OfxNode::paramValue(const Param& param, std::vector<double>& numbers, std::string& text) {
    const OfxNode* node = param.effect ? param.effect->node : nullptr;
    const std::string& type = param.type;
    const bool isString = type == kOfxParamTypeString || type == kOfxParamTypeCustom;
    int dims = 1;
    if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D) dims = 2;
    else if (type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D || type == kOfxParamTypeRGB) dims = 3;
    else if (type == kOfxParamTypeRGBA) dims = 4;

    // The user's value, then one the plugin set itself, then the default.
    const ParamValue* value = nullptr;
    ParamValue fromPlugin;
    if (node) {
        auto it = node->params().find(param.name);
        if (it != node->params().end()) {
            value = &it->second;
        } else {
            std::lock_guard lock(node->pluginValuesMutex_);
            auto pv = node->pluginValues_.find(param.name);
            if (pv != node->pluginValues_.end()) {
                fromPlugin = pv->second;
                value = &fromPlugin;
            }
        }
    }
    if (!value) {
        if (isString) text = param.getString(kOfxParamPropDefault);
        else {
            numbers = param.getDoubles(kOfxParamPropDefault);
            numbers.resize(dims, 0.0);
        }
        return true;
    }

    auto fail = [&](const std::string& expected) {
        noteProblem((node ? node->name() + ": " : std::string()) + "parameter '" + param.name + "' must be " +
                    expected);
        return false;
    };
    const char* expected = dims == 1 ? "a number" : dims == 2 ? "2 numbers" : "3 or 4 numbers (a Color)";
    return std::visit(
        [&](const auto& v) -> bool {
            using T = std::decay_t<decltype(v)>;
            if (isString) {
                if constexpr (std::is_same_v<T, std::string>) {
                    text = v;
                    return true;
                } else {
                    return fail("a string");
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                if (type != kOfxParamTypeChoice) return fail(expected);
                const auto options = param.getStrings(kOfxParamPropChoiceOption);
                for (std::size_t i = 0; i < options.size(); ++i)
                    if (options[i] == v) {
                        numbers = {double(i)};
                        return true;
                    }
                std::string list;
                for (const auto& o : options) list += (list.empty() ? "" : ", ") + o;
                return fail("one of: " + list + " (or its index)");
            } else if constexpr (std::is_same_v<T, Vec2>) {
                if (dims != 2) return fail(expected);
                numbers = {v[0], v[1]};
                return true;
            } else if constexpr (std::is_same_v<T, Color>) {
                if (dims < 3) return fail(expected);
                numbers.assign(v.begin(), v.begin() + dims);
                return true;
            } else {  // bool, int, double
                if (dims != 1) return fail(expected);
                numbers = {static_cast<double>(v)};
                return true;
            }
        },
        *value);
}

void OfxNode::setValueFromPlugin(const Param& param, ParamValue value) {
    const OfxNode* node = param.effect ? param.effect->node : nullptr;
    if (!node) return;  // a descriptor: nothing to store
    std::lock_guard lock(node->pluginValuesMutex_);
    node->pluginValues_[param.name] = std::move(value);
}

}  // namespace comp::ofx
