#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace comp {

// Incremental 64-bit hash (FNV-1a, with a final mix to spread the bits).
// Used to build cache keys; not cryptographic. Values are hashed by their
// bytes, so the result is stable for one build on one machine, which is all
// an in-memory cache needs. Don't write these hashes to disk.
class Hasher {
public:
    Hasher& bytes(const void* data, std::size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0; i < size; ++i) {
            state_ ^= p[i];
            state_ *= 0x100000001b3ull;
        }
        return *this;
    }

    // Numbers, enums and other plain values.
    template <typename T>
        requires std::is_arithmetic_v<T> || std::is_enum_v<T>
    Hasher& add(T value) {
        return bytes(&value, sizeof value);
    }

    // Strings are length-prefixed, so ("ab", "c") and ("a", "bc") differ.
    Hasher& add(std::string_view s) {
        add(s.size());
        return bytes(s.data(), s.size());
    }

    std::uint64_t value() const {
        // splitmix64 finaliser
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

private:
    std::uint64_t state_ = 0xcbf29ce484222325ull;
};

}  // namespace comp
