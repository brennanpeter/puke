#include "comp/ofx/Overlay.h"

#include <map>
#include <mutex>

#include "OfxInternal.h"
#include "OfxNode.h"
#include "ofxInteract.h"
#include "ofxKeySyms.h"
#include "ofxOld.h"  // kOfxInteractPropViewportSize: deprecated, but older plugins read it

namespace comp::ofx {

// The interact's property set; OfxInteractHandle points at one of these.
struct InteractProps : PropertySet {
    Overlay::Impl* owner = nullptr;  // null for a descriptor
    OfxInteractHandle interactHandle() { return reinterpret_cast<OfxInteractHandle>(this); }
    static InteractProps* from(OfxInteractHandle h) { return reinterpret_cast<InteractProps*>(h); }
};

struct Overlay::Impl {
    OfxNode* node = nullptr;
    OfxPluginEntryPoint* entry = nullptr;
    InteractProps props;
    bool redraw = false;

    // Runs an interact action; returns its status (failures: false to the caller).
    OfxStatus call(const char* action, PropertySet* args, const View& view, const Edit* edit);
};

namespace {

// ---------------------------------------------------------------- interact suite

OfxStatus interactSwapBuffers(OfxInteractHandle) { return kOfxStatOK; }  // the host composites

OfxStatus interactRedraw(OfxInteractHandle h) {
    if (!h) return kOfxStatErrBadHandle;
    if (auto* owner = InteractProps::from(h)->owner) owner->redraw = true;
    return kOfxStatOK;
}

OfxStatus interactGetPropertySet(OfxInteractHandle h, OfxPropertySetHandle* out) {
    if (!h || !out) return kOfxStatErrBadHandle;
    *out = InteractProps::from(h)->handle();
    return kOfxStatOK;
}

const OfxInteractSuiteV1 kInteractSuite{interactSwapBuffers, interactRedraw, interactGetPropertySet};

// Interact descriptors, described once per plugin.
std::mutex descriptorsMutex;
std::map<const Plugin*, std::unique_ptr<InteractProps>>& descriptors() {
    static std::map<const Plugin*, std::unique_ptr<InteractProps>> d;
    return d;
}

void setInteractDefaults(PropertySet& p) {
    p.setInt(kOfxInteractPropHasAlpha, 0, 1);
    p.setInt(kOfxInteractPropBitDepth, 0, 8);
}

int keySym(Overlay::Key key, const std::string& text) {
    using K = Overlay::Key;
    switch (key) {
        case K::Return: return kOfxKey_Return;
        case K::Escape: return kOfxKey_Escape;
        case K::Delete: return kOfxKey_Delete;
        case K::Backspace: return kOfxKey_BackSpace;
        case K::Tab: return kOfxKey_Tab;
        case K::Left: return kOfxKey_Left;
        case K::Right: return kOfxKey_Right;
        case K::Up: return kOfxKey_Up;
        case K::Down: return kOfxKey_Down;
        case K::Character: break;
    }
    // Latin-1 characters are their own key symbols.
    return text.empty() ? 0 : static_cast<unsigned char>(text[0]);
}

}  // namespace

const void* interactSuite() { return &kInteractSuite; }

// Where values a plugin sets during an interact event go (see OfxNode).
InteractEdit*& currentInteractEdit() {
    thread_local InteractEdit* edit = nullptr;
    return edit;
}

// ---------------------------------------------------------------- overlay

std::unique_ptr<Overlay> Overlay::create(Node& node) {
    auto* ofxNode = dynamic_cast<OfxNode*>(&node);
    if (!ofxNode) return nullptr;
    Plugin& plugin = ofxNode->plugin_;
    void* entry = nullptr;
    {
        std::lock_guard lock(plugin.contextDescriptor->mutex());
        if (const Property* p = plugin.contextDescriptor->find(kOfxImageEffectPluginPropOverlayInteractV1))
            if (p->type == Property::Type::Pointer && !p->pointers.empty()) entry = p->pointers[0];
    }
    if (!entry) return nullptr;
    auto* main = reinterpret_cast<OfxPluginEntryPoint*>(entry);

    {  // the descriptor, once per plugin
        std::lock_guard lock(descriptorsMutex);
        auto& d = descriptors()[&plugin];
        if (!d) {
            d = std::make_unique<InteractProps>();
            setInteractDefaults(*d);
            const OfxStatus s = main(kOfxActionDescribe, d->interactHandle(), nullptr, nullptr);
            if (s != kOfxStatOK && s != kOfxStatReplyDefault) {
                d.reset();
                return nullptr;
            }
        }
    }

    auto impl = std::make_unique<Impl>();
    impl->node = ofxNode;
    impl->entry = main;
    InteractProps& p = impl->props;
    {
        std::lock_guard lock(descriptorsMutex);
        p.copyFrom(*descriptors()[&plugin]);
    }
    p.owner = impl.get();
    p.setPointer(kOfxPropEffectInstance, 0, ofxNode->instance_->handle());
    p.setPointer(kOfxPropInstanceData, 0, nullptr);
    p.setDoubles(kOfxInteractPropPixelScale, {1, 1});
    p.setDoubles(kOfxInteractPropBackgroundColour, {0, 0, 0});
    p.setInts(kOfxInteractPropViewportSize, {0, 0});
    p.setDoubles(kOfxInteractPropSuggestedColour, {1, 1, 1});
    p.setStrings(kOfxInteractPropSlaveToParam, {});
    if (main(kOfxActionCreateInstance, p.interactHandle(), nullptr, nullptr) == kOfxStatFailed) return nullptr;
    return std::unique_ptr<Overlay>(new Overlay(node, std::move(impl)));
}

Overlay::Overlay(Node& node, std::unique_ptr<Impl> impl) : node_(node), impl_(std::move(impl)) {}

Overlay::~Overlay() {
    impl_->entry(kOfxActionDestroyInstance, impl_->props.interactHandle(), nullptr, nullptr);
}

OfxStatus Overlay::Impl::call(const char* action, PropertySet* args, const View& view, const Edit* edit) {
    OfxNode& n = *node;
    props.setDoubles(kOfxInteractPropPixelScale, {view.pixelScale, view.pixelScale});
    props.setDoubles(kOfxInteractPropBackgroundColour, {view.background[0], view.background[1], view.background[2]});
    props.setInts(kOfxInteractPropViewportSize, {view.viewportWidth, view.viewportHeight});
    args->setPointer(kOfxPropEffectInstance, 0, n.instance_->handle());
    args->setDouble(kOfxPropTime, 0, view.time);
    args->setDoubles(kOfxImageEffectPropRenderScale, {view.renderScale, view.renderScale});
    args->setDoubles(kOfxInteractPropPixelScale, {view.pixelScale, view.pixelScale});
    args->setDoubles(kOfxInteractPropBackgroundColour, {view.background[0], view.background[1], view.background[2]});
    args->setInts(kOfxInteractPropViewportSize, {view.viewportWidth, view.viewportHeight});

    // Like the node's other non-render actions: one at a time.
    std::lock_guard lock(n.actionMutex_);
    std::unique_lock unsafe(n.plugin_.renderMutex, std::defer_lock);
    if (n.plugin_.contextDescriptor->getString(kOfxImageEffectPluginRenderThreadSafety) == kOfxImageEffectRenderUnsafe)
        unsafe.lock();

    // A context for suite calls (clip regions, the project): the last info
    // pass at this frame and scale if there was one, else just the frame.
    CallContext ctx;
    OfxNode::FrameInfo fi;
    {
        std::lock_guard infoLock(n.infoMutex_);
        auto it = n.frameInfos_.find({view.time, view.renderScale, view.format.width, view.format.height});
        if (it != n.frameInfos_.end()) fi = it->second;
        else {
            fi.scale = view.renderScale;
            fi.flipCanonical = view.flipY;
            fi.flipY = static_cast<int>(std::lround(view.flipY * view.renderScale));
            fi.connected.assign(n.inputClips_.size(), false);
            fi.inputs.assign(n.inputClips_.size(), ImageInfo{});
        }
    }
    n.fillContext(ctx, fi, view.time);
    ScopedContext scope(&ctx);

    // Values the plugin sets go to `edit`, or straight onto the node.
    const Edit direct = [this](const std::string& key, ParamValue v) { node->setParam(key, std::move(v)); };
    InteractEdit routing{&n, edit && *edit ? edit : &direct};
    InteractEdit* previous = currentInteractEdit();
    currentInteractEdit() = &routing;
    OfxStatus status = kOfxStatFailed;
    try {
        status = entry(action, props.interactHandle(), args->handle(), nullptr);
    } catch (...) {
    }
    currentInteractEdit() = previous;
    return status;
}

std::array<double, 4> Overlay::canonicalRect(const View& view, double left, double top, double right, double bottom) {
    return {left, view.flipY - bottom, right, view.flipY - top};
}

void Overlay::draw(const View& view) {
    PropertySet args;
    impl_->call(kOfxInteractActionDraw, &args, view, nullptr);
}

namespace {
void penArgs(PropertySet& args, const Overlay::View& view, double x, double y) {
    args.setDoubles(kOfxInteractPropPenPosition, {x, view.flipY - y});  // canonical: y up
    args.setInts(kOfxInteractPropPenViewportPosition, {0, 0});
    args.setDouble(kOfxInteractPropPenPressure, 0, 1.0);
}
}  // namespace

bool Overlay::penDown(const View& view, double x, double y, const Edit& edit) {
    PropertySet args;
    penArgs(args, view, x, y);
    return impl_->call(kOfxInteractActionPenDown, &args, view, &edit) == kOfxStatOK;
}

bool Overlay::penMotion(const View& view, double x, double y, const Edit& edit) {
    PropertySet args;
    penArgs(args, view, x, y);
    return impl_->call(kOfxInteractActionPenMotion, &args, view, &edit) == kOfxStatOK;
}

bool Overlay::penUp(const View& view, double x, double y, const Edit& edit) {
    PropertySet args;
    penArgs(args, view, x, y);
    return impl_->call(kOfxInteractActionPenUp, &args, view, &edit) == kOfxStatOK;
}

bool Overlay::keyDown(const View& view, Key key, const std::string& text, const Edit& edit) {
    PropertySet args;
    args.setInt(kOfxPropKeySym, 0, keySym(key, text));
    args.setString(kOfxPropKeyString, 0, text);
    return impl_->call(kOfxInteractActionKeyDown, &args, view, &edit) == kOfxStatOK;
}

bool Overlay::keyUp(const View& view, Key key, const std::string& text, const Edit& edit) {
    PropertySet args;
    args.setInt(kOfxPropKeySym, 0, keySym(key, text));
    args.setString(kOfxPropKeyString, 0, text);
    return impl_->call(kOfxInteractActionKeyUp, &args, view, &edit) == kOfxStatOK;
}

bool Overlay::takeRedrawRequest() {
    const bool r = impl_->redraw;
    impl_->redraw = false;
    return r;
}

}  // namespace comp::ofx
