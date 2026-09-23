#include <lru/lru_cache.hpp>

#include "conformance.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using IntStringCache = lru::LRUCache<int, std::string>;

// ---------------------------------------------------------------------------
// Basic behaviour
// ---------------------------------------------------------------------------

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

TEST(zero_capacity_is_rejected) {
    bool threw = false;
    try {
        IntStringCache cache(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
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

// ---------------------------------------------------------------------------
// Removal paths: head, tail and middle of the intrusive list
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Value semantics
// ---------------------------------------------------------------------------

struct Tracked {
    static int copies;
    static int moves;
    static int constructions;
    static int destructions;

    int id = 0;

    explicit Tracked(int value) : id(value) { ++constructions; }
    Tracked(const Tracked& other) : id(other.id) {
        ++constructions;
        ++copies;
    }
    Tracked(Tracked&& other) noexcept : id(other.id) {
        ++constructions;
        ++moves;
    }
    Tracked& operator=(const Tracked& other) {
        id = other.id;
        ++copies;
        return *this;
    }
    Tracked& operator=(Tracked&& other) noexcept {
        id = other.id;
        ++moves;
        return *this;
    }
    ~Tracked() { ++destructions; }

    static void reset() {
        copies = 0;
        moves = 0;
        constructions = 0;
        destructions = 0;
    }
};

int Tracked::copies = 0;
int Tracked::moves = 0;
int Tracked::constructions = 0;
int Tracked::destructions = 0;

TEST(rvalue_put_moves_the_value) {
    Tracked::reset();
    lru::LRUCache<int, Tracked> cache(2);
    cache.put(1, Tracked(7));
    CHECK_EQ(Tracked::copies, 0);
    CHECK(Tracked::moves >= 1);
    const std::optional<Tracked> stored = cache.get(1);
    CHECK(stored.has_value());
    CHECK_EQ(stored->id, 7);
}

TEST(lvalue_put_copies_the_value) {
    Tracked::reset();
    lru::LRUCache<int, Tracked> cache(2);
    Tracked value(7);
    cache.put(1, value);
    CHECK(Tracked::copies >= 1);
    CHECK_EQ(value.id, 7);
}

TEST(evicted_and_erased_values_are_destroyed) {
    Tracked::reset();
    {
        lru::LRUCache<int, Tracked> cache(1);
        cache.put(1, Tracked(1));  // temporary destroyed, node keeps a moved copy
        cache.put(2, Tracked(2));  // evicts key 1
        cache.erase(2);
        CHECK(cache.empty());
    }
    // Nothing may outlive the cache: every construction has a matching destruction.
    CHECK_EQ(Tracked::destructions, Tracked::constructions);
}

TEST(string_keys_work) {
    lru::LRUCache<std::string, int> cache(2);
    cache.put("alpha", 1);
    cache.put("beta", 2);
    CHECK_EQ(cache.get("alpha"), 1);
    cache.put("gamma", 3);
    CHECK_EQ(cache.get("beta"), std::nullopt);
    CHECK_EQ(cache.get("alpha"), 1);
    CHECK_EQ(cache.get("gamma"), 3);
}

// ---------------------------------------------------------------------------
// Move semantics
// ---------------------------------------------------------------------------

TEST(move_construction_transfers_entries) {
    IntStringCache source(2);
    source.put(1, "one");
    source.put(2, "two");

    IntStringCache moved(std::move(source));
    CHECK_EQ(moved.size(), std::size_t{2});
    CHECK_EQ(moved.capacity(), std::size_t{2});
    CHECK_EQ(moved.get(1), "one");
    CHECK_EQ(moved.get(2), "two");

    // The moved-from cache is empty but still usable.
    CHECK(source.empty());  // NOLINT(bugprone-use-after-move)
    source.put(3, "three");
    CHECK_EQ(source.get(3), "three");
    CHECK_EQ(source.size(), std::size_t{1});
}

TEST(move_assignment_transfers_entries_and_frees_old_ones) {
    IntStringCache source(2);
    source.put(1, "one");
    source.put(2, "two");

    IntStringCache target(5);
    target.put(10, "ten");
    target.put(11, "eleven");

    target = std::move(source);
    CHECK_EQ(target.size(), std::size_t{2});
    CHECK_EQ(target.capacity(), std::size_t{2});
    CHECK_EQ(target.get(10), std::nullopt);
    CHECK_EQ(target.get(1), "one");

    // Eviction still respects the transferred capacity.
    target.put(3, "three");
    CHECK_EQ(target.size(), std::size_t{2});
}

TEST(self_move_assignment_is_safe) {
    IntStringCache cache(2);
    cache.put(1, "one");
    IntStringCache& alias = cache;
    cache = std::move(alias);
    CHECK_EQ(cache.size(), std::size_t{1});
    CHECK_EQ(cache.get(1), "one");
}

}  // namespace

int main() {
    lru_test::register_conformance_tests("production", [](std::size_t capacity) {
        return std::make_unique<IntStringCache>(capacity);
    });
    return lru_test::run_all("production LRUCache");
}
