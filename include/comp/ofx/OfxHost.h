#pragma once

#include <memory>
#include <string>
#include <vector>

#include "comp/graph/Node.h"

namespace comp::ofx {

// OpenFX plugin hosting: OFX image effects run as comp nodes.
//
// Plugins are found in the folders listed in $OFX_PLUGIN_PATH (colon
// separated), then the standard /usr/OFX/Plugins, searched recursively for
// .ofx.bundle folders. The scan happens once, on first use. Each plugin
// becomes a node type named "ofx:" + its identifier, e.g.
// "ofx:net.sf.openfx.GammaPlugin", so it can be created by name (Graph,
// project files, Python) like any built-in node.
//
// Supported: the Filter, Generator and General contexts (the first of
// those a plugin offers; a generator's frame is the project format), float
// RGBA images (and Alpha for clips that only take alpha),
// tiles, the plugin's declared render thread safety, and parameters of type
// integer, double, boolean, choice, string, 2D and colour.
//
// Not yet: interacts and overlays, animated
// parameters, reading other frames than the one being rendered, render
// scales other than 1, and macOS/Windows bundles.
//
// Parameters are ordinary node parameters, stored by the OFX parameter
// name: integer -> int, double -> float, boolean -> bool, choice -> int
// (the option index; setting the option's label as a string works too),
// string -> str, 2D -> Vec2, RGB/RGBA/3D -> Color. Unset parameters use the
// plugin's defaults and aren't written to project files.
//
// Coordinates: OFX's y axis points up, comp's down. Images are flipped
// around the input's display window, so for a frame starting at (0, 0) OFX
// sees the usual bottom-left origin, as in Nuke. Spatial parameters (2D
// positions, offsets) are in OFX's y-up coordinates.

inline constexpr const char* kNodeTypePrefix = "ofx:";

struct PluginInfo {
    std::string identifier;  // e.g. "net.sf.openfx.GammaPlugin"
    std::string bundle;      // path of the .ofx.bundle it was found in
    int versionMajor = 0;
    int versionMinor = 0;
};

// Every image effect plugin found, in identifier order. A plugin can still
// fail when first used (e.g. it needs a context comp doesn't support yet);
// createNode() reports why.
std::vector<PluginInfo> plugins();

// Bundles that couldn't be loaded and similar problems found while
// scanning, as messages. A broken bundle never stops the others loading.
std::vector<std::string> scanProblems();

// A new node running the plugin. Loads and describes the plugin on first
// use. Throws Error if there's no such plugin or it can't be used.
std::unique_ptr<Node> createNode(const std::string& identifier, const std::string& name);

}  // namespace comp::ofx
