// Core behaviour: reading, writing, recency and eviction.

#include <lru/lru_cache.hpp>

#include "test_harness.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace {

using IntStringCache = lru::LRUCache<int, std::string>;

TEST(put_and_get_roundtrip) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    CHECK_EQ(cache.get(1), "one");
    CHECK_EQ(cache.get(2), "two");
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK(!cache.empty());
}

TEST(get_missing_key_returns_nullopt) {
    IntStringCache cache(2);
    CHECK_EQ(cache.get(42), std::nullopt);
    cache.put(1, "one");
    CHECK_EQ(cache.get(2), std::nullopt);
}

TEST(update_existing_key_overwrites_without_growing) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(1, "uno");
    CHECK_EQ(cache.size(), std::size_t{1});
    CHECK_EQ(cache.get(1), "uno");
}

TEST(evicts_least_recently_used) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");  // evicts key 1
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.get(2), "two");
    CHECK_EQ(cache.get(3), "three");
}

TEST(get_refreshes_recency) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    CHECK_EQ(cache.get(1), "one");  // 1 becomes most recently used
    cache.put(3, "three");          // so 2 is evicted, not 1
    CHECK_EQ(cache.get(2), std::nullopt);
    CHECK_EQ(cache.get(1), "one");
    CHECK_EQ(cache.get(3), "three");
}

TEST(put_refreshes_recency) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(1, "uno");  // refreshes key 1
    cache.put(3, "three");
    CHECK_EQ(cache.get(2), std::nullopt);
    CHECK_EQ(cache.get(1), "uno");
}

TEST(peek_does_not_refresh_recency) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    CHECK_EQ(cache.peek(1), "one");
    cache.put(3, "three");  // key 1 is still the LRU entry
    CHECK_EQ(cache.peek(1), std::nullopt);
    CHECK_EQ(cache.peek(2), "two");
}

TEST(contains_does_not_refresh_recency) {
    IntStringCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    CHECK(cache.contains(1));
    CHECK(!cache.contains(99));
    cache.put(3, "three");
    CHECK(!cache.contains(1));
}

TEST(capacity_one_evicts_every_insert) {
    IntStringCache cache(1);
    cache.put(1, "one");
    CHECK_EQ(cache.get(1), "one");
    cache.put(2, "two");
    CHECK_EQ(cache.size(), std::size_t{1});
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.get(2), "two");
    cache.put(2, "dos");
    CHECK_EQ(cache.size(), std::size_t{1});
    CHECK_EQ(cache.get(2), "dos");
}

TEST(capacity_is_never_exceeded) {
    IntStringCache cache(8);
    for (int i = 0; i < 1000; ++i) {
        cache.put(i, std::to_string(i));
        CHECK(cache.size() <= std::size_t{8});
    }
    CHECK_EQ(cache.size(), std::size_t{8});
    CHECK_EQ(cache.capacity(), std::size_t{8});
}

}  // namespace

int main() { return lru_test::run_all("basics"); }
