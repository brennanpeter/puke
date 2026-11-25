// OFX plugins for testing comp's host, written against the plain C API like
// any third-party plugin. Built into tests' ofx/compTestPlugins.ofx.bundle.
//
//   org.comp.test.Gain          Filter. Every parameter type; posts an error
//                               when asked to fail. Fully thread safe, tiles.
//   org.comp.test.Shift         Filter. Moves the image by whole pixels (OFX
//                               coordinates, y up): region of definition and
//                               regions of interest. Instance safe.
//   org.comp.test.MaskMultiply  General. Source times an optional alpha-only
//                               Mask. Declares itself thread unsafe, no tiles,
//                               and fails if two renders ever overlap.
//   org.comp.test.Coords        Filter. Writes each pixel's OFX coordinates
//                               and the project offset, pinning down how
//                               comp maps its y-down pixels onto OFX's y-up.
//   org.comp.test.Generator     General with no inputs: comp should refuse it.
//   org.comp.test.ByteOnly      8-bit only: comp should refuse it.

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#if defined(_WIN32)
#define EXPORT extern "C" __declspec(dllexport)
#else
#define EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

OfxHost* gHost = nullptr;
const OfxPropertySuiteV1* gProp = nullptr;
const OfxImageEffectSuiteV1* gEffect = nullptr;
const OfxParameterSuiteV1* gParam = nullptr;
const OfxMessageSuiteV1* gMessage = nullptr;

void setHost(OfxHost* host) {
    gHost = host;
    gProp = static_cast<const OfxPropertySuiteV1*>(host->fetchSuite(host->host, kOfxPropertySuite, 1));
    gEffect = static_cast<const OfxImageEffectSuiteV1*>(host->fetchSuite(host->host, kOfxImageEffectSuite, 1));
    gParam = static_cast<const OfxParameterSuiteV1*>(host->fetchSuite(host->host, kOfxParameterSuite, 1));
    gMessage = static_cast<const OfxMessageSuiteV1*>(host->fetchSuite(host->host, kOfxMessageSuite, 1));
}

OfxStatus fail(OfxImageEffectHandle effect, const std::string& msg) {
    gMessage->message(effect, kOfxMessageError, "", "%s", msg.c_str());
    return kOfxStatFailed;
}

// ---------------------------------------------------------------- helpers

OfxPropertySetHandle effectProps(OfxImageEffectHandle e) {
    OfxPropertySetHandle p;
    gEffect->getPropertySet(e, &p);
    return p;
}

OfxParamSetHandle paramSet(OfxImageEffectHandle e) {
    OfxParamSetHandle ps;
    gEffect->getParamSet(e, &ps);
    return ps;
}

OfxParamHandle param(OfxImageEffectHandle e, const char* name) {
    OfxParamHandle p = nullptr;
    gParam->paramGetHandle(paramSet(e), name, &p, nullptr);
    return p;
}

void describeCommon(OfxImageEffectHandle e, const char* label, const char* context, const char* depth,
                    const char* safety, int tiles) {
    OfxPropertySetHandle p = effectProps(e);
    gProp->propSetString(p, kOfxPropLabel, 0, label);
    gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, context);
    gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, depth);
    gProp->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, safety);
    gProp->propSetInt(p, kOfxImageEffectPropSupportsTiles, 0, tiles);
}

OfxPropertySetHandle defineClip(OfxImageEffectHandle e, const char* name, const char* components) {
    OfxPropertySetHandle p;
    gEffect->clipDefine(e, name, &p);
    gProp->propSetString(p, kOfxImageEffectPropSupportedComponents, 0, components);
    return p;
}

OfxPropertySetHandle defineParam(OfxImageEffectHandle e, const char* type, const char* name) {
    OfxPropertySetHandle p;
    gParam->paramDefine(paramSet(e), type, name, &p);
    return p;
}

// An image fetched from a clip, with a pixel accessor in OFX coordinates.
struct Img {
    OfxPropertySetHandle handle = nullptr;
    char* data = nullptr;
    OfxRectI bounds{0, 0, 0, 0};
    int rowBytes = 0;
    int nch = 4;

    bool fetch(OfxImageEffectHandle e, const char* clip, OfxTime t) {
        OfxImageClipHandle c;
        if (gEffect->clipGetHandle(e, clip, &c, nullptr) != kOfxStatOK) return false;
        if (gEffect->clipGetImage(c, t, nullptr, &handle) != kOfxStatOK) return false;
        void* d;
        gProp->propGetPointer(handle, kOfxImagePropData, 0, &d);
        data = static_cast<char*>(d);
        gProp->propGetIntN(handle, kOfxImagePropBounds, 4, &bounds.x1);
        gProp->propGetInt(handle, kOfxImagePropRowBytes, 0, &rowBytes);
        char* comps;
        gProp->propGetString(handle, kOfxImageEffectPropComponents, 0, &comps);
        nch = std::strcmp(comps, kOfxImageComponentAlpha) == 0 ? 1 : 4;
        return true;
    }
    ~Img() {
        if (handle) gEffect->clipReleaseImage(handle);
    }
    bool inside(int x, int y) const { return x >= bounds.x1 && x < bounds.x2 && y >= bounds.y1 && y < bounds.y2; }
    float* at(int x, int y) const {
        return reinterpret_cast<float*>(data + static_cast<long>(y - bounds.y1) * rowBytes) + (x - bounds.x1) * nch;
    }
};

bool isConnected(OfxImageEffectHandle e, const char* clip) {
    OfxImageClipHandle c;
    OfxPropertySetHandle props;
    if (gEffect->clipGetHandle(e, clip, &c, &props) != kOfxStatOK) return false;
    int connected = 0;
    gProp->propGetInt(props, kOfxImageClipPropConnected, 0, &connected);
    return connected != 0;
}

void renderArgs(OfxPropertySetHandle in, OfxTime& t, OfxRectI& window) {
    gProp->propGetDouble(in, kOfxPropTime, 0, &t);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, &window.x1);
}

// ================================================================ Gain

OfxStatus gainDescribeInContext(OfxImageEffectHandle e) {
    defineClip(e, kOfxImageEffectOutputClipName, kOfxImageComponentRGBA);
    defineClip(e, kOfxImageEffectSimpleSourceClipName, kOfxImageComponentRGBA);

    OfxPropertySetHandle p = defineParam(e, kOfxParamTypeDouble, "gain");
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 1.0);
    p = defineParam(e, kOfxParamTypeRGBA, "offset");  // default 0,0,0,0
    p = defineParam(e, kOfxParamTypeBoolean, "invert");
    p = defineParam(e, kOfxParamTypeChoice, "mode");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 0, "multiply");
    gProp->propSetString(p, kOfxParamPropChoiceOption, 1, "add");
    p = defineParam(e, kOfxParamTypeString, "note");
    gProp->propSetString(p, kOfxParamPropDefault, 0, "hello");
    p = defineParam(e, kOfxParamTypeBoolean, "fail");
    p = defineParam(e, kOfxParamTypeInteger, "count");
    gProp->propSetInt(p, kOfxParamPropDefault, 0, 3);
    p = defineParam(e, kOfxParamTypeDouble2D, "size");
    gProp->propSetDouble(p, kOfxParamPropDefault, 0, 1.5);
    gProp->propSetDouble(p, kOfxParamPropDefault, 1, 2.5);
    defineParam(e, kOfxParamTypeGroup, "extras");  // has no value
    return kOfxStatOK;
}

OfxStatus gainRender(OfxImageEffectHandle e, OfxPropertySetHandle in) {
    OfxTime t;
    OfxRectI w;
    renderArgs(in, t, w);

    double gain = 0, off[4] = {0, 0, 0, 0}, size[2] = {0, 0};
    int invert = 0, mode = 0, failFlag = 0, count = 0;
    const char* note = nullptr;
    if (gParam->paramGetValue(param(e, "fail"), &failFlag) != kOfxStatOK) return kOfxStatFailed;
    if (failFlag) return fail(e, "gain plugin asked to fail");
    if (gParam->paramGetValue(param(e, "gain"), &gain) != kOfxStatOK ||
        gParam->paramGetValueAtTime(param(e, "offset"), t, &off[0], &off[1], &off[2], &off[3]) != kOfxStatOK ||
        gParam->paramGetValue(param(e, "invert"), &invert) != kOfxStatOK ||
        gParam->paramGetValue(param(e, "mode"), &mode) != kOfxStatOK ||
        gParam->paramGetValue(param(e, "note"), &note) != kOfxStatOK ||
        gParam->paramGetValue(param(e, "count"), &count) != kOfxStatOK ||
        gParam->paramGetValue(param(e, "size"), &size[0], &size[1]) != kOfxStatOK)
        return kOfxStatFailed;  // the host has already said why
    if (count != 3 && count != 7) return fail(e, "unexpected count " + std::to_string(count));
    if (std::string(note) != "hello" && std::string(note) != "world") return fail(e, "unexpected note");
    if (size[0] != 1.5 || size[1] != 2.5) return fail(e, "unexpected size");

    Img src, dst;
    if (!dst.fetch(e, kOfxImageEffectOutputClipName, t)) return fail(e, "no output image");
    if (!src.fetch(e, kOfxImageEffectSimpleSourceClipName, t)) return fail(e, "no source image");
    if (w.x1 < dst.bounds.x1 || w.y1 < dst.bounds.y1 || w.x2 > dst.bounds.x2 || w.y2 > dst.bounds.y2)
        return fail(e, "render window outside the output image");

    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            float s[4] = {0, 0, 0, 0};
            if (src.inside(x, y)) std::memcpy(s, src.at(x, y), sizeof s);
            float* d = dst.at(x, y);
            for (int c = 0; c < 3; ++c) {
                float v = (mode == 0 ? s[c] * float(gain) : s[c] + float(gain)) + float(off[c]);
                d[c] = invert ? 1.0f - v : v;
            }
            d[3] = s[3] + float(off[3]);
        }
    return kOfxStatOK;
}

OfxStatus gainMain(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Gain", kOfxImageEffectContextFilter, kOfxBitDepthFloat,
                       kOfxImageEffectRenderFullySafe, 1);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return gainDescribeInContext(e);
    if (!std::strcmp(action, kOfxImageEffectActionRender)) return gainRender(e, in);
    return kOfxStatReplyDefault;
}

// ================================================================ Shift

OfxStatus shiftMain(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Shift", kOfxImageEffectContextFilter, kOfxBitDepthFloat,
                       kOfxImageEffectRenderInstanceSafe, 1);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectOutputClipName, kOfxImageComponentRGBA);
        defineClip(e, kOfxImageEffectSimpleSourceClipName, kOfxImageComponentRGBA);
        defineParam(e, kOfxParamTypeInteger2D, "shift");
        return kOfxStatOK;
    }
    int dx = 0, dy = 0;
    if (e && (!std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) ||
              !std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) ||
              !std::strcmp(action, kOfxImageEffectActionRender)))
        if (gParam->paramGetValue(param(e, "shift"), &dx, &dy) != kOfxStatOK) return kOfxStatFailed;

    if (!std::strcmp(action, kOfxImageEffectActionGetRegionOfDefinition)) {
        OfxImageClipHandle src;
        gEffect->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &src, nullptr);
        OfxRectD rod;
        gEffect->clipGetRegionOfDefinition(src, 0, &rod);
        rod.x1 += dx, rod.x2 += dx, rod.y1 += dy, rod.y2 += dy;
        gProp->propSetDoubleN(out, kOfxImageEffectPropRegionOfDefinition, 4, &rod.x1);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionGetRegionsOfInterest)) {
        OfxRectD roi;
        gProp->propGetDoubleN(in, kOfxImageEffectPropRegionOfInterest, 4, &roi.x1);
        roi.x1 -= dx, roi.x2 -= dx, roi.y1 -= dy, roi.y2 -= dy;
        gProp->propSetDoubleN(out, "OfxImageClipPropRoI_Source", 4, &roi.x1);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionRender)) {
        OfxTime t;
        OfxRectI w;
        renderArgs(in, t, w);
        Img src, dst;
        if (!dst.fetch(e, kOfxImageEffectOutputClipName, t) || !src.fetch(e, kOfxImageEffectSimpleSourceClipName, t))
            return fail(e, "missing image");
        for (int y = w.y1; y < w.y2; ++y)
            for (int x = w.x1; x < w.x2; ++x) {
                float* d = dst.at(x, y);
                const int sx = x - dx, sy = y - dy;
                if (src.inside(sx, sy)) std::memcpy(d, src.at(sx, sy), 4 * sizeof(float));
                else std::memset(d, 0, 4 * sizeof(float));
            }
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// ================================================================ MaskMultiply

std::atomic<int> gMaskRenders{0};

OfxStatus maskMain(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Mask Multiply", kOfxImageEffectContextGeneral, kOfxBitDepthFloat,
                       kOfxImageEffectRenderUnsafe, 0);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectOutputClipName, kOfxImageComponentRGBA);
        defineClip(e, kOfxImageEffectSimpleSourceClipName, kOfxImageComponentRGBA);
        OfxPropertySetHandle m = defineClip(e, "Mask", kOfxImageComponentAlpha);
        gProp->propSetInt(m, kOfxImageClipPropOptional, 0, 1);
        gProp->propSetInt(m, kOfxImageClipPropIsMask, 0, 1);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionRender)) {
        // Declared unsafe: the host must never run two of these at once.
        if (gMaskRenders.fetch_add(1) != 0) {
            gMaskRenders.fetch_sub(1);
            return fail(e, "rendered concurrently despite being thread unsafe");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // widen the window for overlaps
        OfxTime t;
        OfxRectI w;
        renderArgs(in, t, w);
        Img src, dst, mask;
        OfxStatus status = kOfxStatOK;
        const bool hasMask = isConnected(e, "Mask");
        if (!dst.fetch(e, kOfxImageEffectOutputClipName, t) || !src.fetch(e, kOfxImageEffectSimpleSourceClipName, t) ||
            (hasMask && !mask.fetch(e, "Mask", t))) {
            status = fail(e, "missing image");
        } else if (hasMask && mask.nch != 1) {
            status = fail(e, "mask isn't alpha-only");
        } else {
            for (int y = w.y1; y < w.y2; ++y)
                for (int x = w.x1; x < w.x2; ++x) {
                    const float m = !hasMask ? 1.0f : mask.inside(x, y) ? *mask.at(x, y) : 0.0f;
                    float* d = dst.at(x, y);
                    for (int c = 0; c < 4; ++c) d[c] = (src.inside(x, y) ? src.at(x, y)[c] : 0.0f) * m;
                }
        }
        gMaskRenders.fetch_sub(1);
        return status;
    }
    return kOfxStatReplyDefault;
}

// ================================================================ Coords

OfxStatus coordsMain(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Coords", kOfxImageEffectContextFilter, kOfxBitDepthFloat,
                       kOfxImageEffectRenderFullySafe, 1);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectOutputClipName, kOfxImageComponentRGBA);
        defineClip(e, kOfxImageEffectSimpleSourceClipName, kOfxImageComponentRGBA);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionRender)) {
        OfxTime t;
        OfxRectI w;
        renderArgs(in, t, w);
        double offset[2], size[2];
        gProp->propGetDoubleN(effectProps(e), kOfxImageEffectPropProjectOffset, 2, offset);
        gProp->propGetDoubleN(effectProps(e), kOfxImageEffectPropProjectSize, 2, size);
        Img dst;
        if (!dst.fetch(e, kOfxImageEffectOutputClipName, t)) return fail(e, "no output image");
        for (int y = w.y1; y < w.y2; ++y)
            for (int x = w.x1; x < w.x2; ++x) {
                float* d = dst.at(x, y);
                d[0] = float(x), d[1] = float(y), d[2] = float(offset[1]), d[3] = float(offset[1] + size[1]);
            }
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// ================================================================ Generator
// No inputs. Each pixel: (red of the color param, OFX x, OFX y, project
// height), so tests can check the coordinates and the project size. Offers
// General too, to check comp picks Generator.

OfxStatus generatorMain(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Generator", kOfxImageEffectContextGeneral, kOfxBitDepthFloat,
                       kOfxImageEffectRenderFullySafe, 1);
        gProp->propSetString(effectProps(e), kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGenerator);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        char* context;
        gProp->propGetString(in, kOfxImageEffectPropContext, 0, &context);
        if (std::strcmp(context, kOfxImageEffectContextGenerator)) return fail(e, "expected the Generator context");
        defineClip(e, kOfxImageEffectOutputClipName, kOfxImageComponentRGBA);
        OfxPropertySetHandle p = defineParam(e, kOfxParamTypeRGBA, "color");
        for (int i = 0; i < 4; ++i) gProp->propSetDouble(p, kOfxParamPropDefault, i, 0.5);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxImageEffectActionRender)) {
        OfxTime t;
        OfxRectI w;
        renderArgs(in, t, w);
        double rgba[4], size[2];
        gParam->paramGetValueAtTime(param(e, "color"), t, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
        gProp->propGetDoubleN(effectProps(e), kOfxImageEffectPropProjectSize, 2, size);
        Img dst;
        if (!dst.fetch(e, kOfxImageEffectOutputClipName, t)) return fail(e, "no output image");
        for (int y = w.y1; y < w.y2; ++y)
            for (int x = w.x1; x < w.x2; ++x) {
                float* d = dst.at(x, y);
                d[0] = float(rgba[0]), d[1] = float(x), d[2] = float(y), d[3] = float(size[1]);
            }
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// ================================================================ ones comp should refuse

OfxStatus transitionMain(const char* action, const void* handle, OfxPropertySetHandle, OfxPropertySetHandle) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Transition", kOfxImageEffectContextTransition, kOfxBitDepthFloat,
                       kOfxImageEffectRenderFullySafe, 1);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

OfxStatus byteOnlyMain(const char* action, const void* handle, OfxPropertySetHandle, OfxPropertySetHandle) {
    auto e = static_cast<OfxImageEffectHandle>(const_cast<void*>(handle));
    if (!std::strcmp(action, kOfxActionDescribe)) {
        describeCommon(e, "Test Byte Only", kOfxImageEffectContextFilter, kOfxBitDepthByte,
                       kOfxImageEffectRenderFullySafe, 1);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

OfxPlugin plugins[] = {
    {kOfxImageEffectPluginApi, 1, "org.comp.test.Gain", 1, 0, setHost, gainMain},
    {kOfxImageEffectPluginApi, 1, "org.comp.test.Shift", 1, 2, setHost, shiftMain},
    {kOfxImageEffectPluginApi, 1, "org.comp.test.MaskMultiply", 1, 0, setHost, maskMain},
    {kOfxImageEffectPluginApi, 1, "org.comp.test.Coords", 1, 0, setHost, coordsMain},
    {kOfxImageEffectPluginApi, 1, "org.comp.test.Generator", 1, 0, setHost, generatorMain},
    {kOfxImageEffectPluginApi, 1, "org.comp.test.ByteOnly", 1, 0, setHost, byteOnlyMain},
    {kOfxImageEffectPluginApi, 1, "org.comp.test.Transition", 1, 0, setHost, transitionMain},
};

}  // namespace

EXPORT int OfxGetNumberOfPlugins(void) { return sizeof plugins / sizeof plugins[0]; }
EXPORT OfxPlugin* OfxGetPlugin(int i) { return &plugins[i]; }
