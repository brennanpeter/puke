#pragma once

#include "comp/graph/Node.h"

namespace comp {

struct RenderStats {
    int nodesComputed = 0;
    long long pixelsComputed = 0;
};

// Evaluates node graphs, pull-based and region-driven.
//
// render(output, ctx, region) runs in four passes:
//   1. Plan:    collect every node upstream of `output` in topological order,
//               checking for cycles and unconnected required inputs.
//   2. Info:    bottom-up, ask each node what it produces (computeInfo).
//   3. Regions: top-down, ask each node which input regions it needs
//               (inputRegion). A node feeding several consumers gets the
//               union of their requests, so it is computed only once.
//               Every request is clipped to the node's data window.
//   4. Compute: bottom-up, compute each node's region exactly once. Images
//               are freed as soon as their last consumer has run.
//
// Single-threaded and uncached for now. Planned next steps:
//   - split the compute pass into tiles and run them on a thread pool
//   - hash-based tile cache keyed on (node params, input hashes, frame)
//   - render from an immutable snapshot so a GUI can edit during renders
class Engine {
public:
    ImageInfo info(const Node& output, const RenderContext& ctx);
    Image render(const Node& output, const RenderContext& ctx, const Box& region);

    // Counters from the most recent render() call.
    const RenderStats& lastStats() const { return stats_; }

private:
    RenderStats stats_;
};

}  // namespace comp
