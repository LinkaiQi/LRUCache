// How the cache treats the values it stores: what it copies, what it moves,
// and what it destroys. Also covers moving the cache itself.

#include <lru/lru_cache.hpp>

#include "test_harness.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace {

using IntStringCache = lru::LRUCache<int, std::string>;

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

int main() { return lru_test::run_all("value semantics"); }
