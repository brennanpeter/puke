#include "comp/engine/TileCache.h"

#include "comp/core/Hash.h"

namespace comp {

std::size_t TileKeyHash::operator()(const TileKey& k) const {
    Hasher h;
    h.add(k.nodeHash).add(k.region.x0).add(k.region.y0).add(k.region.x1).add(k.region.y1);
    return static_cast<std::size_t>(h.value());
}

std::shared_ptr<const Image> TileCache::get(const TileKey& key) {
    std::lock_guard lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) return nullptr;
    lru_.splice(lru_.begin(), lru_, it->second);  // mark as most recently used
    return it->second->image;
}

void TileCache::put(const TileKey& key, std::shared_ptr<const Image> image) {
    if (!image) return;
    const std::size_t bytes = bytesOf(*image);

    std::lock_guard lock(mutex_);
    if (bytes > maxBytes_) return;
    if (auto it = index_.find(key); it != index_.end()) {
        // Same key means same pixels; just refresh its position.
        lru_.splice(lru_.begin(), lru_, it->second);
        return;
    }
    lru_.push_front({key, std::move(image), bytes});
    index_.emplace(key, lru_.begin());
    bytesUsed_ += bytes;
    evictToFit();
}

void TileCache::evictToFit() {
    while (bytesUsed_ > maxBytes_ && !lru_.empty()) {
        bytesUsed_ -= lru_.back().bytes;
        index_.erase(lru_.back().key);
        lru_.pop_back();
    }
}

void TileCache::clear() {
    std::lock_guard lock(mutex_);
    lru_.clear();
    index_.clear();
    bytesUsed_ = 0;
}

void TileCache::setMaxBytes(std::size_t maxBytes) {
    std::lock_guard lock(mutex_);
    maxBytes_ = maxBytes;
    evictToFit();
}

std::size_t TileCache::maxBytes() const {
    std::lock_guard lock(mutex_);
    return maxBytes_;
}

std::size_t TileCache::bytesUsed() const {
    std::lock_guard lock(mutex_);
    return bytesUsed_;
}

std::size_t TileCache::size() const {
    std::lock_guard lock(mutex_);
    return lru_.size();
}

}  // namespace comp
