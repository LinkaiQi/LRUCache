// Concurrency tests.
//
// These lean on ThreadSanitizer to catch data races, so `make tsan` is where
// they earn their keep. What they assert directly are the invariants a race
// would break: values never get mixed up between keys, capacity is never
// exceeded, racing callers agree on one result, and the cache is still usable
// afterwards.

#include <lru/lru_cache.hpp>

#include <chrono>

#include "test_harness.hpp"

#include <atomic>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// Releases every worker at once. Without this the first thread can finish
// before the last one has started, and the operations never actually overlap.
class StartGate {
public:
    void wait() const {
        while (!m_open.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }

    void open() { m_open.store(true, std::memory_order_release); }

private:
    std::atomic<bool> m_open{false};
};

template <typename Body>
void run_threads(int count, Body body) {
    StartGate gate;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<std::size_t>(count));

    for (int t = 0; t < count; ++t) {
        threads.emplace_back([&gate, &body, t] {
            gate.wait();
            body(t);
        });
    }
    gate.open();

    for (std::thread& thread : threads) {
        thread.join();
    }
}

std::string value_for(int key) { return "v" + std::to_string(key); }

// ---------------------------------------------------------------------------
// get_or_compute
// ---------------------------------------------------------------------------

TEST(get_or_compute_racing_on_one_key_agrees_on_one_value) {
    constexpr int kThreads = 16;

    lru::LRUCache<int, std::string> cache(64);
    std::atomic<int> factory_calls{0};
    std::vector<std::string> results(static_cast<std::size_t>(kThreads));

    run_threads(kThreads, [&cache, &factory_calls, &results](int t) {
        results[static_cast<std::size_t>(t)] = cache.get_or_compute(1, [&factory_calls] {
            return "computed-" + std::to_string(factory_calls.fetch_add(1));
        });
    });

    // Losing threads discard their own result, so the factory may run more than
    // once, but only the first value to reach the cache is ever handed out.
    CHECK(factory_calls.load() >= 1);
    CHECK(factory_calls.load() <= kThreads);
    CHECK_EQ(cache.size(), std::size_t{1});

    const std::string& winner = results.front();
    CHECK(!winner.empty());
    for (const std::string& result : results) {
        CHECK_EQ(result, winner);
    }
    CHECK_EQ(cache.get(1), winner);
}

TEST(get_or_compute_under_eviction_never_mismatches_key_and_value) {
    constexpr int kThreads = 8;
    constexpr int kKeys = 400;
    constexpr std::size_t kCapacity = 32;  // far smaller than the key space

    lru::LRUCache<int, std::string> cache(kCapacity);
    std::atomic<bool> mismatched{false};

    run_threads(kThreads, [&cache, &mismatched](int) {
        for (int key = 0; key < kKeys; ++key) {
            const std::string value =
                cache.get_or_compute(key, [key] { return value_for(key); });
            if (value != value_for(key)) {
                mismatched = true;
            }
        }
    });

    CHECK(!mismatched.load());
    CHECK(cache.size() <= kCapacity);
}

TEST(get_or_compute_factories_can_touch_the_cache_from_many_threads) {
    // The factory runs without the lock held, so re-entering the cache from
    // inside it must not deadlock no matter how many threads do it at once.
    constexpr int kThreads = 8;

    lru::LRUCache<int, std::string> cache(128);
    run_threads(kThreads, [&cache](int t) {
        for (int i = 0; i < 100; ++i) {
            const int key = t * 100 + i;
            cache.get_or_compute(key, [&cache, key] {
                cache.contains(key - 1);
                cache.peek(key - 1);
                return value_for(key);
            });
        }
    });

    CHECK(cache.size() <= std::size_t{128});
    cache.put(-1, "alive");
    CHECK_EQ(cache.get(-1), "alive");
}

// ---------------------------------------------------------------------------
// Removal under load
// ---------------------------------------------------------------------------

TEST(erase_mixed_with_reads_and_writes_stays_consistent) {
    constexpr int kThreads = 8;
    constexpr int kOps = 4000;
    constexpr int kKeySpace = 128;
    constexpr std::size_t kCapacity = 32;

    lru::LRUCache<int, std::string> cache(kCapacity);
    std::atomic<bool> corrupted{false};

    run_threads(kThreads, [&cache, &corrupted](int t) {
        std::mt19937 rng(static_cast<unsigned>(t) + 101);
        for (int i = 0; i < kOps; ++i) {
            const int key = static_cast<int>(rng() % kKeySpace);
            const std::string expected = value_for(key);
            switch (rng() % 6) {
                case 0: {
                    cache.put(key, expected);
                    break;
                }
                case 1: {
                    const std::optional<std::string> value = cache.get(key);
                    if (value && *value != expected) {
                        corrupted = true;
                    }
                    break;
                }
                case 2: {
                    const std::optional<std::string> value = cache.peek(key);
                    if (value && *value != expected) {
                        corrupted = true;
                    }
                    break;
                }
                case 3: {
                    cache.erase(key);
                    break;
                }
                case 4: {
                    cache.contains(key);
                    break;
                }
                default: {
                    cache.insert_if_absent(key, expected);
                    break;
                }
            }
        }
    });

    CHECK(!corrupted.load());
    CHECK(cache.size() <= kCapacity);
}

TEST(clear_concurrent_with_traffic_is_safe) {
    // clear() frees every node while other threads are reading and writing,
    // which is the most destructive thing that can happen to the list.
    constexpr int kThreads = 8;
    constexpr int kOps = 3000;
    constexpr int kKeySpace = 64;

    lru::LRUCache<int, std::string> cache(32);
    std::atomic<bool> corrupted{false};

    run_threads(kThreads, [&cache, &corrupted](int t) {
        std::mt19937 rng(static_cast<unsigned>(t) + 7);
        for (int i = 0; i < kOps; ++i) {
            const int key = static_cast<int>(rng() % kKeySpace);
            const std::string expected = value_for(key);
            if (t == 0 && i % 250 == 0) {
                cache.clear();
            } else if (rng() % 2 == 0) {
                cache.put(key, expected);
            } else {
                const std::optional<std::string> value = cache.get(key);
                if (value && *value != expected) {
                    corrupted = true;
                }
            }
        }
    });

    CHECK(!corrupted.load());
    cache.put(1, "alive");
    CHECK_EQ(cache.get(1), "alive");
}

// ---------------------------------------------------------------------------
// Recency bookkeeping under contention
// ---------------------------------------------------------------------------

TEST(hot_key_set_hammers_move_to_front) {
    // A small key set with every thread reading a different one keeps the list
    // reordering constantly, which is the riskiest pointer work in the cache.
    constexpr int kThreads = 8;
    constexpr int kOps = 20000;
    constexpr int kHotKeys = 4;

    lru::LRUCache<int, std::string> cache(8);
    for (int key = 0; key < kHotKeys; ++key) {
        cache.put(key, value_for(key));
    }

    std::atomic<bool> corrupted{false};
    run_threads(kThreads, [&cache, &corrupted](int t) {
        for (int i = 0; i < kOps; ++i) {
            const int key = (i + t) % kHotKeys;
            const std::optional<std::string> value = cache.get(key);
            if (!value || *value != value_for(key)) {
                corrupted = true;
            }
        }
    });

    CHECK(!corrupted.load());
    CHECK_EQ(cache.size(), std::size_t{kHotKeys});
}

TEST(observers_are_safe_while_writers_run) {
    constexpr int kWriters = 4;
    constexpr int kReaders = 4;
    constexpr int kOps = 5000;
    constexpr std::size_t kCapacity = 16;

    lru::LRUCache<int, std::string> cache(kCapacity);
    std::atomic<bool> over_capacity{false};

    run_threads(kWriters + kReaders, [&cache, &over_capacity](int t) {
        if (t < kWriters) {
            for (int i = 0; i < kOps; ++i) {
                cache.put(i % 100, value_for(i % 100));
            }
            return;
        }
        for (int i = 0; i < kOps; ++i) {
            if (cache.size() > kCapacity) {
                over_capacity = true;
            }
            cache.capacity();
            cache.empty();
            cache.stats();
        }
    });

    CHECK(!over_capacity.load());
    CHECK(cache.size() <= kCapacity);
}

TEST(statistics_are_never_lost_under_contention) {
    // Every operation is locked, so the counters must add up exactly.
    constexpr int kThreads = 8;
    constexpr int kOps = 2000;

    lru::LRUCache<int, std::string> cache(1024);
    cache.put(1, "present");
    cache.reset_stats();

    run_threads(kThreads, [&cache](int t) {
        for (int i = 0; i < kOps; ++i) {
            cache.get(t % 2 == 0 ? 1 : -1);  // half hits, half misses
        }
    });

    const lru::CacheStats stats = cache.stats();
    const std::size_t total = stats.hits + stats.misses;
    CHECK_EQ(total, static_cast<std::size_t>(kThreads * kOps));
    CHECK_EQ(stats.hits, static_cast<std::size_t>(kThreads / 2 * kOps));
    CHECK_EQ(stats.misses, static_cast<std::size_t>(kThreads / 2 * kOps));
}

// ---------------------------------------------------------------------------
// Atomic operations and TTL under load
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

}  // namespace

int main() { return lru_test::run_all("concurrency"); }
