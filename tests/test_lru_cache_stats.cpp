// Hit, miss, eviction and expiration counters.

#include <lru/lru_cache.hpp>

#include "test_harness.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace {

using IntStringCache = lru::LRUCache<int, std::string>;

TEST(stats_track_hits_misses_and_evictions) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    CHECK_EQ(cache.get(1), "one");          // hit
    CHECK_EQ(cache.get(99), std::nullopt);  // miss
    cache.put(3, "three");                  // evicts key 2

    const lru::CacheStats stats = cache.stats();
    CHECK_EQ(stats.hits, std::size_t{1});
    CHECK_EQ(stats.misses, std::size_t{1});
    CHECK_EQ(stats.evictions, std::size_t{1});

    cache.reset_stats();
    const lru::CacheStats cleared = cache.stats();
    CHECK_EQ(cleared.hits, std::size_t{0});
    CHECK_EQ(cleared.misses, std::size_t{0});
    CHECK_EQ(cleared.evictions, std::size_t{0});
}

TEST(peek_does_not_change_stats) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.peek(1);
    cache.peek(2);
    const lru::CacheStats stats = cache.stats();
    CHECK_EQ(stats.hits, std::size_t{0});
    CHECK_EQ(stats.misses, std::size_t{0});
}

}  // namespace

int main() { return lru_test::run_all("statistics"); }
