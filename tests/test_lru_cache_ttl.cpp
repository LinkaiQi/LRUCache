// TTL and atomic-compound-operation tests for lru::LRUCache.
//
// Expiry is tested against a manually driven clock, so the suite is
// deterministic and never sleeps. One test uses the real steady_clock purely to
// confirm the default wiring.

#include <lru/lru_cache.hpp>

#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// A clock the tests drive by hand. Single-threaded use only.
struct ManualClock {
    using duration = std::chrono::milliseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<ManualClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept { return s_now; }
    static void advance(duration by) { s_now += by; }
    static void reset() { s_now = time_point{duration{0}}; }

    static time_point s_now;
};

ManualClock::time_point ManualClock::s_now{ManualClock::duration{0}};

static_assert(ManualClock::is_steady, "a cache clock must be monotonic");

using TestCache = lru::LRUCache<int, std::string, std::hash<int>, std::equal_to<int>, ManualClock>;
using Ms = std::chrono::milliseconds;

TestCache make_cache(std::size_t capacity, std::optional<Ms> ttl = std::nullopt) {
    ManualClock::reset();
    return TestCache(capacity, ttl);
}

// ---------------------------------------------------------------------------
// TTL basics
// ---------------------------------------------------------------------------

TEST(without_a_ttl_entries_never_expire) {
    auto cache = make_cache(4);
    cache.put(1, "one");
    ManualClock::advance(Ms{1000000});
    CHECK_EQ(cache.get(1), "one");
    CHECK(cache.contains(1));
}

TEST(entry_expires_after_the_default_ttl) {
    auto cache = make_cache(4, Ms{100});
    cache.put(1, "one");

    ManualClock::advance(Ms{99});
    CHECK_EQ(cache.get(1), "one");  // still inside the window

    ManualClock::advance(Ms{1});  // exactly at the deadline
    CHECK_EQ(cache.get(1), std::nullopt);
}

TEST(expiry_is_inclusive_of_the_deadline) {
    auto cache = make_cache(4, Ms{10});
    cache.put(1, "one");
    ManualClock::advance(Ms{10});
    CHECK(!cache.contains(1));
}

TEST(per_entry_ttl_overrides_the_default) {
    auto cache = make_cache(4, Ms{100});
    cache.put(1, "short", Ms{10});
    cache.put(2, "long", Ms{1000});
    cache.put(3, "default");

    ManualClock::advance(Ms{50});
    CHECK_EQ(cache.get(1), std::nullopt);  // 10ms TTL has passed
    CHECK_EQ(cache.get(2), "long");
    CHECK_EQ(cache.get(3), "default");  // 100ms default has not

    ManualClock::advance(Ms{100});
    CHECK_EQ(cache.get(3), std::nullopt);
    CHECK_EQ(cache.get(2), "long");
}

TEST(nullopt_ttl_pins_an_entry_in_a_ttl_cache) {
    auto cache = make_cache(4, Ms{10});
    cache.put(1, "permanent", std::nullopt);
    cache.put(2, "temporary");

    ManualClock::advance(Ms{1000});
    CHECK_EQ(cache.get(1), "permanent");
    CHECK_EQ(cache.get(2), std::nullopt);
}

TEST(put_refreshes_the_expiry) {
    auto cache = make_cache(4, Ms{100});
    cache.put(1, "one");
    ManualClock::advance(Ms{90});
    cache.put(1, "one again");  // restarts the clock for this entry
    ManualClock::advance(Ms{90});
    CHECK_EQ(cache.get(1), "one again");
    ManualClock::advance(Ms{10});
    CHECK_EQ(cache.get(1), std::nullopt);
}

TEST(get_does_not_extend_the_expiry) {
    auto cache = make_cache(4, Ms{100});
    cache.put(1, "one");
    for (int i = 0; i < 9; ++i) {
        ManualClock::advance(Ms{10});
        CHECK_EQ(cache.get(1), "one");  // reads keep it most recently used...
    }
    ManualClock::advance(Ms{10});
    CHECK_EQ(cache.get(1), std::nullopt);  // ...but do not postpone the deadline
}

TEST(expired_entry_can_be_overwritten_by_put) {
    auto cache = make_cache(4, Ms{10});
    cache.put(1, "old");
    ManualClock::advance(Ms{20});
    cache.put(1, "new");
    CHECK_EQ(cache.get(1), "new");
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(non_positive_ttl_is_rejected) {
    bool ctor_threw = false;
    try {
        TestCache cache(4, Ms{0});
    } catch (const std::invalid_argument&) {
        ctor_threw = true;
    }
    CHECK(ctor_threw);

    bool ctor_threw_negative = false;
    try {
        TestCache cache(4, Ms{-1});
    } catch (const std::invalid_argument&) {
        ctor_threw_negative = true;
    }
    CHECK(ctor_threw_negative);

    auto cache = make_cache(4);
    bool put_threw = false;
    try {
        cache.put(1, "one", Ms{0});
    } catch (const std::invalid_argument&) {
        put_threw = true;
    }
    CHECK(put_threw);
    // The rejected put must not have changed anything.
    CHECK(cache.empty());
}

// ---------------------------------------------------------------------------
// How expiry interacts with the rest of the API
// ---------------------------------------------------------------------------

TEST(get_removes_the_expired_entry_it_reports) {
    auto cache = make_cache(4, Ms{10});
    cache.put(1, "one");
    ManualClock::advance(Ms{20});
    CHECK_EQ(cache.size(), std::size_t{1});  // still occupying a slot
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.size(), std::size_t{0});  // the failed read reclaimed it
}

TEST(peek_reports_expired_as_absent_without_removing) {
    auto cache = make_cache(4, Ms{10});
    cache.put(1, "one");
    ManualClock::advance(Ms{20});
    CHECK_EQ(cache.peek(1), std::nullopt);
    CHECK_EQ(cache.size(), std::size_t{1});  // peek is const, so nothing is reclaimed
    CHECK(!cache.contains(1));
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(expired_entries_do_not_count_as_hits) {
    auto cache = make_cache(4, Ms{10});
    cache.put(1, "one");
    ManualClock::advance(Ms{20});
    cache.get(1);

    const lru::CacheStats stats = cache.stats();
    CHECK_EQ(stats.hits, std::size_t{0});
    CHECK_EQ(stats.misses, std::size_t{1});
    CHECK_EQ(stats.expirations, std::size_t{1});
}

TEST(purge_expired_reclaims_untouched_entries) {
    auto cache = make_cache(10, Ms{100});
    for (int i = 0; i < 5; ++i) {
        cache.put(i, "short");
    }
    ManualClock::advance(Ms{50});
    for (int i = 5; i < 10; ++i) {
        cache.put(i, "long");
    }

    ManualClock::advance(Ms{60});  // the first five are now past their deadline
    CHECK_EQ(cache.size(), std::size_t{10});

    CHECK_EQ(cache.purge_expired(), std::size_t{5});
    CHECK_EQ(cache.size(), std::size_t{5});
    CHECK_EQ(cache.purge_expired(), std::size_t{0});  // idempotent

    for (int i = 0; i < 5; ++i) {
        CHECK_EQ(cache.peek(i), std::nullopt);
    }
    for (int i = 5; i < 10; ++i) {
        CHECK_EQ(cache.peek(i), "long");
    }
}

TEST(purge_expired_keeps_the_list_usable) {
    auto cache = make_cache(6, Ms{100});
    cache.put(1, "a", Ms{10});   // will expire: head region
    cache.put(2, "b");           // survives
    cache.put(3, "c", Ms{10});   // will expire: middle
    cache.put(4, "d");           // survives
    cache.put(5, "e", Ms{10});   // will expire: most recent

    ManualClock::advance(Ms{20});
    CHECK_EQ(cache.purge_expired(), std::size_t{3});
    CHECK_EQ(cache.size(), std::size_t{2});

    // Recency order among the survivors must be intact: 4 is newer than 2, so
    // 2 is the first to go. Two survivors plus five inserts overflows capacity 6
    // by one, which forces exactly one eviction.
    cache.put(6, "f");
    cache.put(7, "g");
    cache.put(8, "h");
    cache.put(9, "i");
    cache.put(10, "j");
    CHECK_EQ(cache.size(), std::size_t{6});
    CHECK_EQ(cache.peek(2), std::nullopt);
    CHECK_EQ(cache.peek(4), "d");
}

TEST(purge_expired_on_an_empty_cache) {
    auto cache = make_cache(4, Ms{10});
    CHECK_EQ(cache.purge_expired(), std::size_t{0});
    cache.put(1, "one");
    CHECK_EQ(cache.purge_expired(), std::size_t{0});
    CHECK_EQ(cache.get(1), "one");
}

TEST(expired_entries_still_occupy_capacity_until_reclaimed) {
    auto cache = make_cache(2, Ms{10});
    cache.put(1, "one");
    cache.put(2, "two");
    ManualClock::advance(Ms{20});

    // Both are expired but still stored, so inserting evicts the LRU one as usual.
    cache.put(3, "three");
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK_EQ(cache.get(3), "three");
    CHECK_EQ(cache.get(1), std::nullopt);
}

// ---------------------------------------------------------------------------
// Atomic compound operations
// ---------------------------------------------------------------------------

TEST(insert_if_absent_only_inserts_when_missing) {
    auto cache = make_cache(4);
    CHECK(cache.insert_if_absent(1, "first"));
    CHECK(!cache.insert_if_absent(1, "second"));
    CHECK_EQ(cache.get(1), "first");
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(insert_if_absent_replaces_an_expired_entry) {
    auto cache = make_cache(4, Ms{10});
    CHECK(cache.insert_if_absent(1, "old"));
    CHECK(!cache.insert_if_absent(1, "ignored"));

    ManualClock::advance(Ms{20});
    CHECK(cache.insert_if_absent(1, "fresh"));
    CHECK_EQ(cache.get(1), "fresh");
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(get_or_compute_computes_only_when_absent) {
    auto cache = make_cache(4);
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

TEST(get_or_compute_recomputes_after_expiry) {
    auto cache = make_cache(4, Ms{10});
    int calls = 0;
    const auto factory = [&calls] { return "value " + std::to_string(++calls); };

    CHECK_EQ(cache.get_or_compute(1, factory), "value 1");
    ManualClock::advance(Ms{20});
    CHECK_EQ(cache.get_or_compute(1, factory), "value 2");
    CHECK_EQ(calls, 2);
    CHECK_EQ(cache.size(), std::size_t{1});
}

TEST(get_or_compute_honours_an_explicit_ttl) {
    auto cache = make_cache(4);
    const auto factory = [] { return std::string("value"); };

    CHECK_EQ(cache.get_or_compute(1, factory, Ms{10}), "value");
    ManualClock::advance(Ms{5});
    CHECK_EQ(cache.peek(1), "value");
    ManualClock::advance(Ms{5});
    CHECK_EQ(cache.peek(1), std::nullopt);
}

TEST(get_or_compute_does_not_hold_the_lock_during_the_factory) {
    auto cache = make_cache(4);
    // A factory that touches the same cache would deadlock if the lock were
    // held across the call.
    const auto factory = [&cache] {
        cache.put(99, "written from inside the factory");
        return std::string("outer");
    };
    CHECK_EQ(cache.get_or_compute(1, factory), "outer");
    CHECK_EQ(cache.get(99), "written from inside the factory");
}

// ---------------------------------------------------------------------------
// Concurrency
// ---------------------------------------------------------------------------

TEST(insert_if_absent_elects_exactly_one_winner) {
    // The race insert_if_absent exists to fix: with contains()+put() several
    // threads would each believe they were first.
    lru::LRUCache<int, int> cache(256);
    constexpr int kThreads = 8;
    constexpr int kKeys = 200;

    std::vector<std::thread> threads;
    std::atomic<int> winners{0};
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, &winners, t] {
            for (int key = 0; key < kKeys; ++key) {
                if (cache.insert_if_absent(key, t)) {
                    ++winners;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    // Capacity is larger than the key space, so nothing is evicted and exactly
    // one thread can have won each key.
    CHECK_EQ(winners.load(), kKeys);
}

TEST(concurrent_ttl_traffic_is_safe) {
    lru::LRUCache<int, std::string> cache(64, std::chrono::milliseconds(2));
    constexpr int kThreads = 8;
    constexpr int kOpsPerThread = 3000;

    std::vector<std::thread> threads;
    std::atomic<bool> corrupted{false};
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&cache, &corrupted, t] {
            for (int i = 0; i < kOpsPerThread; ++i) {
                const int key = (i * 7 + t) % 128;
                const std::string expected = "value-" + std::to_string(key);
                switch (i % 4) {
                    case 0:
                        cache.put(key, expected);
                        break;
                    case 1:
                        if (auto value = cache.get(key); value && *value != expected) {
                            corrupted = true;
                        }
                        break;
                    case 2:
                        cache.insert_if_absent(key, expected);
                        break;
                    default:
                        cache.purge_expired();
                        break;
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    CHECK(!corrupted.load());
    CHECK(cache.size() <= std::size_t{64});
}

// ---------------------------------------------------------------------------
// Default clock wiring
// ---------------------------------------------------------------------------

TEST(real_steady_clock_expiry) {
    lru::LRUCache<int, std::string> cache(4, std::chrono::milliseconds(20));
    cache.put(1, "one");
    CHECK_EQ(cache.get(1), "one");
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(cache.get(1), std::nullopt);
}

}  // namespace

int main() { return lru_test::run_all("LRUCache TTL"); }
