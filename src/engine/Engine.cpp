#include "comp/engine/Engine.h"

#include <cstring>
#include <memory>
#include <type_traits>
#include <variant>
#include <unordered_map>
#include <vector>

#include "comp/engine/ThreadPool.h"
#include "comp/engine/TileCache.h"

namespace comp {
namespace {

struct Plan {
    std::vector<const Node*> order;  // inputs before consumers; the output is last
    std::unordered_map<const Node*, int> index;

    int indexOf(const Node* n) const { return index.at(n); }
};

// Iterative depth-first search, so deep graphs can't overflow the call stack.
Plan makePlan(const Node& output) {
    enum class Mark { Visiting, Done };
    struct Frame {
        const Node* node;
        int nextInput;
    };

    Plan plan;
    std::unordered_map<const Node*, Mark> marks{{&output, Mark::Visiting}};
    std::vector<Frame> stack{{&output, 0}};

    while (!stack.empty()) {
        const Node* node = stack.back().node;
        const int i = stack.back().nextInput;

        if (i < node->maxInputs()) {
            ++stack.back().nextInput;
            const Node* in = node->input(i);
            if (!in) {
                if (!node->inputOptional(i))
                    throw Error(node->name() + ": input " + std::to_string(i) + " is not connected");
                continue;
            }
            auto it = marks.find(in);
            if (it == marks.end()) {
                marks.emplace(in, Mark::Visiting);
                stack.push_back({in, 0});
            } else if (it->second == Mark::Visiting) {
                throw Error("cycle in graph: " + node->name() + " depends on " + in->name() +
                            ", which depends on " + node->name());
            }
        } else {
            marks[node] = Mark::Done;
            plan.index.emplace(node, static_cast<int>(plan.order.size()));
            plan.order.push_back(node);
            stack.pop_back();
        }
    }
    return plan;
}

// Runs a node hook, making sure any exception names the node it came from.
template <typename F>
auto inNode(const Node& node, const char* stage, F&& f) {
    try {
        return f();
    } catch (const Error&) {
        throw;  // our errors already carry the node name
    } catch (const std::exception& e) {
        throw Error(node.name() + ": " + stage + " failed: " + e.what());
    }
}

std::vector<ImageInfo> computeInfos(const Plan& plan, const RenderContext& ctx) {
    std::vector<ImageInfo> infos(plan.order.size());
    std::vector<const ImageInfo*> ins;
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        const Node* node = plan.order[i];
        ins.assign(node->maxInputs(), nullptr);
        for (int j = 0; j < node->maxInputs(); ++j)
            if (const Node* in = node->input(j)) ins[j] = &infos[plan.indexOf(in)];
        infos[i] = inNode(*node, "info", [&] { return node->computeInfo(ctx, ins); });
    }
    return infos;
}

// Floor division, so negative coordinates land in the right tile.
int floorDiv(int a, int b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// Copies all of `src` into `dst`, which must cover src.region().
void paste(Image& dst, const Image& src) {
    const Box& r = src.region();
    const std::size_t bytes = static_cast<std::size_t>(r.width()) * src.numChannels() * sizeof(float);
    for (int y = r.y0; y < r.y1; ++y) std::memcpy(dst.pixel(r.x0, y), src.pixel(r.x0, y), bytes);
}

void hashParam(Hasher& h, const ParamValue& v) {
    h.add(v.index());  // so 2 (int) and 2.0 (double) differ, as they may to a node
    std::visit(
        [&](const auto& x) {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::string>) {
                h.add(std::string_view(x));
            } else if constexpr (std::is_same_v<T, Vec2> || std::is_same_v<T, Color>) {
                for (double d : x) h.add(d);
            } else {
                h.add(x);
            }
        },
        v);
}

// Each node's cache key, bottom-up: the node's identity, type and
// parameters, the frame, its inputs' hashes, and whatever appendHash() adds.
std::vector<std::uint64_t> computeHashes(const Plan& plan, const RenderContext& ctx) {
    std::vector<std::uint64_t> hashes(plan.order.size());
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        const Node* node = plan.order[i];
        Hasher h;
        h.add(node->id()).add(std::string_view(node->typeName())).add(ctx.frame);
        h.add(node->params().size());
        for (const auto& [key, value] : node->params()) {  // std::map: sorted, so order is stable
            h.add(std::string_view(key));
            hashParam(h, value);
        }
        for (int j = 0; j < node->maxInputs(); ++j) {
            const Node* in = node->input(j);
            h.add(in ? hashes[plan.indexOf(in)] : std::uint64_t{0});
        }
        inNode(*node, "hash", [&] { node->appendHash(h, ctx); });
        hashes[i] = h.value();
    }
    return hashes;
}

}  // namespace

std::vector<Box> tilesOf(const Box& region, int tileSize) {
    std::vector<Box> tiles;
    if (region.empty()) return tiles;
    if (tileSize < 1) throw Error("tile size must be at least 1, not " + std::to_string(tileSize));
    const int tx0 = floorDiv(region.x0, tileSize), tx1 = floorDiv(region.x1 - 1, tileSize);
    const int ty0 = floorDiv(region.y0, tileSize), ty1 = floorDiv(region.y1 - 1, tileSize);
    tiles.reserve(static_cast<std::size_t>(tx1 - tx0 + 1) * (ty1 - ty0 + 1));
    for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx) {
            const Box cell{tx * tileSize, ty * tileSize, (tx + 1) * tileSize, (ty + 1) * tileSize};
            tiles.push_back(cell.intersect(region));
        }
    return tiles;
}

Engine::Engine(EngineOptions options)
    : options_(options), pool_(std::make_unique<ThreadPool>(options.threads)) {
    if (options_.tileSize < 1)
        throw Error("tile size must be at least 1, not " + std::to_string(options_.tileSize));
    if (options_.cacheBytes > 0) cache_ = std::make_unique<TileCache>(options_.cacheBytes);
}

Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

int Engine::threads() const { return pool_->threads(); }

void Engine::setCacheBytes(std::size_t bytes) {
    options_.cacheBytes = bytes;
    if (bytes == 0) cache_.reset();
    else if (cache_) cache_->setMaxBytes(bytes);
    else cache_ = std::make_unique<TileCache>(bytes);
}

void Engine::clearCache() {
    if (cache_) cache_->clear();
}

std::size_t Engine::cacheBytesUsed() const { return cache_ ? cache_->bytesUsed() : 0; }

ImageInfo Engine::info(const Node& output, const RenderContext& ctx) {
    const Plan plan = makePlan(output);
    return computeInfos(plan, ctx).back();
}

Image Engine::render(const Node& output, const RenderContext& ctx, const Box& region) {
    stats_ = {};
    const Plan plan = makePlan(output);
    const std::vector<ImageInfo> infos = computeInfos(plan, ctx);
    const std::vector<std::uint64_t> hashes =
        cache_ ? computeHashes(plan, ctx) : std::vector<std::uint64_t>(plan.order.size());
    const int n = static_cast<int>(plan.order.size());

    // Regions and cache lookups, top-down. Reverse topological order
    // guarantees every consumer of a node has added its request before that
    // node passes requests on.
    //
    // Each node's request is clipped to its data window: pixels outside it
    // are zero by definition, so there's nothing to compute. This is what
    // stops, say, a scaled-down image from asking its input for a region many
    // times larger than the input actually is.
    //
    // The request is then split into tiles and each tile looked up in the
    // cache. Only the tiles that missed ask anything of the node's inputs, so
    // a node whose tiles are all cached cuts off everything upstream of it.
    // Hits are held here, so eviction during the render can't lose them.
    std::vector<Box> regions(n);
    std::vector<std::vector<Box>> tiles(n);
    std::vector<std::vector<std::shared_ptr<const Image>>> hits(n);
    regions[n - 1] = region;
    for (int i = n - 1; i >= 0; --i) {
        const Node* node = plan.order[i];
        regions[i] = regions[i].intersect(infos[i].dataWindow);
        if (regions[i].empty()) continue;

        tiles[i] = node->supportsTiles() ? tilesOf(regions[i], options_.tileSize)
                                         : std::vector<Box>{regions[i]};
        hits[i].resize(tiles[i].size());
        Box misses;
        for (std::size_t t = 0; t < tiles[i].size(); ++t) {
            if (cache_) hits[i][t] = cache_->get({hashes[i], tiles[i][t]});
            if (!hits[i][t]) misses = misses.unite(tiles[i][t]);
        }
        if (misses.empty()) continue;

        for (int j = 0; j < node->maxInputs(); ++j) {
            const Node* in = node->input(j);
            if (!in) continue;
            const Box need = inNode(*node, "region", [&] { return node->inputRegion(ctx, j, misses); });
            const int k = plan.indexOf(in);
            regions[k] = regions[k].unite(need);
        }
    }

    // How many times each node's image will be read, so it can be freed early.
    std::vector<int> usesLeft(n, 0);
    for (const Node* node : plan.order)
        for (int j = 0; j < node->maxInputs(); ++j)
            if (const Node* in = node->input(j)) ++usesLeft[plan.indexOf(in)];

    // Compute, bottom-up.
    std::vector<Image> images(n);
    std::vector<const Image*> ins;
    for (int i = 0; i < n; ++i) {
        const Node* node = plan.order[i];
        const Box& nodeRegion = regions[i];
        if (nodeRegion.empty()) {
            images[i] = Image(Box{}, infos[i].channels);  // nothing needed from this node
        } else {
            ins.assign(node->maxInputs(), nullptr);
            for (int j = 0; j < node->maxInputs(); ++j)
                if (const Node* in = node->input(j)) ins[j] = &images[plan.indexOf(in)];

            // Runs compute() for one tile and checks it kept to the contract.
            auto computeTile = [&](const Box& tile) {
                Image img = inNode(*node, "render", [&] { return node->compute(ctx, tile, ins); });
                if (img.region() != tile || img.channels() != infos[i].channels)
                    throw Error(node->name() + ": " + node->typeName() +
                                " node returned a different region or channel list than requested");
                return img;
            };

            const std::vector<Box>& nodeTiles = tiles[i];
            long long missed = 0, missedPixels = 0;
            for (std::size_t t = 0; t < nodeTiles.size(); ++t)
                if (!hits[i][t]) ++missed, missedPixels += nodeTiles[t].area();

            if (!cache_ && nodeTiles.size() == 1) {
                images[i] = computeTile(nodeRegion);  // nothing to share or assemble
            } else {
                // Tiles cover disjoint pixels, so threads can paste without locking.
                Image out(nodeRegion, infos[i].channels);
                pool_->parallelFor(nodeTiles.size(), [&](std::size_t t) {
                    if (const auto& hit = hits[i][t]) {
                        paste(out, *hit);
                    } else if (cache_) {
                        auto img = std::make_shared<const Image>(computeTile(nodeTiles[t]));
                        cache_->put({hashes[i], nodeTiles[t]}, img);
                        paste(out, *img);
                    } else {
                        paste(out, computeTile(nodeTiles[t]));
                    }
                });
                images[i] = std::move(out);
            }
            hits[i].clear();  // release this node's hold on cached tiles

            stats_.tilesFromCache += static_cast<long long>(nodeTiles.size()) - missed;
            stats_.tilesComputed += missed;
            stats_.pixelsComputed += missedPixels;
            if (missed > 0) ++stats_.nodesComputed;
        }

        for (int j = 0; j < node->maxInputs(); ++j)
            if (const Node* in = node->input(j)) {
                const int k = plan.indexOf(in);
                if (--usesLeft[k] == 0) images[k] = Image{};
            }
    }
    // The output was clipped to its data window; give the caller exactly what
    // they asked for, zero-padded.
    Image result = std::move(images[n - 1]);
    if (result.region() != region) result = cropped(result, region);
    return result;
}

}  // namespace comp
