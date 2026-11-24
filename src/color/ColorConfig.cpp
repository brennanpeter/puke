#include "comp/color/ColorConfig.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>
#include <sstream>

#include <OpenColorIO/OpenColorIO.h>

#include "comp/core/Hash.h"
#include "comp/graph/Node.h"  // Error

namespace comp {

namespace OCIO = OCIO_NAMESPACE;

namespace {

// comp's built-in config, used when $OCIO isn't set. Kept small: the spaces
// a compositor needs day to day, built from OCIO's own built-in transforms
// (so no LUT files), with names matching the ACES studio configs so projects
// carry over to a studio config.
//
//   ACES2065-1             scene reference
//   ACEScg                 working space (scene_linear)
//   ACEScct                log, for grading (color_timing, compositing_log)
//   Linear Rec.709 (sRGB)  linear, Rec.709 primaries
//   sRGB - Texture         sRGB-encoded, Rec.709 primaries (matte_paint):
//                          8- and 16-bit files by default
//   Raw                    data: never converted
//
// Displays (for the viewer, later): sRGB and Rec.1886 Rec.709, each with an
// ACES 1.0 SDR view, an un-tone-mapped view and Raw.
//
// Needs OCIO 2.1 or newer (BuiltinTransform).
constexpr const char* kBuiltinConfig = R"yaml(
ocio_profile_version: 2.1

name: comp-builtin
description: comp's built-in color config, used when $OCIO is not set.
environment: {}
search_path: ""
strictparsing: true
luma: [0.2126, 0.7152, 0.0722]

roles:
  aces_interchange: ACES2065-1
  cie_xyz_d65_interchange: CIE-XYZ-D65
  color_picking: sRGB - Texture
  color_timing: ACEScct
  compositing_log: ACEScct
  data: Raw
  default: ACEScg
  matte_paint: sRGB - Texture
  scene_linear: ACEScg
  texture_paint: ACEScct

file_rules:
  - !<Rule> {name: Default, colorspace: default}

displays:
  sRGB - Display:
    - !<View> {name: ACES 1.0 - SDR Video, view_transform: ACES 1.0 - SDR Video, display_colorspace: sRGB - Display}
    - !<View> {name: Un-tone-mapped, view_transform: Un-tone-mapped, display_colorspace: sRGB - Display}
    - !<View> {name: Raw, colorspace: Raw}
  Rec.1886 Rec.709 - Display:
    - !<View> {name: ACES 1.0 - SDR Video, view_transform: ACES 1.0 - SDR Video, display_colorspace: Rec.1886 Rec.709 - Display}
    - !<View> {name: Un-tone-mapped, view_transform: Un-tone-mapped, display_colorspace: Rec.1886 Rec.709 - Display}
    - !<View> {name: Raw, colorspace: Raw}

active_displays: []
active_views: []
default_view_transform: Un-tone-mapped

view_transforms:
  - !<ViewTransform>
    name: ACES 1.0 - SDR Video
    description: ACES 1.0 output transform for SDR video
    from_scene_reference: !<BuiltinTransform> {style: ACES-OUTPUT - ACES2065-1_to_CIE-XYZ-D65 - SDR-VIDEO_1.0}

  - !<ViewTransform>
    name: Un-tone-mapped
    description: Linear to the display, no tone mapping
    from_scene_reference: !<BuiltinTransform> {style: UTILITY - ACES-AP0_to_CIE-XYZ-D65_BFD}

display_colorspaces:
  - !<ColorSpace>
    name: CIE-XYZ-D65
    description: The display reference space
    isdata: false

  - !<ColorSpace>
    name: sRGB - Display
    description: sRGB monitor (piecewise sRGB curve, Rec.709 primaries)
    isdata: false
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_sRGB}

  - !<ColorSpace>
    name: Rec.1886 Rec.709 - Display
    description: Rec.709 video monitor (Rec.1886 / gamma 2.4)
    isdata: false
    from_display_reference: !<BuiltinTransform> {style: DISPLAY - CIE-XYZ-D65_to_REC.1886-REC.709}

colorspaces:
  - !<ColorSpace>
    name: ACES2065-1
    description: ACES AP0, linear. The scene reference space.
    isdata: false

  - !<ColorSpace>
    name: ACEScg
    description: ACES AP1, linear. The working space.
    isdata: false
    to_scene_reference: !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}

  - !<ColorSpace>
    name: ACEScct
    description: ACES AP1, ACEScct log curve
    isdata: false
    to_scene_reference: !<BuiltinTransform> {style: ACEScct_to_ACES2065-1}

  - !<ColorSpace>
    name: Linear Rec.709 (sRGB)
    description: Rec.709 / sRGB primaries, linear
    isdata: false
    to_scene_reference: !<GroupTransform>
      children:
        - !<BuiltinTransform> {style: UTILITY - ACES-AP1_to_LINEAR-REC709_BFD, direction: inverse}
        - !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}

  - !<ColorSpace>
    name: sRGB - Texture
    description: Rec.709 / sRGB primaries, piecewise sRGB curve
    isdata: false
    to_scene_reference: !<GroupTransform>
      children:
        - !<ExponentWithLinearTransform> {gamma: 2.4, offset: 0.055}  # forward = decode to linear
        - !<BuiltinTransform> {style: UTILITY - ACES-AP1_to_LINEAR-REC709_BFD, direction: inverse}
        - !<BuiltinTransform> {style: ACEScg_to_ACES2065-1}

  - !<ColorSpace>
    name: Raw
    description: Data, such as normals or IDs. Never converted.
    isdata: true
)yaml";

// Runs `f`, turning OCIO's exceptions into ours, prefixed with the config's source.
template <typename F>
auto guarded(const std::string& source, F&& f) {
    try {
        return f();
    } catch (const OCIO::Exception& e) {
        throw Error("OCIO config '" + source + "': " + e.what());
    }
}

std::mutex currentMutex;
std::shared_ptr<const ColorConfig> currentConfig;  // guarded by currentMutex

}  // namespace

struct ColorConfig::Impl {
    OCIO::ConstConfigRcPtr config;
    std::string source;
    std::uint64_t hash = 0;

    // Conversions, set up once each. Getting a processor must not happen on
    // several threads at once (OCIO 2.1 races in there); applying one can.
    struct Conversion {
        bool noOp;
        OCIO::ConstCPUProcessorRcPtr cpu;
    };
    mutable std::mutex mutex;
    mutable std::map<std::pair<std::string, std::string>, Conversion> conversions;  // by resolved names
    mutable std::map<std::pair<std::string, std::string>, Conversion> displayViews;  // by display, view

    const Conversion& conversion(const std::string& src, const std::string& dst) const {
        std::lock_guard lock(mutex);
        auto it = conversions.find({src, dst});
        if (it == conversions.end()) {
            const auto proc = guarded(source, [&] { return config->getProcessor(src.c_str(), dst.c_str()); });
            Conversion c{proc->isNoOp(), guarded(source, [&] { return proc->getDefaultCPUProcessor(); })};
            it = conversions.emplace(std::make_pair(src, dst), std::move(c)).first;
        }
        return it->second;  // std::map: stays valid as more are added
    }

    const Conversion& displayView(const std::string& display, const std::string& view) const {
        std::lock_guard lock(mutex);
        auto it = displayViews.find({display, view});
        if (it == displayViews.end()) {
            const auto proc = guarded(source, [&] {
                auto t = OCIO::DisplayViewTransform::Create();
                t->setSrc(OCIO::ROLE_SCENE_LINEAR);
                t->setDisplay(display.c_str());
                t->setView(view.c_str());
                return config->getProcessor(t);
            });
            Conversion c{proc->isNoOp(), guarded(source, [&] { return proc->getDefaultCPUProcessor(); })};
            it = displayViews.emplace(std::make_pair(display, view), std::move(c)).first;
        }
        return it->second;
    }
};

namespace {

std::shared_ptr<const ColorConfig> makeConfig(OCIO::ConstConfigRcPtr config, std::string source) {
    return guarded(source, [&] {
        config->validate();
        auto impl = std::make_shared<ColorConfig::Impl>();
        impl->hash = Hasher().add(std::string_view(config->getCacheID())).value();
        impl->config = std::move(config);
        impl->source = std::move(source);
        return std::make_shared<const ColorConfig>(std::move(impl));
    });
}

}  // namespace


namespace {

// Runs `cpu` over the color of `image` inside `region`: R, G and B, or a
// lone Y. With `premultiplied`, color is divided by alpha first and
// multiplied back after, and transparent black is left alone.
void applyCpu(const OCIO::CPUProcessor& cpu, Image& image, const Box& region, const std::string& what,
              bool premultiplied) {
    int r = image.channelIndex("R"), g = image.channelIndex("G"), b = image.channelIndex("B");
    if (r < 0 || g < 0 || b < 0) {
        r = g = b = image.channelIndex("Y");  // luminance only: convert it as grey
        if (r < 0) throw Error(what + ": the image has no R, G and B channels (or Y)");
    }
    int a = -1;
    if (premultiplied) {
        a = image.channelIndex("A");
        if (a < 0) a = image.channelIndex("alpha");
    }

    const int width = region.width();
    const int nch = image.numChannels();
    std::vector<float> rgb(static_cast<std::size_t>(width) * 3);
    for (int y = region.y0; y < region.y1; ++y) {
        float* row = image.pixel(region.x0, y);
        for (int x = 0; x < width; ++x) {
            const float* p = row + static_cast<std::size_t>(x) * nch;
            const float alpha = a >= 0 ? p[a] : 1.0f;
            const float k = alpha != 0.0f ? 1.0f / alpha : 1.0f;
            rgb[3 * x + 0] = p[r] * k;
            rgb[3 * x + 1] = p[g] * k;
            rgb[3 * x + 2] = p[b] * k;
        }
        OCIO::PackedImageDesc desc(rgb.data(), width, 1, 3);
        try {
            cpu.apply(desc);
        } catch (const OCIO::Exception& e) {
            throw Error(what + ": " + e.what());
        }
        for (int x = 0; x < width; ++x) {
            float* p = row + static_cast<std::size_t>(x) * nch;
            const float alpha = a >= 0 ? p[a] : 1.0f;
            // Transparent black stays transparent black, even where a curve
            // maps 0 elsewhere (ACEScct, say). Alpha-0 pixels with color
            // (additive glows) are converted like the rest.
            if (a >= 0 && alpha == 0.0f && p[r] == 0.0f && p[g] == 0.0f && p[b] == 0.0f) continue;
            const float k = alpha != 0.0f ? alpha : 1.0f;
            p[r] = rgb[3 * x + 0] * k;  // with Y alone, r == g == b: one write
            if (g != r) p[g] = rgb[3 * x + 1] * k;
            if (b != r) p[b] = rgb[3 * x + 2] * k;
        }
    }
}

}  // namespace

ColorConfig::ColorConfig(std::shared_ptr<const Impl> impl) : impl_(std::move(impl)) {}

std::shared_ptr<const ColorConfig> ColorConfig::builtin() {
    static const auto config = [] {
        std::istringstream yaml(kBuiltinConfig);
        return makeConfig(guarded("built-in", [&] { return OCIO::Config::CreateFromStream(yaml); }), "built-in");
    }();
    return config;
}

std::shared_ptr<const ColorConfig> ColorConfig::fromFile(const std::string& path) {
    return makeConfig(guarded(path, [&] { return OCIO::Config::CreateFromFile(path.c_str()); }), path);
}

std::shared_ptr<const ColorConfig> ColorConfig::fromEnvironment() {
    const char* env = std::getenv("OCIO");
    if (env && *env) return fromFile(env);
    return builtin();
}

std::shared_ptr<const ColorConfig> ColorConfig::current() {
    std::lock_guard lock(currentMutex);
    if (!currentConfig) currentConfig = fromEnvironment();
    return currentConfig;
}

void ColorConfig::setCurrent(std::shared_ptr<const ColorConfig> config) {
    std::lock_guard lock(currentMutex);
    currentConfig = std::move(config);  // nullptr: current() reloads on next use
}

const std::string& ColorConfig::source() const { return impl_->source; }
std::uint64_t ColorConfig::hash() const { return impl_->hash; }

std::vector<std::string> ColorConfig::colorSpaces() const {
    std::vector<std::string> names;
    const auto& c = *impl_->config;
    for (int i = 0; i < c.getNumColorSpaces(); ++i) names.emplace_back(c.getColorSpaceNameByIndex(i));
    return names;
}

bool ColorConfig::has(const std::string& name) const {
    return !name.empty() && impl_->config->getColorSpace(name.c_str()) != nullptr;
}

std::string ColorConfig::resolve(const std::string& name) const {
    if (auto cs = impl_->config->getColorSpace(name.c_str()); cs && !name.empty()) return cs->getName();
    std::string known;
    for (const auto& n : colorSpaces()) known += (known.empty() ? "" : ", ") + n;
    throw Error("unknown color space '" + name + "' in OCIO config '" + impl_->source + "' (known: " + known + ")");
}

std::vector<std::string> ColorConfig::displays() const {
    std::vector<std::string> out;
    const auto& c = *impl_->config;
    for (int i = 0; i < c.getNumDisplays(); ++i) out.emplace_back(c.getDisplay(i));
    return out;
}

std::string ColorConfig::defaultDisplay() const { return impl_->config->getDefaultDisplay(); }

std::vector<std::string> ColorConfig::views(const std::string& display) const {
    std::vector<std::string> out;
    const auto& c = *impl_->config;
    for (int i = 0; i < c.getNumViews(display.c_str()); ++i) out.emplace_back(c.getView(display.c_str(), i));
    return out;
}

std::string ColorConfig::defaultView(const std::string& display) const {
    return impl_->config->getDefaultView(display.c_str());
}

void ColorConfig::convertToDisplay(Image& image, const std::string& display, const std::string& view) const {
    const auto ds = displays();
    if (std::find(ds.begin(), ds.end(), display) == ds.end())
        throw Error("unknown display '" + display + "' in OCIO config '" + impl_->source + "'");
    const auto vs = views(display);
    if (std::find(vs.begin(), vs.end(), view) == vs.end())
        throw Error("display '" + display + "' has no view '" + view + "'");
    if (image.region().empty()) return;
    const auto& conversion = impl_->displayView(display, view);
    if (conversion.noOp) return;
    applyCpu(*conversion.cpu, image, image.region(), "cannot show on '" + display + "' / '" + view + "'", false);
}

std::string ColorConfig::workingSpace() const {
    if (!impl_->config->hasRole(OCIO::ROLE_SCENE_LINEAR))
        throw Error("OCIO config '" + impl_->source + "' has no scene_linear role");
    return resolve(OCIO::ROLE_SCENE_LINEAR);
}

bool ColorConfig::isNoOp(const std::string& from, const std::string& to) const {
    const std::string src = resolve(from), dst = resolve(to);
    return src == dst || impl_->conversion(src, dst).noOp;
}

void ColorConfig::convert(Image& image, const std::string& from, const std::string& to) const {
    convert(image, from, to, image.region());
}

void ColorConfig::convert(Image& image, const std::string& from, const std::string& to, const Box& area) const {
    const Box region = area.intersect(image.region());
    if (region.empty()) return;
    const std::string src = resolve(from), dst = resolve(to);
    if (src == dst) return;
    const auto& conversion = impl_->conversion(src, dst);
    if (conversion.noOp) return;

    applyCpu(*conversion.cpu, image, region, "cannot convert from '" + from + "' to '" + to + "'", true);
}

}  // namespace comp
