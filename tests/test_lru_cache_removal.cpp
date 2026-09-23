// Removal paths through the intrusive list: head, tail, middle, only entry,
// and wiping the whole thing.

#include <lru/lru_cache.hpp>

#include "test_harness.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace {

using IntStringCache = lru::LRUCache<int, std::string>;

TEST(erase_removes_entry_and_reports_it) {
    IntStringCache cache(4);
    cache.put(1, "one");
    CHECK(cache.erase(1));
    CHECK(!cache.erase(1));
    CHECK_EQ(cache.size(), std::size_t{0});
    CHECK(cache.empty());
    CHECK_EQ(cache.get(1), std::nullopt);
}

TEST(erase_head_keeps_list_consistent) {
    IntStringCache cache(3);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");  // list: 3 (MRU), 2, 1 (LRU)
    CHECK(cache.erase(3));
    cache.put(4, "four");
    cache.put(5, "five");  // evicts key 1
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.get(2), "two");
    CHECK_EQ(cache.get(4), "four");
    CHECK_EQ(cache.get(5), "five");
}

TEST(erase_tail_keeps_list_consistent) {
    IntStringCache cache(3);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK(cache.erase(1));  // erase the LRU entry
    cache.put(4, "four");
    CHECK_EQ(cache.size(), std::size_t{3});
    cache.put(5, "five");  // evicts key 2, now the LRU entry
    CHECK_EQ(cache.get(2), std::nullopt);
    CHECK_EQ(cache.get(3), "three");
}

TEST(erase_middle_keeps_list_consistent) {
    IntStringCache cache(3);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK(cache.erase(2));
    cache.put(4, "four");
    cache.put(5, "five");  // evicts key 1
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.get(3), "three");
    CHECK_EQ(cache.get(4), "four");
}

TEST(erase_only_entry_then_reuse) {
    IntStringCache cache(2);
    cache.put(1, "one");
    CHECK(cache.erase(1));
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK_EQ(cache.get(2), "two");
    CHECK_EQ(cache.get(3), "three");
}

TEST(clear_empties_cache_and_allows_reuse) {
    IntStringCache cache(3);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.clear();
    CHECK(cache.empty());
    CHECK_EQ(cache.size(), std::size_t{0});
    CHECK_EQ(cache.get(1), std::nullopt);
    cache.put(3, "three");
    CHECK_EQ(cache.get(3), "three");
    CHECK_EQ(cache.capacity(), std::size_t{3});
}

}  // namespace

int main() { return lru_test::run_all("removal"); }
