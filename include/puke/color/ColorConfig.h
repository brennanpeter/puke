#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "puke/core/Image.h"

namespace puke {

// Color management, via OpenColorIO (kept out of this header).
//
// All pixels inside a graph are in one linear working space: the config's
// "scene_linear" role (ACEScg in the built-in config). Read converts files
// into it, Write converts out of it, and the ColorSpace node converts
// between any two spaces in the middle of a graph.
//
// Which config: $OCIO if it's set, otherwise puke's own built-in config (see
// ColorConfig.cpp for its color spaces, which follow the ACES studio
// config's names). One config is current for the whole process.
//
// Anywhere a color space is named, an OCIO role ("scene_linear",
// "matte_paint", ...) works too.
class ColorConfig {
public:
    static std::shared_ptr<const ColorConfig> builtin();
    static std::shared_ptr<const ColorConfig> fromFile(const std::string& path);
    // $OCIO if set and not empty, otherwise builtin().
    static std::shared_ptr<const ColorConfig> fromEnvironment();

    // The config nodes use. Loaded with fromEnvironment() on first use.
    static std::shared_ptr<const ColorConfig> current();
    // Replaces the current config (nullptr: reload from the environment).
    // Renders already running keep the config they started with.
    static void setCurrent(std::shared_ptr<const ColorConfig> config);

    // The file it came from, or "built-in".
    const std::string& source() const;
    // Changes whenever the config's contents do. Part of cache keys.
    std::uint64_t hash() const;

    // Every color space's name, in config order.
    std::vector<std::string> colorSpaces() const;
    // Whether `name` is a color space or role in this config.
    bool has(const std::string& name) const;
    // The color space `name` refers to (resolving roles). Throws Error if
    // there isn't one, listing the color spaces that do exist.
    std::string resolve(const std::string& name) const;
    // The working space: what the "scene_linear" role points at.
    std::string workingSpace() const;

    // Displays and their views, for viewers (OCIO's display/view model).
    std::vector<std::string> displays() const;
    std::string defaultDisplay() const;
    std::vector<std::string> views(const std::string& display) const;
    std::string defaultView(const std::string& display) const;

    // Converts working-space color for viewing on `display` through `view`.
    // R, G and B (or Y) only, as they are: no alpha handling, as viewers
    // show premultiplied color. Throws Error for an unknown display or view.
    void convertToDisplay(Image& image, const std::string& display, const std::string& view) const;

    // Whether converting `from` -> `to` would change any pixels.
    bool isNoOp(const std::string& from, const std::string& to) const;

    // Converts `image` in place. Only color is converted: the R, G and B
    // channels, or a lone Y (luminance) channel. Other channels, including
    // alpha, are left alone. When there's an alpha channel the color is
    // treated as premultiplied: divided by alpha before converting and
    // multiplied back after. Where alpha is 0: transparent black is left
    // alone; any other color (an additive glow) is converted as is.
    // Throws Error for an unknown color space, or if a conversion is needed
    // but the image has no color channels.
    //
    // `area` limits the conversion to part of the image (default: all of
    // it). Nodes convert only inside their data window: some curves map 0
    // to non-zero, which would otherwise fill the padding around an image.
    void convert(Image& image, const std::string& from, const std::string& to) const;
    void convert(Image& image, const std::string& from, const std::string& to, const Box& area) const;

    struct Impl;
    explicit ColorConfig(std::shared_ptr<const Impl> impl);

private:
    std::shared_ptr<const Impl> impl_;
};

}  // namespace puke
