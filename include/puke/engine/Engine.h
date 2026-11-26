#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "puke/graph/Node.h"

namespace puke {

class Snapshot;
class ThreadPool;
class TileCache;

struct EngineOptions {
    // Threads used for rendering, including the calling thread.
    // 0 means one per hardware thread; 1 renders on the calling thread only.
    int threads = 0;

    // Tiles are squares of this size on a fixed grid anchored at pixel (0, 0),
    // so the same pixel always falls in the same tile (which the tile cache
    // relies on). Must be at least 1.
    int tileSize = 64;

    // Memory limit for cached tiles, in bytes. 0 turns the cache off.
    // Off by default: a render that moves through frames gets nothing from
    // it. Interactive use (a viewer re-rendering after each edit) does.
    std::size_t cacheBytes = 0;
};

struct RenderStats {
    int nodesComputed = 0;
    long long pixelsComputed = 0;
    long long tilesComputed = 0;   // compute() calls; an untiled node counts as one
    long long tilesFromCache = 0;  // tiles reused instead of computed
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
// Caching (when EngineOptions::cacheBytes > 0): every tile computed is kept
// in a memory-bounded LRU cache, keyed by a hash of the node (its id, type,
// parameters, the frame, its inputs' hashes and Node::appendHash) and the
// tile's pixel region. Before computing, the engine looks up each tile it
// needs; tiles found aren't computed, and nothing upstream is computed for
// them either. Editing a node changes its hash and every hash downstream of
// it, so only those tiles are recomputed. Stale tiles just age out.
//
// One Engine renders one thing at a time: don't call render() or info() on
// the same Engine from several threads at once. Separate Engines are fine.
//
// Editing during renders: render() works from a Snapshot of the graph (see
// Snapshot.h), taken on the editing thread.
//
// Planned next steps:
//   - run independent branches of the graph at the same time
class Engine {
public:
    explicit Engine(EngineOptions options = {});
    ~Engine();
    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;

    const EngineOptions& options() const { return options_; }
    int threads() const;  // actual thread count, with 0 resolved

    // Changes the cache limit; 0 turns the cache off and frees it.
    void setCacheBytes(std::size_t bytes);
    void clearCache();
    std::size_t cacheBytesUsed() const;

    // These take a Snapshot of `output` first, on the calling thread.
    ImageInfo info(const Node& output, const RenderContext& ctx);
    Image render(const Node& output, const RenderContext& ctx, const Box& region);

    // Render a snapshot taken earlier, typically on another thread: the
    // graph can be edited meanwhile without affecting the result.
    ImageInfo info(const Snapshot& snapshot, const RenderContext& ctx);
    Image render(const Snapshot& snapshot, const RenderContext& ctx, const Box& region);

    // A hash of everything that decides the output's pixels at this
    // context: the same as the engine's tile-cache key for the output
    // node. Equal keys mean equal images, so a caller can keep a rendered
    // frame across edits that don't change its key (an edit to a node that
    // isn't upstream, say). Cheap: no node computes anything.
    static std::uint64_t renderKey(const Snapshot& snapshot, const RenderContext& ctx);

    // Counters from the most recent render() call.
    const RenderStats& lastStats() const { return stats_; }

private:
    EngineOptions options_;
    std::unique_ptr<ThreadPool> pool_;
    std::unique_ptr<TileCache> cache_;  // null when caching is off
    RenderStats stats_;
};

}  // namespace puke
