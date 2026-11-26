#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "puke/core/Box.h"
#include "puke/core/Image.h"

namespace puke {

// Identifies one cached block of pixels: which node state produced it
// (see Engine for how the hash is built) and which pixels it covers.
struct TileKey {
    std::uint64_t nodeHash = 0;
    Box region;

    friend bool operator==(const TileKey&, const TileKey&) = default;
};

struct TileKeyHash {
    std::size_t operator()(const TileKey& k) const;
};

// A thread-safe, memory-bounded LRU cache of rendered tiles.
//
// Tiles are stored as shared_ptr<const Image>, so a tile handed out by get()
// stays valid even if the cache evicts it a moment later.
class TileCache {
public:
    explicit TileCache(std::size_t maxBytes) : maxBytes_(maxBytes) {}

    // The cached tile, or nullptr. A hit marks the tile as recently used.
    std::shared_ptr<const Image> get(const TileKey& key);

    // Stores a tile, evicting the least recently used ones to stay under the
    // limit. Tiles bigger than the whole limit aren't stored.
    void put(const TileKey& key, std::shared_ptr<const Image> image);

    void clear();
    void setMaxBytes(std::size_t maxBytes);  // evicts if now over the limit

    std::size_t maxBytes() const;
    std::size_t bytesUsed() const;
    std::size_t size() const;  // number of tiles

    static std::size_t bytesOf(const Image& image) {
        return static_cast<std::size_t>(image.region().area()) * image.numChannels() * sizeof(float);
    }

private:
    struct Entry {
        TileKey key;
        std::shared_ptr<const Image> image;
        std::size_t bytes;
    };
    using List = std::list<Entry>;  // front = most recently used

    void evictToFit();  // caller holds mutex_

    mutable std::mutex mutex_;
    std::size_t maxBytes_;
    std::size_t bytesUsed_ = 0;
    List lru_;
    std::unordered_map<TileKey, List::iterator, TileKeyHash> index_;
};

}  // namespace puke
