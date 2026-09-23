// Behaviour every LRU cache in this repository must exhibit, written against the
// smallest common API: put(int, std::string) and get(int) -> optional<string>.
//
// Both the classic and the production cache are registered against this suite,
// which is what guarantees the two versions are observably identical.
#pragma once

#include "test_harness.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lru_test {

// Deliberately naive LRU used as the oracle: a vector with the most recently
// used entry at the front. Obviously correct, far too slow to ship.
class ReferenceLRU {
public:
    explicit ReferenceLRU(std::size_t capacity) : m_capacity(capacity) {}

    void put(int key, const std::string& value) {
        auto it = find(key);
        if (it != m_entries.end()) {
            it->second = value;
            std::rotate(m_entries.begin(), it, it + 1);
            return;
        }
        if (m_entries.size() >= m_capacity) {
            m_entries.pop_back();
        }
        m_entries.insert(m_entries.begin(), {key, value});
    }

    std::optional<std::string> get(int key) {
        auto it = find(key);
        if (it == m_entries.end()) {
            return std::nullopt;
        }
        std::rotate(m_entries.begin(), it, it + 1);
        return m_entries.front().second;
    }

    std::size_t size() const { return m_entries.size(); }

private:
    using Entry = std::pair<int, std::string>;

    std::vector<Entry>::iterator find(int key) {
        return std::find_if(m_entries.begin(), m_entries.end(),
                            [key](const Entry& entry) { return entry.first == key; });
    }

    std::size_t m_capacity;
    std::vector<Entry> m_entries;  // front = most recently used
};

// Factory returns std::unique_ptr so this works for caches that are not movable.
template <typename Factory>
void register_conformance_tests(const std::string& prefix, Factory make) {
    const auto name = [&prefix](const char* test) { return prefix + "/" + test; };

    add_test(name("put_and_get_roundtrip"), [make] {
        auto cache = make(2);
        cache->put(1, "one");
        cache->put(2, "two");
        CHECK_EQ(cache->get(1), "one");
        CHECK_EQ(cache->get(2), "two");
    });

    add_test(name("get_missing_key_returns_nullopt"), [make] {
        auto cache = make(2);
        CHECK_EQ(cache->get(42), std::nullopt);
        cache->put(1, "one");
        CHECK_EQ(cache->get(2), std::nullopt);
    });

    add_test(name("update_existing_key_overwrites_without_evicting"), [make] {
        auto cache = make(2);
        cache->put(1, "one");
        cache->put(2, "two");
        cache->put(1, "uno");
        CHECK_EQ(cache->get(1), "uno");
        CHECK_EQ(cache->get(2), "two");  // still there, so nothing was evicted
    });

    add_test(name("evicts_least_recently_used"), [make] {
        auto cache = make(2);
        cache->put(1, "one");
        cache->put(2, "two");
        cache->put(3, "three");  // evicts key 1
        CHECK_EQ(cache->get(1), std::nullopt);
        CHECK_EQ(cache->get(2), "two");
        CHECK_EQ(cache->get(3), "three");
    });

    add_test(name("get_refreshes_recency"), [make] {
        auto cache = make(2);
        cache->put(1, "one");
        cache->put(2, "two");
        CHECK_EQ(cache->get(1), "one");  // key 1 becomes most recently used
        cache->put(3, "three");          // so key 2 is evicted, not key 1
        CHECK_EQ(cache->get(2), std::nullopt);
        CHECK_EQ(cache->get(1), "one");
        CHECK_EQ(cache->get(3), "three");
    });

    add_test(name("put_refreshes_recency"), [make] {
        auto cache = make(2);
        cache->put(1, "one");
        cache->put(2, "two");
        cache->put(1, "uno");  // refreshes key 1
        cache->put(3, "three");
        CHECK_EQ(cache->get(2), std::nullopt);
        CHECK_EQ(cache->get(1), "uno");
        CHECK_EQ(cache->get(3), "three");
    });

    add_test(name("capacity_one_evicts_every_insert"), [make] {
        auto cache = make(1);
        cache->put(1, "one");
        CHECK_EQ(cache->get(1), "one");
        cache->put(2, "two");
        CHECK_EQ(cache->get(1), std::nullopt);
        CHECK_EQ(cache->get(2), "two");
        cache->put(2, "dos");  // update, not insert
        CHECK_EQ(cache->get(2), "dos");
    });

    add_test(name("long_insert_run_keeps_only_the_newest_entries"), [make] {
        constexpr int kCapacity = 8;
        constexpr int kInserts = 1000;
        auto cache = make(kCapacity);
        for (int i = 0; i < kInserts; ++i) {
            cache->put(i, std::to_string(i));
        }
        // Probing an absent key cannot disturb the recency order, and all the
        // surviving keys are probed, so the order is unchanged overall.
        for (int i = 0; i < kInserts - kCapacity; ++i) {
            CHECK_EQ(cache->get(i), std::nullopt);
        }
        for (int i = kInserts - kCapacity; i < kInserts; ++i) {
            CHECK_EQ(cache->get(i), std::to_string(i));
        }
    });

    add_test(name("empty_value_and_negative_keys"), [make] {
        auto cache = make(2);
        cache->put(-5, "");
        cache->put(0, "zero");
        CHECK_EQ(cache->get(-5), "");
        CHECK_EQ(cache->get(0), "zero");
        cache->put(-1, "minus one");  // evicts key -5, the least recently used
        CHECK_EQ(cache->get(-5), std::nullopt);
    });

    add_test(name("randomized_operations_match_reference_model"), [make] {
        constexpr std::size_t kCapacity = 7;
        constexpr int kKeySpace = 20;
        constexpr int kOperations = 20000;

        auto cache = make(kCapacity);
        ReferenceLRU reference(kCapacity);
        std::mt19937 rng(20240522);

        for (int step = 0; step < kOperations; ++step) {
            const int key = static_cast<int>(rng() % kKeySpace);
            if (rng() % 10 < 4) {
                const std::string value = "v" + std::to_string(key) + "_" + std::to_string(step);
                cache->put(key, value);
                reference.put(key, value);
            } else {
                const auto actual = cache->get(key);
                const auto expected = reference.get(key);
                if (actual != expected) {
                    report_failure(__FILE__, __LINE__,
                                   "diverged from reference at step " + std::to_string(step) +
                                       " for key " + std::to_string(key) + ": got " +
                                       describe(actual) + ", want " + describe(expected));
                    return;
                }
                ++check_count();
            }
        }
    });

    add_test(name("concurrent_access_is_safe"), [make] {
        constexpr int kThreads = 8;
        constexpr int kOpsPerThread = 5000;
        constexpr int kKeySpace = 64;

        auto cache = make(16);
        std::atomic<bool> corrupted_value{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&cache, &corrupted_value, t] {
                std::mt19937 rng(static_cast<unsigned>(t) + 1);
                for (int i = 0; i < kOpsPerThread; ++i) {
                    const int key = static_cast<int>(rng() % kKeySpace);
                    const std::string expected = "value-" + std::to_string(key);
                    if (rng() % 2 == 0) {
                        cache->put(key, expected);
                    } else {
                        // Every writer stores the same value for a given key, so a
                        // reader sees exactly that value or nothing at all.
                        const auto value = cache->get(key);
                        if (value && *value != expected) {
                            corrupted_value = true;
                        }
                    }
                }
            });
        }

        for (std::thread& thread : threads) {
            thread.join();
        }

        CHECK(!corrupted_value.load());

        // Still functional after the hammering.
        cache->put(999, "still works");
        CHECK_EQ(cache->get(999), "still works");
    });
}

}  // namespace lru_test
