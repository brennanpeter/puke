#pragma once

#include <memory>

#include "comp/graph/Node.h"

namespace comp {

class ThreadPool;

struct EngineOptions {
    // Threads used for rendering, including the calling thread.
    // 0 means one per hardware thread; 1 renders on the calling thread only.
    int threads = 0;

    // Tiles are squares of this size on a fixed grid anchored at pixel (0, 0),
    // so the same pixel always falls in the same tile (which a tile cache
    // will rely on). Must be at least 1.
    int tileSize = 64;
};

struct RenderStats {
    int nodesComputed = 0;
    long long pixelsComputed = 0;
    long long tilesComputed = 0;  // compute() calls; an untiled node counts as one
};

// Splits `region` along the tile grid, row by row, top to bottom.
std::vector<Box> tilesOf(const Box& region, int tileSize);

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
// Threading: nodes are computed one at a time, in order. Each node's region
// is split into tiles (see EngineOptions::tileSize) and its tiles run in
// parallel on the engine's thread pool. Nodes that opt out of tiling
// (Node::supportsTiles) get their whole region in one call. Results are
// identical for any thread count or tile size.
//
// One Engine renders one thing at a time: don't call render() or info() on
// the same Engine from several threads at once. Separate Engines are fine.
//
// Uncached for now. Planned next steps:
//   - hash-based tile cache keyed on (node params, input hashes, frame)
//   - render from an immutable snapshot so a GUI can edit during renders
//   - run independent branches of the graph at the same time
class Engine {
public:
    explicit Engine(EngineOptions options = {});
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    const EngineOptions& options() const { return options_; }
    int threads() const;  // actual thread count, with 0 resolved

    ImageInfo info(const Node& output, const RenderContext& ctx);
    Image render(const Node& output, const RenderContext& ctx, const Box& region);

    // Counters from the most recent render() call.
    const RenderStats& lastStats() const { return stats_; }

private:
    EngineOptions options_;
    std::unique_ptr<ThreadPool> pool_;
    RenderStats stats_;
};

}  // namespace comp
