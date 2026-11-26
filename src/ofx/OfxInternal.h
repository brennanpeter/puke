#pragma once

// Internals of comp's OpenFX host, shared by OfxHost.cpp (suites, plugin
// loading) and OfxNode.cpp (running a plugin inside the engine). Nothing
// here is public API; see include/comp/ofx/OfxHost.h for that.

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include "comp/core/Box.h"
#include "comp/core/Image.h"
#include "comp/graph/Node.h"

namespace comp::ofx {

// ---------------------------------------------------------------- properties

// One named property: a typed array of values.
struct Property {
    enum class Type { Pointer, String, Double, Int };
    Type type = Type::Int;
    std::vector<void*> pointers;
    std::vector<std::string> strings;
    std::vector<double> doubles;
    std::vector<int> ints;

    int size() const;
    void resize(int n);
};

// What every OFX handle with properties points at (OfxPropertySetHandle).
// Plugins may create properties hosts don't know about, so set() creates;
// get() of a missing property is an error, as the API requires.
class PropertySet {
public:
    virtual ~PropertySet() = default;

    void setPointer(const std::string& name, int index, void* v);
    void setString(const std::string& name, int index, const std::string& v);
    void setDouble(const std::string& name, int index, double v);
    void setInt(const std::string& name, int index, int v);
    void setStrings(const std::string& name, const std::vector<std::string>& v);
    void setDoubles(const std::string& name, const std::vector<double>& v);
    void setInts(const std::string& name, const std::vector<int>& v);

    // nullptr if absent. The lock guards against plugins touching a set from
    // several render threads; values returned to plugins (strings) stay valid
    // until the property is changed.
    const Property* find(const std::string& name) const;
    Property* find(const std::string& name);
    std::mutex& mutex() const { return mutex_; }

    // Convenience reads for the host, with a fallback.
    std::string getString(const std::string& name, int index = 0, const std::string& fallback = {}) const;
    int getInt(const std::string& name, int index = 0, int fallback = 0) const;
    double getDouble(const std::string& name, int index = 0, double fallback = 0) const;
    std::vector<std::string> getStrings(const std::string& name) const;
    std::vector<double> getDoubles(const std::string& name) const;

    Property& at(const std::string& name, Property::Type type);  // creates or retypes; caller locks
    void copyFrom(const PropertySet& other);

    OfxPropertySetHandle handle() { return reinterpret_cast<OfxPropertySetHandle>(this); }
    static PropertySet* from(OfxPropertySetHandle h) { return reinterpret_cast<PropertySet*>(h); }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Property> props_;
};

// ---------------------------------------------------------------- effects

struct Effect;
class OfxNode;

struct Clip : PropertySet {
    std::string name;
    Effect* effect = nullptr;
    int input = -1;  // index of the comp input feeding it; -1 for the output clip
};

struct Param : PropertySet {
    std::string name;
    std::string type;  // kOfxParamType...
    Effect* effect = nullptr;
};

struct ParamSet : PropertySet {
    Effect* effect = nullptr;
    std::vector<std::unique_ptr<Param>> params;  // definition order
    Param* find(const std::string& name) const;
};

struct Plugin;

// An image effect: the plugin's descriptor (node == nullptr) or an instance
// belonging to an OfxNode. OfxImageEffectHandle points at one of these.
struct Effect : PropertySet {
    Plugin* plugin = nullptr;
    const OfxNode* node = nullptr;
    ParamSet params;
    std::vector<std::unique_ptr<Clip>> clips;  // definition order
    Clip* findClip(const std::string& name) const;

    // Copies a descriptor's properties, clips and params into a new instance.
    void copyFrom(const Effect& descriptor);

    OfxImageEffectHandle handle() { return reinterpret_cast<OfxImageEffectHandle>(this); }
    static Effect* from(OfxImageEffectHandle h) { return reinterpret_cast<Effect*>(h); }
};

// ---------------------------------------------------------------- plugins

struct Binary;  // a loaded .ofx file

struct Plugin {
    OfxPlugin* api = nullptr;
    Binary* binary = nullptr;
    std::string identifier;
    std::string bundlePath;

    // Set up on first use by describe(), under describeMutex.
    std::mutex describeMutex;
    bool described = false;
    std::string error;                  // why the plugin can't be used, if it can't
    std::unique_ptr<Effect> descriptor;  // from kOfxActionDescribe
    std::unique_ptr<Effect> contextDescriptor;  // from DescribeInContext
    std::string context;                // kOfxImageEffectContextFilter or ...General

    // For plugins whose render isn't thread safe at all.
    std::mutex renderMutex;

    // Loads and describes the plugin if that hasn't happened. Throws Error
    // (and keeps throwing it) if the plugin can't be used.
    void describe();

    // Calls the plugin's main entry point; throws Error on a failure status,
    // including any message the plugin posted.
    OfxStatus call(const char* action, const void* handle, PropertySet* inArgs, PropertySet* outArgs,
                   const std::string& who) const;
};

// Every plugin found, scanning on first use.
std::vector<Plugin*>& allPlugins();
Plugin* findPlugin(const std::string& identifier);

// The host description handed to plugins.
OfxHost* host();

// Messages plugins post with the message suite, per thread, so a failing
// action can report what the plugin said.
std::string takeLastMessage();
// Records why a suite call failed, for the plugin's action error.
void noteProblem(const std::string& message);

// ---------------------------------------------------------------- rendering

// What the engine gives one action call: set as the current thread's
// context while an action runs, so suite calls the plugin makes (getting a
// clip's image or region of definition) can find it.
struct CallContext {
    const OfxNode* node = nullptr;
    double time = 0;
    int flipY = 0;  // OFX y = flipY - comp y (see OfxNode.cpp), in pixel coordinates
    double scale = 1;        // render scale
    int flipCanonical = 0;   // the same for canonical (full-size) coordinates
    std::vector<ImageInfo> inputInfos;  // per comp input; empty info if unconnected
    std::vector<bool> connected;
    ImageInfo outputInfo;

    // During render only.
    std::vector<const Image*> inputs;
    Box renderWindow;            // comp coordinates
    std::vector<float>* output = nullptr;  // RGBA, OFX row order, covering renderWindow

    // Input buffers built on first request, one per clip.
    std::mutex buffersMutex;
    std::map<const Clip*, std::vector<float>> buffers;
};

CallContext* currentContext();
struct ScopedContext {
    explicit ScopedContext(CallContext* c);
    ~ScopedContext();
    CallContext* previous;
};

// Rectangles between comp's y-down pixel boxes and OFX's y-up coordinates.
OfxRectI toOfx(const Box& b, int flipY);
Box fromOfx(const OfxRectI& r, int flipY);
Box fromOfx(const OfxRectD& r, int flipY);  // rounded outwards

}  // namespace comp::ofx
