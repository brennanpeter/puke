#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "puke/graph/Graph.h"

namespace puke {

// Project files: a graph saved as JSON (conventionally with a .puke extension).
//
//   {
//     "format": "puke",   (or "comp": the old name; read, never written)
//     "version": 1,
//     "nodes": [
//       {
//         "name": "Read1",
//         "type": "Read",
//         "inputs": [],
//         "params": { "file": "plate.####.exr", "first": 1001, "last": 1100 }
//       },
//       {
//         "name": "Grade1",
//         "type": "Grade",
//         "inputs": ["Read1"],
//         "params": { "gain": [1.2, 1.1, 1.0, 1.0] }
//       }
//     ]
//   }
//
// A node may also have "position": [x, y], where a node graph editor placed
// it (whole pixels).
//
// The project's settings (Graph::settings()) go in an optional top-level
// object, left out when they're all default; any key in it may be left out:
//
//   "settings": {
//     "first_frame": 1001, "last_frame": 1100,     (default 1 and 1)
//     "fps": 24,                                   (default 24)
//     "format": {"width": 1920, "height": 1080}    (the project format;
//   }                                               default 1920x1080)
//
// (Not to be confused with the top-level "format": "puke", which says what
// kind of file this is.)
//
// Written to diff well: two-space indent, nodes in graph order, parameters
// sorted by name. "inputs" lists the node feeding each input by name, null
// for an unconnected input, with trailing nulls left off.
//
// Parameter values: true/false -> bool, whole numbers -> int, numbers with a
// decimal point or exponent -> double, strings -> string, 2 numbers -> Vec2,
// 4 numbers -> Color. Saving always writes doubles with a decimal point, so
// types survive a save and load.
//
// Loading is strict: unknown keys, unknown node types, missing input nodes,
// cycles and files from a newer format version are all errors, reported
// with the file and node they occur in. Paths inside parameters (Read and
// Write files) are kept exactly as written; relative paths resolve against
// the working directory, not the project file.

inline constexpr int kProjectFormatVersion = 1;

std::string projectToJson(const Graph& graph);

// Just some of a graph's nodes, as a project (for copy and paste): the same
// format, with connections to nodes outside the set left unconnected.
std::string nodesToJson(const Graph& graph, const std::vector<const Node*>& nodes);

// `source` names the text in error messages (usually the file path).
Graph projectFromJson(const std::string& json, const std::string& source = "project");

// Writes to "<path>.partial" and renames into place, so an interrupted save
// never leaves a truncated project.
void saveProject(const Graph& graph, const std::filesystem::path& path);
Graph loadProject(const std::filesystem::path& path);

}  // namespace puke
