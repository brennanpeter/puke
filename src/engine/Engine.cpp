#include "comp/engine/Engine.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include "comp/engine/ThreadPool.h"

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
}

Engine::~Engine() = default;
Engine::Engine(Engine&&) noexcept = default;
Engine& Engine::operator=(Engine&&) noexcept = default;

int Engine::threads() const { return pool_->threads(); }

ImageInfo Engine::info(const Node& output, const RenderContext& ctx) {
    const Plan plan = makePlan(output);
    return computeInfos(plan, ctx).back();
}

Image Engine::render(const Node& output, const RenderContext& ctx, const Box& region) {
    stats_ = {};
    const Plan plan = makePlan(output);
    const std::vector<ImageInfo> infos = computeInfos(plan, ctx);
    const int n = static_cast<int>(plan.order.size());

    // Regions, top-down. Reverse topological order guarantees every consumer
    // of a node has added its request before that node passes requests on.
    // Each node's request is clipped to its data window before being passed on:
    // pixels outside it are zero by definition, so there's nothing to compute.
    // This is what stops, say, a scaled-down image from asking its input for
    // a region many times larger than the input actually is.
    std::vector<Box> regions(n);
    regions[n - 1] = region;
    for (int i = n - 1; i >= 0; --i) {
        const Node* node = plan.order[i];
        regions[i] = regions[i].intersect(infos[i].dataWindow);
        if (regions[i].empty()) continue;
        for (int j = 0; j < node->maxInputs(); ++j) {
            const Node* in = node->input(j);
            if (!in) continue;
            const Box need = inNode(*node, "region", [&] { return node->inputRegion(ctx, j, regions[i]); });
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
        if (regions[i].empty()) {
            images[i] = Image(Box{}, infos[i].channels);  // nothing needed from this node
        } else {
            ins.assign(node->maxInputs(), nullptr);
            for (int j = 0; j < node->maxInputs(); ++j)
                if (const Node* in = node->input(j)) ins[j] = &images[plan.indexOf(in)];

            const Box& nodeRegion = regions[i];
            const std::vector<Box> tiles =
                node->supportsTiles() ? tilesOf(nodeRegion, options_.tileSize) : std::vector<Box>{nodeRegion};

            // Runs compute() for one tile and checks it kept to the contract.
            auto computeTile = [&](const Box& tile) {
                Image img = inNode(*node, "render", [&] { return node->compute(ctx, tile, ins); });
                if (img.region() != tile || img.channels() != infos[i].channels)
                    throw Error(node->name() + ": " + node->typeName() +
                                " node returned a different region or channel list than requested");
                return img;
            };

            if (tiles.size() == 1) {
                images[i] = computeTile(nodeRegion);
            } else {
                // Tiles cover disjoint pixels, so threads can paste without locking.
                Image out(nodeRegion, infos[i].channels);
                pool_->parallelFor(tiles.size(), [&](std::size_t t) { paste(out, computeTile(tiles[t])); });
                images[i] = std::move(out);
            }

            stats_.tilesComputed += static_cast<long long>(tiles.size());
            ++stats_.nodesComputed;
            stats_.pixelsComputed += regions[i].area();
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
