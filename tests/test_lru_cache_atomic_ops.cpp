// insert_if_absent and get_or_compute, the operations that exist so callers do
// not have to build racy check-then-act sequences out of contains() and put().
// Their behaviour under threads lives in test_lru_cache_concurrency.cpp.

#include <lru/lru_cache.hpp>

#include "test_harness.hpp"

#include <cstddef>
#include <optional>
#include <string>

namespace {

TEST(insert_if_absent_only_inserts_when_missing) {
    lru::LRUCache<int, std::string> cache(4);
    CHECK(cache.insert_if_absent(1, "first"));
    CHECK(!cache.insert_if_absent(1, "second"));
    CHECK_EQ(cache.get(1), "first");
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(get_or_compute_computes_only_when_absent) {
    lru::LRUCache<int, std::string> cache(4);
    int calls = 0;
    const auto factory = [&calls] {
        ++calls;
        return std::string("computed");
    };

    CHECK_EQ(cache.get_or_compute(1, factory), "computed");
    CHECK_EQ(calls, 1);

    CHECK_EQ(cache.get_or_compute(1, factory), "computed");
    CHECK_EQ(calls, 1);  // served from the cache
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(get_or_compute_does_not_hold_the_lock_during_the_factory) {
    lru::LRUCache<int, std::string> cache(4);
    // A factory that touches the same cache would deadlock if the lock were
    // held across the call.
    const auto factory = [&cache] {
        cache.put(99, "written from inside the factory");
        return std::string("outer");
    };
    CHECK_EQ(cache.get_or_compute(1, factory), "outer");
    CHECK_EQ(cache.get(99), "written from inside the factory");
}

}  // namespace

int main() { return lru_test::run_all("atomic operations"); }
