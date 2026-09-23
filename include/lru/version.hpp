// Version information for the lru_cache library.
#pragma once

#define LRU_CACHE_VERSION_MAJOR 1
#define LRU_CACHE_VERSION_MINOR 0
#define LRU_CACHE_VERSION_PATCH 0

#define LRU_CACHE_VERSION_STRING "1.0.0"

namespace lru {

struct Version {
    static constexpr int major = LRU_CACHE_VERSION_MAJOR;
    static constexpr int minor = LRU_CACHE_VERSION_MINOR;
    static constexpr int patch = LRU_CACHE_VERSION_PATCH;

    static constexpr const char* string() { return LRU_CACHE_VERSION_STRING; }
};

}  // namespace lru
