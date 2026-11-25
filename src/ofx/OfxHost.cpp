// comp's OpenFX host: property sets, the suites plugins call, and finding,
// loading and describing plugins. How a plugin runs inside the engine is in
// OfxNode.cpp.

#include "comp/ofx/OfxHost.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <utility>

#if defined(__linux__)
#include <dlfcn.h>
#endif

#include "OfxInternal.h"  // first: the OFX headers below need ofxCore.h

#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"
#include "ofxParametricParam.h"
#include "OfxNode.h"

namespace comp::ofx {

namespace fs = std::filesystem;

// ================================================================ properties

int Property::size() const {
    switch (type) {
        case Type::Pointer: return static_cast<int>(pointers.size());
        case Type::String: return static_cast<int>(strings.size());
        case Type::Double: return static_cast<int>(doubles.size());
        case Type::Int: return static_cast<int>(ints.size());
    }
    return 0;
}

void Property::resize(int n) {
    switch (type) {
        case Type::Pointer: pointers.resize(n, nullptr); break;
        case Type::String: strings.resize(n); break;
        case Type::Double: doubles.resize(n, 0.0); break;
        case Type::Int: ints.resize(n, 0); break;
    }
}

Property& PropertySet::at(const std::string& name, Property::Type type) {
    Property& p = props_[name];
    if (p.type != type) {
        // A property set with a different type than it had: keep the size.
        const int n = p.size();
        p = Property{};
        p.type = type;
        p.resize(n);
    }
    return p;
}

const Property* PropertySet::find(const std::string& name) const {
    auto it = props_.find(name);
    return it == props_.end() ? nullptr : &it->second;
}

Property* PropertySet::find(const std::string& name) {
    auto it = props_.find(name);
    return it == props_.end() ? nullptr : &it->second;
}

namespace {
template <typename V>
void setOne(std::vector<V>& values, int index, V v) {
    if (index >= static_cast<int>(values.size())) values.resize(index + 1);
    values[index] = std::move(v);
}
}  // namespace

void PropertySet::setPointer(const std::string& n, int i, void* v) {
    std::lock_guard lock(mutex_);
    setOne(at(n, Property::Type::Pointer).pointers, i, v);
}
void PropertySet::setString(const std::string& n, int i, const std::string& v) {
    std::lock_guard lock(mutex_);
    setOne(at(n, Property::Type::String).strings, i, v);
}
void PropertySet::setDouble(const std::string& n, int i, double v) {
    std::lock_guard lock(mutex_);
    setOne(at(n, Property::Type::Double).doubles, i, v);
}
void PropertySet::setInt(const std::string& n, int i, int v) {
    std::lock_guard lock(mutex_);
    setOne(at(n, Property::Type::Int).ints, i, v);
}
void PropertySet::setStrings(const std::string& n, const std::vector<std::string>& v) {
    std::lock_guard lock(mutex_);
    at(n, Property::Type::String).strings = v;
}
void PropertySet::setDoubles(const std::string& n, const std::vector<double>& v) {
    std::lock_guard lock(mutex_);
    at(n, Property::Type::Double).doubles = v;
}
void PropertySet::setInts(const std::string& n, const std::vector<int>& v) {
    std::lock_guard lock(mutex_);
    at(n, Property::Type::Int).ints = v;
}

std::string PropertySet::getString(const std::string& n, int i, const std::string& fallback) const {
    std::lock_guard lock(mutex_);
    const Property* p = find(n);
    if (!p || p->type != Property::Type::String || i >= p->size()) return fallback;
    return p->strings[i];
}
int PropertySet::getInt(const std::string& n, int i, int fallback) const {
    std::lock_guard lock(mutex_);
    const Property* p = find(n);
    if (!p || i >= p->size()) return fallback;
    if (p->type == Property::Type::Int) return p->ints[i];
    if (p->type == Property::Type::Double) return static_cast<int>(p->doubles[i]);
    return fallback;
}
double PropertySet::getDouble(const std::string& n, int i, double fallback) const {
    std::lock_guard lock(mutex_);
    const Property* p = find(n);
    if (!p || i >= p->size()) return fallback;
    if (p->type == Property::Type::Double) return p->doubles[i];
    if (p->type == Property::Type::Int) return p->ints[i];
    return fallback;
}
std::vector<std::string> PropertySet::getStrings(const std::string& n) const {
    std::lock_guard lock(mutex_);
    const Property* p = find(n);
    return p && p->type == Property::Type::String ? p->strings : std::vector<std::string>{};
}
std::vector<double> PropertySet::getDoubles(const std::string& n) const {
    std::lock_guard lock(mutex_);
    const Property* p = find(n);
    if (!p) return {};
    if (p->type == Property::Type::Double) return p->doubles;
    if (p->type == Property::Type::Int) return {p->ints.begin(), p->ints.end()};
    return {};
}

void PropertySet::copyFrom(const PropertySet& other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    props_ = other.props_;
}

Param* ParamSet::find(const std::string& name) const {
    for (const auto& p : params)
        if (p->name == name) return p.get();
    return nullptr;
}

Clip* Effect::findClip(const std::string& name) const {
    for (const auto& c : clips)
        if (c->name == name) return c.get();
    return nullptr;
}

void Effect::copyFrom(const Effect& d) {
    PropertySet::copyFrom(d);
    plugin = d.plugin;
    params.PropertySet::copyFrom(d.params);
    params.effect = this;
    for (const auto& p : d.params.params) {
        auto copy = std::make_unique<Param>();
        copy->PropertySet::copyFrom(*p);
        copy->name = p->name;
        copy->type = p->type;
        copy->effect = this;
        params.params.push_back(std::move(copy));
    }
    for (const auto& c : d.clips) {
        auto copy = std::make_unique<Clip>();
        copy->PropertySet::copyFrom(*c);
        copy->name = c->name;
        copy->effect = this;
        clips.push_back(std::move(copy));
    }
}

// ================================================================ coordinates

OfxRectI toOfx(const Box& b, int flipY) { return {b.x0, flipY - b.y1, b.x1, flipY - b.y0}; }
Box fromOfx(const OfxRectI& r, int flipY) { return {r.x1, flipY - r.y2, r.x2, flipY - r.y1}; }
Box fromOfx(const OfxRectD& r, int flipY) {
    auto lo = [](double v) { return static_cast<int>(std::floor(std::clamp(v, -1e9, 1e9))); };
    auto hi = [](double v) { return static_cast<int>(std::ceil(std::clamp(v, -1e9, 1e9))); };
    return {lo(r.x1), flipY - hi(r.y2), hi(r.x2), flipY - lo(r.y1)};
}

// ================================================================ call context, messages

namespace {
thread_local CallContext* tlsContext = nullptr;
thread_local std::string tlsMessage;
}  // namespace

CallContext* currentContext() { return tlsContext; }
ScopedContext::ScopedContext(CallContext* c) : previous(tlsContext) { tlsContext = c; }
ScopedContext::~ScopedContext() { tlsContext = previous; }

std::string takeLastMessage() { return std::exchange(tlsMessage, std::string{}); }
void noteProblem(const std::string& message) { tlsMessage = message; }

namespace {

// ================================================================ property suite

OfxStatus propSet(OfxPropertySetHandle h, const char* name, int index, Property::Type type, auto&& assign) {
    if (!h || !name) return kOfxStatErrBadHandle;
    if (index < 0) return kOfxStatErrBadIndex;
    PropertySet* ps = PropertySet::from(h);
    std::lock_guard lock(ps->mutex());
    assign(ps->at(name, type), index);
    return kOfxStatOK;
}

OfxStatus propSetPointer(OfxPropertySetHandle h, const char* n, int i, void* v) {
    return propSet(h, n, i, Property::Type::Pointer, [&](Property& p, int i) { setOne(p.pointers, i, v); });
}
OfxStatus propSetString(OfxPropertySetHandle h, const char* n, int i, const char* v) {
    return propSet(h, n, i, Property::Type::String, [&](Property& p, int i) { setOne(p.strings, i, std::string(v ? v : "")); });
}
OfxStatus propSetDouble(OfxPropertySetHandle h, const char* n, int i, double v) {
    return propSet(h, n, i, Property::Type::Double, [&](Property& p, int i) { setOne(p.doubles, i, v); });
}
OfxStatus propSetInt(OfxPropertySetHandle h, const char* n, int i, int v) {
    return propSet(h, n, i, Property::Type::Int, [&](Property& p, int i) { setOne(p.ints, i, v); });
}

template <typename T, typename Assign>
OfxStatus propSetN(OfxPropertySetHandle h, const char* name, int count, const T* values, Property::Type type,
                   Assign assign) {
    if (!h || !name) return kOfxStatErrBadHandle;
    if (count < 0 || (count > 0 && !values)) return kOfxStatErrValue;
    PropertySet* ps = PropertySet::from(h);
    std::lock_guard lock(ps->mutex());
    Property& p = ps->at(name, type);
    p.resize(count);
    for (int i = 0; i < count; ++i) assign(p, i, values[i]);
    return kOfxStatOK;
}

OfxStatus propSetPointerN(OfxPropertySetHandle h, const char* n, int c, void* const* v) {
    return propSetN(h, n, c, v, Property::Type::Pointer, [](Property& p, int i, void* x) { p.pointers[i] = x; });
}
OfxStatus propSetStringN(OfxPropertySetHandle h, const char* n, int c, const char* const* v) {
    return propSetN(h, n, c, v, Property::Type::String,
                    [](Property& p, int i, const char* x) { p.strings[i] = x ? x : ""; });
}
OfxStatus propSetDoubleN(OfxPropertySetHandle h, const char* n, int c, const double* v) {
    return propSetN(h, n, c, v, Property::Type::Double, [](Property& p, int i, double x) { p.doubles[i] = x; });
}
OfxStatus propSetIntN(OfxPropertySetHandle h, const char* n, int c, const int* v) {
    return propSetN(h, n, c, v, Property::Type::Int, [](Property& p, int i, int x) { p.ints[i] = x; });
}

// Reads `count` values starting at `first`. Int and double convert to each
// other (plenty of plugins mix them up); other mismatches are errors.
template <typename T>
OfxStatus propGet(OfxPropertySetHandle h, const char* name, int first, int count, T* out) {
    if (!h || !name) return kOfxStatErrBadHandle;
    if (!out) return kOfxStatErrValue;
    PropertySet* ps = PropertySet::from(h);
    std::lock_guard lock(ps->mutex());
    const Property* p = ps->find(name);
    if (!p) return kOfxStatErrUnknown;
    if (first < 0 || first + count > p->size()) return kOfxStatErrBadIndex;
    for (int i = 0; i < count; ++i) {
        const int k = first + i;
        if constexpr (std::is_same_v<T, void*>) {
            if (p->type != Property::Type::Pointer) return kOfxStatErrValue;
            out[i] = p->pointers[k];
        } else if constexpr (std::is_same_v<T, char*>) {
            if (p->type != Property::Type::String) return kOfxStatErrValue;
            out[i] = const_cast<char*>(p->strings[k].c_str());
        } else {
            if (p->type == Property::Type::Double) out[i] = static_cast<T>(p->doubles[k]);
            else if (p->type == Property::Type::Int) out[i] = static_cast<T>(p->ints[k]);
            else return kOfxStatErrValue;
        }
    }
    return kOfxStatOK;
}

OfxStatus propGetPointer(OfxPropertySetHandle h, const char* n, int i, void** v) { return propGet(h, n, i, 1, v); }
OfxStatus propGetString(OfxPropertySetHandle h, const char* n, int i, char** v) { return propGet(h, n, i, 1, v); }
OfxStatus propGetDouble(OfxPropertySetHandle h, const char* n, int i, double* v) { return propGet(h, n, i, 1, v); }
OfxStatus propGetInt(OfxPropertySetHandle h, const char* n, int i, int* v) { return propGet(h, n, i, 1, v); }
OfxStatus propGetPointerN(OfxPropertySetHandle h, const char* n, int c, void** v) { return propGet(h, n, 0, c, v); }
OfxStatus propGetStringN(OfxPropertySetHandle h, const char* n, int c, char** v) { return propGet(h, n, 0, c, v); }
OfxStatus propGetDoubleN(OfxPropertySetHandle h, const char* n, int c, double* v) { return propGet(h, n, 0, c, v); }
OfxStatus propGetIntN(OfxPropertySetHandle h, const char* n, int c, int* v) { return propGet(h, n, 0, c, v); }

OfxStatus propReset(OfxPropertySetHandle h, const char* name) {
    if (!h || !name) return kOfxStatErrBadHandle;
    PropertySet* ps = PropertySet::from(h);
    std::lock_guard lock(ps->mutex());
    Property* p = ps->find(name);
    if (!p) return kOfxStatErrUnknown;
    const int n = p->size();
    const auto type = p->type;
    *p = Property{};
    p->type = type;
    p->resize(n);
    return kOfxStatOK;
}

OfxStatus propGetDimension(OfxPropertySetHandle h, const char* name, int* count) {
    if (!h || !name || !count) return kOfxStatErrBadHandle;
    PropertySet* ps = PropertySet::from(h);
    std::lock_guard lock(ps->mutex());
    const Property* p = ps->find(name);
    if (!p) return kOfxStatErrUnknown;
    *count = p->size();
    return kOfxStatOK;
}

const OfxPropertySuiteV1 propertySuite{
    propSetPointer,  propSetString,  propSetDouble,  propSetInt,  propSetPointerN,  propSetStringN,
    propSetDoubleN,  propSetIntN,    propGetPointer, propGetString, propGetDouble, propGetInt,
    propGetPointerN, propGetStringN, propGetDoubleN, propGetIntN, propReset,      propGetDimension,
};

// ================================================================ image effect suite

OfxStatus getPropertySet(OfxImageEffectHandle e, OfxPropertySetHandle* out) {
    if (!e || !out) return kOfxStatErrBadHandle;
    *out = Effect::from(e)->PropertySet::handle();
    return kOfxStatOK;
}

OfxStatus getParamSet(OfxImageEffectHandle e, OfxParamSetHandle* out) {
    if (!e || !out) return kOfxStatErrBadHandle;
    *out = reinterpret_cast<OfxParamSetHandle>(&Effect::from(e)->params);
    return kOfxStatOK;
}

OfxStatus clipDefine(OfxImageEffectHandle e, const char* name, OfxPropertySetHandle* out) {
    if (!e || !name) return kOfxStatErrBadHandle;
    Effect* effect = Effect::from(e);
    if (effect->node) return kOfxStatErrUnsupported;  // only while describing
    if (effect->findClip(name)) return kOfxStatErrExists;
    auto clip = std::make_unique<Clip>();
    clip->name = name;
    clip->effect = effect;
    clip->setString(kOfxPropType, 0, kOfxTypeClip);
    clip->setString(kOfxPropName, 0, name);
    clip->setString(kOfxPropLabel, 0, name);
    clip->setStrings(kOfxImageEffectPropSupportedComponents, {});
    clip->setInt(kOfxImageClipPropOptional, 0, 0);
    clip->setInt(kOfxImageClipPropIsMask, 0, 0);
    clip->setInt(kOfxImageEffectPropSupportsTiles, 0, 1);
    clip->setInt(kOfxImageEffectPropTemporalClipAccess, 0, 0);
    clip->setString(kOfxImageClipPropFieldExtraction, 0, kOfxImageFieldDoubled);
    if (out) *out = clip->handle();
    effect->clips.push_back(std::move(clip));
    return kOfxStatOK;
}

OfxStatus clipGetHandle(OfxImageEffectHandle e, const char* name, OfxImageClipHandle* clip,
                        OfxPropertySetHandle* props) {
    if (!e || !name || !clip) return kOfxStatErrBadHandle;
    Clip* c = Effect::from(e)->findClip(name);
    if (!c) return kOfxStatErrUnknown;
    *clip = reinterpret_cast<OfxImageClipHandle>(c);
    if (props) *props = c->handle();
    return kOfxStatOK;
}

OfxStatus clipGetPropertySet(OfxImageClipHandle clip, OfxPropertySetHandle* out) {
    if (!clip || !out) return kOfxStatErrBadHandle;
    *out = reinterpret_cast<Clip*>(clip)->handle();
    return kOfxStatOK;
}

OfxStatus clipGetImage(OfxImageClipHandle clip, OfxTime time, const OfxRectD* region, OfxPropertySetHandle* out) {
    if (!clip || !out) return kOfxStatErrBadHandle;
    return OfxNode::clipImage(*reinterpret_cast<Clip*>(clip), time, region, out);
}

OfxStatus clipReleaseImage(OfxPropertySetHandle image) {
    if (!image) return kOfxStatErrBadHandle;
    delete PropertySet::from(image);
    return kOfxStatOK;
}

OfxStatus clipGetRegionOfDefinition(OfxImageClipHandle clip, OfxTime time, OfxRectD* bounds) {
    if (!clip || !bounds) return kOfxStatErrBadHandle;
    return OfxNode::clipRegionOfDefinition(*reinterpret_cast<Clip*>(clip), time, bounds);
}

int abortRender(OfxImageEffectHandle) { return 0; }

struct ImageMemory {
    void* data;
};

OfxStatus imageMemoryAlloc(OfxImageEffectHandle, size_t nBytes, OfxImageMemoryHandle* out) {
    if (!out) return kOfxStatErrBadHandle;
    void* data = std::malloc(nBytes ? nBytes : 1);
    if (!data) return kOfxStatErrMemory;
    *out = reinterpret_cast<OfxImageMemoryHandle>(new ImageMemory{data});
    return kOfxStatOK;
}
OfxStatus imageMemoryFree(OfxImageMemoryHandle h) {
    if (!h) return kOfxStatErrBadHandle;
    auto* m = reinterpret_cast<ImageMemory*>(h);
    std::free(m->data);
    delete m;
    return kOfxStatOK;
}
OfxStatus imageMemoryLock(OfxImageMemoryHandle h, void** out) {
    if (!h || !out) return kOfxStatErrBadHandle;
    *out = reinterpret_cast<ImageMemory*>(h)->data;
    return kOfxStatOK;
}
OfxStatus imageMemoryUnlock(OfxImageMemoryHandle h) { return h ? kOfxStatOK : kOfxStatErrBadHandle; }

const OfxImageEffectSuiteV1 imageEffectSuite{
    getPropertySet,   getParamSet,     clipDefine,       clipGetHandle,    clipGetPropertySet,
    clipGetImage,     clipReleaseImage, clipGetRegionOfDefinition, abortRender, imageMemoryAlloc,
    imageMemoryFree,  imageMemoryLock, imageMemoryUnlock,
};

// ================================================================ parameter suite

Param* asParam(OfxParamHandle h) { return reinterpret_cast<Param*>(h); }
ParamSet* asParamSet(OfxParamSetHandle h) { return reinterpret_cast<ParamSet*>(h); }

// Values per parameter type, and their defaults.
int dimensionOf(const std::string& type) {
    if (type == kOfxParamTypeDouble2D || type == kOfxParamTypeInteger2D) return 2;
    if (type == kOfxParamTypeDouble3D || type == kOfxParamTypeInteger3D || type == kOfxParamTypeRGB) return 3;
    if (type == kOfxParamTypeRGBA) return 4;
    return 1;
}
bool isIntType(const std::string& t) {
    return t == kOfxParamTypeInteger || t == kOfxParamTypeBoolean || t == kOfxParamTypeChoice ||
           t == kOfxParamTypeInteger2D || t == kOfxParamTypeInteger3D;
}
bool isStringType(const std::string& t) { return t == kOfxParamTypeString || t == kOfxParamTypeCustom; }
bool hasValue(const std::string& t) {
    return t != kOfxParamTypeGroup && t != kOfxParamTypePage && t != kOfxParamTypePushButton &&
           t != kOfxParamTypeParametric;
}

OfxStatus paramDefine(OfxParamSetHandle ps, const char* type, const char* name, OfxPropertySetHandle* out) {
    if (!ps || !type || !name) return kOfxStatErrBadHandle;
    ParamSet* set = asParamSet(ps);
    if (set->effect && set->effect->node) return kOfxStatErrUnsupported;  // only while describing
    if (set->find(name)) return kOfxStatErrExists;
    auto p = std::make_unique<Param>();
    p->name = name;
    p->type = type;
    p->effect = set->effect;
    p->setString(kOfxPropType, 0, kOfxTypeParameter);
    p->setString(kOfxParamPropType, 0, type);
    p->setString(kOfxPropName, 0, name);
    p->setString(kOfxPropLabel, 0, name);
    p->setString(kOfxParamPropScriptName, 0, name);
    p->setString(kOfxParamPropHint, 0, "");
    p->setInt(kOfxParamPropSecret, 0, 0);
    p->setInt(kOfxParamPropEnabled, 0, 1);
    p->setInt(kOfxParamPropAnimates, 0, 1);
    p->setInt(kOfxParamPropEvaluateOnChange, 0, 1);
    p->setString(kOfxParamPropParent, 0, "");
    if (hasValue(type)) {
        if (isStringType(type)) p->setString(kOfxParamPropDefault, 0, "");
        else if (isIntType(type)) p->setInts(kOfxParamPropDefault, std::vector<int>(dimensionOf(type), 0));
        else p->setDoubles(kOfxParamPropDefault, std::vector<double>(dimensionOf(type), 0.0));
    }
    if (std::string(type) == kOfxParamTypeChoice) p->setStrings(kOfxParamPropChoiceOption, {});
    if (std::string(type) == kOfxParamTypeDouble || std::string(type) == kOfxParamTypeDouble2D ||
        std::string(type) == kOfxParamTypeDouble3D)
        p->setString(kOfxParamPropDoubleType, 0, kOfxParamDoubleTypePlain);
    if (out) *out = p->handle();
    set->params.push_back(std::move(p));
    return kOfxStatOK;
}

OfxStatus paramGetHandle(OfxParamSetHandle ps, const char* name, OfxParamHandle* param, OfxPropertySetHandle* props) {
    if (!ps || !name || !param) return kOfxStatErrBadHandle;
    Param* p = asParamSet(ps)->find(name);
    if (!p) return kOfxStatErrUnknown;
    *param = reinterpret_cast<OfxParamHandle>(p);
    if (props) *props = p->handle();
    return kOfxStatOK;
}

OfxStatus paramSetGetPropertySet(OfxParamSetHandle ps, OfxPropertySetHandle* out) {
    if (!ps || !out) return kOfxStatErrBadHandle;
    *out = asParamSet(ps)->handle();
    return kOfxStatOK;
}

OfxStatus paramGetPropertySet(OfxParamHandle p, OfxPropertySetHandle* out) {
    if (!p || !out) return kOfxStatErrBadHandle;
    *out = asParam(p)->handle();
    return kOfxStatOK;
}

// Writes a parameter's value to the pointers in `ap`, as the parameter
// suite's variadic getters do. `scale` multiplies numbers (for integrals).
OfxStatus writeValue(Param& p, va_list ap, double scale = 1.0) {
    if (!hasValue(p.type)) return kOfxStatErrUnsupported;
    std::vector<double> numbers;
    std::string text;
    if (!OfxNode::paramValue(p, numbers, text)) return kOfxStatErrValue;
    if (isStringType(p.type)) {
        // The pointer must stay valid after we return: keep the text on the
        // parameter, per thread, until the next read.
        thread_local std::map<const Param*, std::string> kept;
        std::string& s = kept[&p];
        s = std::move(text);
        *va_arg(ap, const char**) = s.c_str();
        return kOfxStatOK;
    }
    for (double v : numbers) {
        if (isIntType(p.type)) *va_arg(ap, int*) = static_cast<int>(std::lround(v * scale));
        else *va_arg(ap, double*) = v * scale;
    }
    return kOfxStatOK;
}

OfxStatus paramGetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    const OfxStatus s = writeValue(*asParam(h), ap);
    va_end(ap);
    return s;
}

OfxStatus paramGetValueAtTime(OfxParamHandle h, OfxTime time, ...) {  // no animation: same at every time
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus s = writeValue(*asParam(h), ap);
    va_end(ap);
    return s;
}

OfxStatus paramGetDerivative(OfxParamHandle h, OfxTime time, ...) {  // constant: zero
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus s = writeValue(*asParam(h), ap, 0.0);
    va_end(ap);
    return s;
}

OfxStatus paramGetIntegral(OfxParamHandle h, OfxTime t1, OfxTime t2, ...) {  // constant: value * duration
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, t2);
    const OfxStatus s = writeValue(*asParam(h), ap, t2 - t1);
    va_end(ap);
    return s;
}

// Values plugins set themselves are read from `ap` by type and kept on the
// node, below the user's values (see OfxNode::setValueFromPlugin).
OfxStatus readAndSet(Param& p, va_list ap) {
    if (!hasValue(p.type)) return kOfxStatErrUnsupported;
    ParamValue value;
    const int n = dimensionOf(p.type);
    if (isStringType(p.type)) {
        const char* s = va_arg(ap, const char*);
        value = std::string(s ? s : "");
    } else if (p.type == kOfxParamTypeBoolean) {
        value = va_arg(ap, int) != 0;
    } else if (n == 1) {
        if (isIntType(p.type)) value = va_arg(ap, int);
        else value = va_arg(ap, double);
    } else {
        std::array<double, 4> v{0, 0, 0, 0};
        for (int i = 0; i < n; ++i) v[i] = isIntType(p.type) ? va_arg(ap, int) : va_arg(ap, double);
        if (n == 2) value = Vec2{v[0], v[1]};
        else value = Color{v[0], v[1], v[2], v[3]};
    }
    OfxNode::setValueFromPlugin(p, std::move(value));
    return kOfxStatOK;
}

OfxStatus paramSetValue(OfxParamHandle h, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, h);
    const OfxStatus s = readAndSet(*asParam(h), ap);
    va_end(ap);
    return s;
}

OfxStatus paramSetValueAtTime(OfxParamHandle h, OfxTime time, ...) {
    if (!h) return kOfxStatErrBadHandle;
    va_list ap;
    va_start(ap, time);
    const OfxStatus s = readAndSet(*asParam(h), ap);
    va_end(ap);
    return s;
}

OfxStatus paramGetNumKeys(OfxParamHandle h, unsigned int* n) {
    if (!h || !n) return kOfxStatErrBadHandle;
    *n = 0;
    return kOfxStatOK;
}
OfxStatus paramGetKeyTime(OfxParamHandle, unsigned int, OfxTime*) { return kOfxStatErrBadIndex; }
OfxStatus paramGetKeyIndex(OfxParamHandle, OfxTime, int, int*) { return kOfxStatFailed; }
OfxStatus paramDeleteKey(OfxParamHandle, OfxTime) { return kOfxStatErrBadIndex; }
OfxStatus paramDeleteAllKeys(OfxParamHandle) { return kOfxStatOK; }
OfxStatus paramCopy(OfxParamHandle, OfxParamHandle, OfxTime, const OfxRangeD*) { return kOfxStatErrUnsupported; }
OfxStatus paramEditBegin(OfxParamSetHandle, const char*) { return kOfxStatOK; }
OfxStatus paramEditEnd(OfxParamSetHandle) { return kOfxStatOK; }

const OfxParameterSuiteV1 parameterSuite{
    paramDefine,     paramGetHandle,   paramSetGetPropertySet, paramGetPropertySet, paramGetValue,
    paramGetValueAtTime, paramGetDerivative, paramGetIntegral, paramSetValue, paramSetValueAtTime,
    paramGetNumKeys, paramGetKeyTime,  paramGetKeyIndex,       paramDeleteKey,      paramDeleteAllKeys,
    paramCopy,       paramEditBegin,   paramEditEnd,
};

// ================================================================ memory suite

OfxStatus memoryAlloc(void*, size_t nBytes, void** out) {
    if (!out) return kOfxStatErrBadHandle;
    *out = std::malloc(nBytes ? nBytes : 1);
    return *out ? kOfxStatOK : kOfxStatErrMemory;
}
OfxStatus memoryFree(void* data) {
    std::free(data);
    return kOfxStatOK;
}
const OfxMemorySuiteV1 memorySuite{memoryAlloc, memoryFree};

// ================================================================ multi-thread suite
//
// comp already renders tiles on all cores, so a plugin's own threading
// would only compete with that. We report one CPU, and run any threads a
// plugin asks for one after another on the calling thread.

thread_local unsigned int tlsThreadIndex = 0;
thread_local bool tlsSpawned = false;

OfxStatus multiThread(OfxThreadFunctionV1 func, unsigned int nThreads, void* arg) {
    if (!func) return kOfxStatErrBadHandle;
    if (nThreads == 0) nThreads = 1;
    const auto savedIndex = tlsThreadIndex;
    const auto savedSpawned = tlsSpawned;
    tlsSpawned = true;
    for (unsigned int i = 0; i < nThreads; ++i) {
        tlsThreadIndex = i;
        func(i, nThreads, arg);
    }
    tlsThreadIndex = savedIndex;
    tlsSpawned = savedSpawned;
    return kOfxStatOK;
}
OfxStatus multiThreadNumCPUs(unsigned int* n) {
    if (!n) return kOfxStatErrBadHandle;
    *n = 1;
    return kOfxStatOK;
}
OfxStatus multiThreadIndex(unsigned int* i) {
    if (!i) return kOfxStatErrBadHandle;
    *i = tlsThreadIndex;
    return kOfxStatOK;
}
int multiThreadIsSpawnedThread() { return tlsSpawned ? 1 : 0; }

OfxStatus mutexCreate(OfxMutexHandle* out, int lockCount) {
    if (!out) return kOfxStatErrBadHandle;
    auto* m = new std::recursive_mutex;
    for (int i = 0; i < lockCount; ++i) m->lock();
    *out = reinterpret_cast<OfxMutexHandle>(m);
    return kOfxStatOK;
}
OfxStatus mutexDestroy(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    delete reinterpret_cast<std::recursive_mutex*>(m);
    return kOfxStatOK;
}
OfxStatus mutexLock(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    reinterpret_cast<std::recursive_mutex*>(m)->lock();
    return kOfxStatOK;
}
OfxStatus mutexUnLock(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    reinterpret_cast<std::recursive_mutex*>(m)->unlock();
    return kOfxStatOK;
}
OfxStatus mutexTryLock(const OfxMutexHandle m) {
    if (!m) return kOfxStatErrBadHandle;
    return reinterpret_cast<std::recursive_mutex*>(m)->try_lock() ? kOfxStatOK : kOfxStatFailed;
}

const OfxMultiThreadSuiteV1 multiThreadSuite{
    multiThread, multiThreadNumCPUs, multiThreadIndex, multiThreadIsSpawnedThread,
    mutexCreate, mutexDestroy,       mutexLock,        mutexUnLock, mutexTryLock,
};

// ================================================================ message suite
//
// Errors are kept (per thread) so the failing action can report them;
// warnings and plain messages go to stderr; questions get the default answer.

OfxStatus postMessage(const char* type, const char* format, va_list ap) {
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, format ? format : "", ap);
    const std::string t = type ? type : "";
    if (t == kOfxMessageError || t == kOfxMessageFatal) {
        tlsMessage = buf;
    } else if (t == kOfxMessageWarning || t == kOfxMessageMessage) {
        std::fprintf(stderr, "OFX plugin: %s\n", buf);
    } else if (t == kOfxMessageQuestion) {
        return kOfxStatReplyDefault;
    }
    return kOfxStatOK;
}

OfxStatus message(void*, const char* type, const char*, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const OfxStatus s = postMessage(type, format, ap);
    va_end(ap);
    return s;
}
OfxStatus setPersistentMessage(void*, const char* type, const char*, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    const OfxStatus s = postMessage(type, format, ap);
    va_end(ap);
    return s;
}
OfxStatus clearPersistentMessage(void*) { return kOfxStatOK; }

const OfxMessageSuiteV1 messageSuiteV1{message};
const OfxMessageSuiteV2 messageSuiteV2{message, setPersistentMessage, clearPersistentMessage};

// ================================================================ host

const void* fetchSuite(OfxPropertySetHandle, const char* name, int version) {
    if (!name) return nullptr;
    const std::string n = name;
    if (n == kOfxPropertySuite && version == 1) return &propertySuite;
    if (n == kOfxImageEffectSuite && version == 1) return &imageEffectSuite;
    if (n == kOfxParameterSuite && version == 1) return &parameterSuite;
    if (n == kOfxMemorySuite && version == 1) return &memorySuite;
    if (n == kOfxMultiThreadSuite && version == 1) return &multiThreadSuite;
    if (n == kOfxMessageSuite && version == 1) return &messageSuiteV1;
    if (n == kOfxMessageSuite && version == 2) return &messageSuiteV2;
    return nullptr;  // interacts, progress, timeline, parametric params...: not hosted
}

}  // namespace

OfxHost* host() {
    static PropertySet props;
    static const bool filled = [] {
        PropertySet& p = props;
        p.setString(kOfxPropType, 0, kOfxTypeImageEffectHost);
        p.setString(kOfxPropName, 0, "org.comp.host");
        p.setString(kOfxPropLabel, 0, "comp");
        p.setInts(kOfxPropAPIVersion, {1, 4});
        p.setInts(kOfxPropVersion, {0, 1, 0});
        p.setString(kOfxPropVersionLabel, 0, "0.1.0");
        p.setInt(kOfxImageEffectHostPropIsBackground, 0, 1);
        p.setInt(kOfxImageEffectPropSupportsOverlays, 0, 0);
        p.setInt(kOfxImageEffectPropSupportsMultiResolution, 0, 0);
        p.setInt(kOfxImageEffectPropSupportsTiles, 0, 1);
        p.setInt(kOfxImageEffectPropTemporalClipAccess, 0, 0);
        p.setStrings(kOfxImageEffectPropSupportedComponents, {kOfxImageComponentRGBA, kOfxImageComponentAlpha});
        p.setStrings(kOfxImageEffectPropSupportedContexts,
                     {kOfxImageEffectContextFilter, kOfxImageEffectContextGenerator, kOfxImageEffectContextGeneral});
        p.setStrings(kOfxImageEffectPropSupportedPixelDepths, {kOfxBitDepthFloat});
        p.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
        p.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
        p.setInt(kOfxImageEffectPropSetableFrameRate, 0, 0);
        p.setInt(kOfxImageEffectPropSetableFielding, 0, 0);
        p.setInt(kOfxParamHostPropSupportsCustomInteract, 0, 0);
        p.setInt(kOfxParamHostPropSupportsStringAnimation, 0, 0);
        p.setInt(kOfxParamHostPropSupportsChoiceAnimation, 0, 0);
        p.setInt(kOfxParamHostPropSupportsBooleanAnimation, 0, 0);
        p.setInt(kOfxParamHostPropSupportsCustomAnimation, 0, 0);
        p.setInt(kOfxParamHostPropMaxParameters, 0, -1);
        p.setInt(kOfxParamHostPropMaxPages, 0, 0);
        p.setInts(kOfxParamHostPropPageRowColumnCount, {0, 0});
        p.setInt(kOfxParamHostPropSupportsParametricAnimation, 0, 0);
        return true;
    }();
    (void)filled;
    static OfxHost h{props.handle(), fetchSuite};
    return &h;
}

// ================================================================ plugins

struct Binary {
    std::string path;
    void* handle = nullptr;
};

namespace {

struct Registry {
    std::vector<std::unique_ptr<Binary>> binaries;
    std::vector<std::unique_ptr<Plugin>> owned;
    std::vector<Plugin*> plugins;  // identifier order
    std::vector<std::string> problems;
};

const char* bundleArch() {
#if defined(__linux__) && (defined(__x86_64__) || defined(_M_X64))
    return "Linux-x86-64";
#elif defined(__linux__) && (defined(__aarch64__))
    return "Linux-arm-64";
#elif defined(__linux__)
    return "Linux-x86";
#else
    return nullptr;
#endif
}

std::vector<fs::path> searchPaths() {
    std::vector<fs::path> paths;
    if (const char* env = std::getenv("OFX_PLUGIN_PATH")) {
        std::stringstream ss(env);
        std::string item;
        while (std::getline(ss, item, ':'))
            if (!item.empty()) paths.emplace_back(item);
    }
    paths.emplace_back("/usr/OFX/Plugins");
    return paths;
}

void loadBundle(Registry& reg, const fs::path& bundle) {
#if defined(__linux__)
    const std::string base = bundle.stem().string();  // "Foo.ofx" from "Foo.ofx.bundle"
    const fs::path binary = bundle / "Contents" / bundleArch() / base;
    std::error_code ec;
    if (!fs::exists(binary, ec)) {
        reg.problems.push_back(bundle.string() + ": no " + bundleArch() + " binary (" + binary.string() + ")");
        return;
    }
    void* handle = dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        reg.problems.push_back(bundle.string() + ": cannot load: " + dlerror());
        return;
    }
    using GetCount = int (*)();
    using GetPlugin = OfxPlugin* (*)(int);
    auto getCount = reinterpret_cast<GetCount>(dlsym(handle, "OfxGetNumberOfPlugins"));
    auto getPlugin = reinterpret_cast<GetPlugin>(dlsym(handle, "OfxGetPlugin"));
    if (!getCount || !getPlugin) {
        reg.problems.push_back(bundle.string() + ": not an OFX plugin (no OfxGetNumberOfPlugins/OfxGetPlugin)");
        dlclose(handle);
        return;
    }
    auto bin = std::make_unique<Binary>();
    bin->path = binary.string();
    bin->handle = handle;  // kept loaded for the life of the program

    const int count = getCount();
    for (int i = 0; i < count; ++i) {
        OfxPlugin* api = getPlugin(i);
        if (!api || !api->pluginApi || !api->pluginIdentifier) continue;
        if (std::strcmp(api->pluginApi, kOfxImageEffectPluginApi) != 0) continue;  // not an image effect
        if (api->apiVersion != 1) {
            reg.problems.push_back(bundle.string() + ": " + api->pluginIdentifier + ": unsupported API version " +
                                   std::to_string(api->apiVersion));
            continue;
        }
        // The same identifier in several bundles: keep the newest version.
        Plugin* existing = nullptr;
        for (Plugin* p : reg.plugins)
            if (p->identifier == api->pluginIdentifier) existing = p;
        if (existing) {
            const bool newer = api->pluginVersionMajor > existing->api->pluginVersionMajor ||
                               (api->pluginVersionMajor == existing->api->pluginVersionMajor &&
                                api->pluginVersionMinor > existing->api->pluginVersionMinor);
            if (!newer) continue;
            existing->api = api;
            existing->binary = bin.get();
            existing->bundlePath = bundle.string();
            continue;
        }
        auto plugin = std::make_unique<Plugin>();
        plugin->api = api;
        plugin->binary = bin.get();
        plugin->identifier = api->pluginIdentifier;
        plugin->bundlePath = bundle.string();
        reg.plugins.push_back(plugin.get());
        reg.owned.push_back(std::move(plugin));
    }
    reg.binaries.push_back(std::move(bin));
#else
    (void)reg;
    (void)bundle;
#endif
}

Registry& registry() {
    static Registry reg = [] {
        Registry r;
#if !defined(__linux__)
        r.problems.push_back("OFX plugins are only supported on Linux so far");
        return r;
#endif
        for (const fs::path& dir : searchPaths()) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;
            for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                const fs::path& p = it->path();
                if (it->is_directory(ec) && p.extension() == ".bundle" && p.stem().extension() == ".ofx") {
                    loadBundle(r, p);
                    it.disable_recursion_pending();  // don't look for bundles inside bundles
                }
            }
        }
        std::sort(r.plugins.begin(), r.plugins.end(),
                  [](const Plugin* a, const Plugin* b) { return a->identifier < b->identifier; });
        return r;
    }();
    return reg;
}

}  // namespace

std::vector<Plugin*>& allPlugins() { return registry().plugins; }

Plugin* findPlugin(const std::string& identifier) {
    for (Plugin* p : allPlugins())
        if (p->identifier == identifier) return p;
    return nullptr;
}

OfxStatus Plugin::call(const char* action, const void* handle, PropertySet* inArgs, PropertySet* outArgs,
                       const std::string& who) const {
    takeLastMessage();  // forget anything left over
    OfxStatus status;
    try {
        status = api->mainEntry(action, handle, inArgs ? inArgs->handle() : nullptr,
                                outArgs ? outArgs->handle() : nullptr);
    } catch (...) {  // C++ plugins sometimes let exceptions escape
        throw Error(who + ": " + action + " threw an exception inside the plugin");
    }
    if (status == kOfxStatOK || status == kOfxStatReplyDefault || status == kOfxStatReplyYes ||
        status == kOfxStatReplyNo)
        return status;
    std::string msg = takeLastMessage();
    std::string what;
    switch (status) {
        case kOfxStatFailed: what = "failed"; break;
        case kOfxStatErrMissingHostFeature: what = "needs a host feature comp doesn't support yet"; break;
        case kOfxStatErrUnsupported: what = "is unsupported"; break;
        case kOfxStatErrMemory: what = "ran out of memory"; break;
        case kOfxStatErrImageFormat: what = "doesn't support this image format"; break;
        case kOfxStatErrValue: what = "got a bad value"; break;
        default: what = "failed with status " + std::to_string(status); break;
    }
    throw Error(who + ": " + action + " " + what + (msg.empty() ? "" : ": " + msg));
}

void Plugin::describe() {
    std::lock_guard lock(describeMutex);
    if (described) {
        if (!error.empty()) throw Error(error);
        return;
    }
    described = true;
    const std::string who = "OFX plugin " + identifier;
    try {
        api->setHost(host());
        call(kOfxActionLoad, nullptr, nullptr, nullptr, who);

        descriptor = std::make_unique<Effect>();
        Effect& d = *descriptor;
        d.plugin = this;
        d.params.effect = &d;
        d.setString(kOfxPropType, 0, kOfxTypeImageEffect);
        d.setString(kOfxPropLabel, 0, identifier);
        d.setString(kOfxImageEffectPluginPropGrouping, 0, "");
        d.setStrings(kOfxImageEffectPropSupportedContexts, {});
        d.setStrings(kOfxImageEffectPropSupportedPixelDepths, {});
        d.setString(kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderInstanceSafe);
        d.setInt(kOfxImageEffectPluginPropSingleInstance, 0, 0);
        d.setInt(kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
        d.setInt(kOfxImageEffectPropSupportsMultiResolution, 0, 1);
        d.setInt(kOfxImageEffectPropSupportsTiles, 0, 1);
        d.setInt(kOfxImageEffectPropTemporalClipAccess, 0, 0);
        d.setInt(kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
        d.setInt(kOfxImageEffectPropSupportsMultipleClipPARs, 0, 0);
        d.setInt(kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);
        d.setStrings(kOfxImageEffectPropClipPreferencesSlaveParam, {});
        d.setString(kOfxPluginPropFilePath, 0, bundlePath);
        call(kOfxActionDescribe, d.handle(), nullptr, nullptr, who);

        const auto contexts = d.getStrings(kOfxImageEffectPropSupportedContexts);
        auto has = [](const std::vector<std::string>& v, const char* s) {
            return std::find(v.begin(), v.end(), s) != v.end();
        };
        // The most specific first: a plugin offering Generator as well as
        // General is a generator at heart.
        if (has(contexts, kOfxImageEffectContextFilter)) context = kOfxImageEffectContextFilter;
        else if (has(contexts, kOfxImageEffectContextGenerator)) context = kOfxImageEffectContextGenerator;
        else if (has(contexts, kOfxImageEffectContextGeneral)) context = kOfxImageEffectContextGeneral;
        else {
            std::string list;
            for (const auto& c : contexts) list += (list.empty() ? "" : ", ") + c;
            throw Error(who + " has no context comp can host yet (Filter, Generator or General); it offers: " +
                        (list.empty() ? "none" : list));
        }
        if (!has(d.getStrings(kOfxImageEffectPropSupportedPixelDepths), kOfxBitDepthFloat))
            throw Error(who + " doesn't support float images, the only kind comp uses");

        contextDescriptor = std::make_unique<Effect>();
        Effect& cd = *contextDescriptor;
        cd.PropertySet::copyFrom(d);
        cd.plugin = this;
        cd.params.effect = &cd;
        PropertySet in;
        in.setString(kOfxImageEffectPropContext, 0, context);
        call(kOfxImageEffectActionDescribeInContext, cd.handle(), &in, nullptr, who);

        Clip* output = cd.findClip(kOfxImageEffectOutputClipName);
        if (!output) throw Error(who + " defined no Output clip");
        if (!has(output->getStrings(kOfxImageEffectPropSupportedComponents), kOfxImageComponentRGBA))
            throw Error(who + ": its output doesn't support RGBA images");
        for (const auto& c : cd.clips) {
            if (c.get() == output) continue;
            const auto comps = c->getStrings(kOfxImageEffectPropSupportedComponents);
            if (!has(comps, kOfxImageComponentRGBA) && !has(comps, kOfxImageComponentAlpha))
                throw Error(who + ": clip '" + c->name + "' takes neither RGBA nor Alpha images");
        }
    } catch (const Error& e) {
        error = e.what();
        throw;
    }
}

// ================================================================ public API

std::vector<PluginInfo> plugins() {
    std::vector<PluginInfo> out;
    for (const Plugin* p : allPlugins())
        out.push_back({p->identifier, p->bundlePath, static_cast<int>(p->api->pluginVersionMajor),
                       static_cast<int>(p->api->pluginVersionMinor)});
    return out;
}

std::vector<std::string> scanProblems() { return registry().problems; }

std::unique_ptr<Node> createNode(const std::string& identifier, const std::string& name) {
    Plugin* plugin = findPlugin(identifier);
    if (!plugin) throw Error("no OFX plugin '" + identifier + "' (searched $OFX_PLUGIN_PATH and /usr/OFX/Plugins)");
    plugin->describe();
    return std::make_unique<OfxNode>(name, *plugin);
}

}  // namespace comp::ofx
