// Error handling: rejecting bad arguments, and surviving exceptions thrown by
// the types being stored.
//
// The argument checks are the easy half. The interesting half is exception
// safety: the README promises the basic guarantee, meaning the map and the
// list always agree and the cache stays usable even when a Key or Value
// operation throws part way through an insert. That promise is worth nothing
// unless something actually throws, so ThrowingValue below does, at every
// point in turn.

#include <lru/lru_cache.hpp>
#include <lru/lru_cache_classic.hpp>

#include "test_harness.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using Ms = std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// Argument validation
// ---------------------------------------------------------------------------

template <typename Fn>
bool throws_invalid_argument(Fn&& fn) {
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

TEST(zero_capacity_is_rejected) {
    CHECK(throws_invalid_argument([] { lru::LRUCache<int, std::string> cache(0); }));
}

TEST(classic_rejects_a_non_positive_size) {
    CHECK(throws_invalid_argument([] { lru::classic::LRUCache cache(0); }));
    CHECK(throws_invalid_argument([] { lru::classic::LRUCache cache(-1); }));
}

TEST(non_positive_default_ttl_is_rejected) {
    CHECK(throws_invalid_argument([] { lru::LRUCache<int, std::string> cache(4, Ms{0}); }));
    CHECK(throws_invalid_argument([] { lru::LRUCache<int, std::string> cache(4, Ms{-1}); }));
}

TEST(non_positive_per_entry_ttl_is_rejected_and_changes_nothing) {
    lru::LRUCache<int, std::string> cache(4);
    cache.put(1, "one");

    CHECK(throws_invalid_argument([&cache] { cache.put(2, "two", Ms{0}); }));
    CHECK(throws_invalid_argument([&cache] { cache.put(2, "two", Ms{-5}); }));
    CHECK(throws_invalid_argument([&cache] { cache.insert_if_absent(2, "two", Ms{0}); }));
    CHECK(throws_invalid_argument(
        [&cache] { cache.get_or_compute(2, [] { return std::string("two"); }, Ms{0}); }));

    // The rejected calls are refused before anything is locked or modified.
    CHECK_EQ(cache.size(), std::size_t{1});
    CHECK_EQ(cache.get(1), "one");
    CHECK_EQ(cache.get(2), std::nullopt);
}

TEST(a_rejected_argument_leaves_the_cache_usable) {
    lru::LRUCache<int, std::string> cache(2);
    CHECK(throws_invalid_argument([&cache] { cache.put(1, "one", Ms{-1}); }));
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK_EQ(cache.size(), std::size_t{2});
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.get(3), "three");
}

// ---------------------------------------------------------------------------
// Exception safety
// ---------------------------------------------------------------------------

struct Boom : std::runtime_error {
    Boom() : std::runtime_error("ThrowingValue detonated") {}
};

// Throws from whichever copy or move operation the countdown lands on, so a
// test can walk the trip point through every step of an insert.
class ThrowingValue {
public:
    explicit ThrowingValue(std::string payload) : m_payload(std::move(payload)) { ++s_live; }

    ThrowingValue(const ThrowingValue& other) : m_payload(other.m_payload) {
        detonate_or_arm();
        ++s_live;
    }

    ThrowingValue(ThrowingValue&& other) : m_payload(std::move(other.m_payload)) {
        detonate_or_arm();
        ++s_live;
    }

    ThrowingValue& operator=(const ThrowingValue& other) {
        detonate_or_arm();
        m_payload = other.m_payload;
        return *this;
    }

    ThrowingValue& operator=(ThrowingValue&& other) {
        detonate_or_arm();
        m_payload = std::move(other.m_payload);
        return *this;
    }

    ~ThrowingValue() { --s_live; }

    const std::string& payload() const { return m_payload; }

    static void arm(int operations_until_throw) { s_countdown = operations_until_throw; }
    static void disarm() { s_countdown = -1; }
    static int live() { return s_live; }

private:
    static void detonate_or_arm() {
        if (s_countdown < 0) {
            return;
        }
        if (s_countdown == 0) {
            s_countdown = -1;  // one shot, so the unwinding itself cannot throw again
            throw Boom{};
        }
        --s_countdown;
    }

    static int s_countdown;
    static int s_live;

    std::string m_payload;
};

int ThrowingValue::s_countdown = -1;
int ThrowingValue::s_live = 0;

// Lets the harness print one when a check fails.
std::ostream& operator<<(std::ostream& os, const ThrowingValue& value) {
    return os << value.payload();
}

using ThrowingCache = lru::LRUCache<int, ThrowingValue>;

std::string expected_payload(int key) { return "payload-" + std::to_string(key); }

// Confirms the map and the list still agree and nothing is corrupt.
void check_cache_is_intact(ThrowingCache& cache, std::size_t capacity) {
    CHECK(cache.size() <= capacity);

    // Whatever survived must still be readable and must still carry the value
    // that belongs to its own key.
    std::size_t found = 0;
    for (int key = 0; key < 32; ++key) {
        const std::optional<ThrowingValue> value = cache.peek(key);
        if (value) {
            ++found;
            CHECK_EQ(value->payload(), expected_payload(key));
        }
    }
    CHECK_EQ(found, cache.size());

    // And the cache must still work.
    ThrowingValue::disarm();
    cache.put(999, ThrowingValue("payload-999"));
    const std::optional<ThrowingValue> stored = cache.get(999);
    CHECK(stored.has_value());
    if (stored) {
        CHECK_EQ(stored->payload(), std::string("payload-999"));
    }
    cache.erase(999);
}

TEST(a_throwing_insert_below_capacity_leaves_the_cache_untouched) {
    ThrowingValue::disarm();
    {
        ThrowingCache cache(8);
        cache.put(0, ThrowingValue(expected_payload(0)));
        cache.put(1, ThrowingValue(expected_payload(1)));

        ThrowingValue::arm(0);  // throw on the very first copy or move
        bool threw = false;
        try {
            cache.put(2, ThrowingValue(expected_payload(2)));
        } catch (const Boom&) {
            threw = true;
        }
        ThrowingValue::disarm();

        CHECK(threw);
        CHECK_EQ(cache.size(), std::size_t{2});
        CHECK_EQ(cache.get(2), std::nullopt);
        check_cache_is_intact(cache, 8);
    }
    CHECK_EQ(ThrowingValue::live(), 0);
}

// Walks the trip point through every copy and move an insert performs, both
// below capacity and at capacity, where put() recycles the evicted node. The
// recycling path has a catch block that nothing else in the suite reaches.
TEST(an_insert_that_throws_at_any_point_keeps_the_cache_consistent) {
    constexpr std::size_t kCapacity = 4;

    for (int trip = 0; trip < 8; ++trip) {
        ThrowingValue::disarm();
        {
            ThrowingCache cache(kCapacity);
            for (int key = 0; key < static_cast<int>(kCapacity); ++key) {
                cache.put(key, ThrowingValue(expected_payload(key)));
            }
            CHECK_EQ(cache.size(), kCapacity);

            ThrowingValue::arm(trip);
            try {
                cache.put(10, ThrowingValue(expected_payload(10)));
            } catch (const Boom&) {
                // Expected for the trip points that land inside the insert.
            }
            ThrowingValue::disarm();

            // Whether or not it threw, the cache must be coherent. Losing the
            // evicted entry is allowed by the basic guarantee. Corruption,
            // a double free or a mismatched key and value is not.
            check_cache_is_intact(cache, kCapacity);
        }
        // Every ThrowingValue, including the ones abandoned mid-insert, is gone.
        CHECK_EQ(ThrowingValue::live(), 0);
    }
}

TEST(a_throwing_update_of_an_existing_key_keeps_the_cache_consistent) {
    for (int trip = 0; trip < 4; ++trip) {
        ThrowingValue::disarm();
        {
            ThrowingCache cache(4);
            cache.put(0, ThrowingValue(expected_payload(0)));
            cache.put(1, ThrowingValue(expected_payload(1)));

            ThrowingValue::arm(trip);
            try {
                cache.put(1, ThrowingValue(expected_payload(1)));
            } catch (const Boom&) {
            }
            ThrowingValue::disarm();

            CHECK(cache.size() <= std::size_t{4});
            const std::optional<ThrowingValue> untouched = cache.peek(0);
            CHECK(untouched.has_value());
            if (untouched) {
                CHECK_EQ(untouched->payload(), expected_payload(0));
            }
            check_cache_is_intact(cache, 4);
        }
        CHECK_EQ(ThrowingValue::live(), 0);
    }
}

TEST(a_throwing_insert_if_absent_keeps_the_cache_consistent) {
    for (int trip = 0; trip < 6; ++trip) {
        ThrowingValue::disarm();
        {
            ThrowingCache cache(2);
            cache.put(0, ThrowingValue(expected_payload(0)));
            cache.put(1, ThrowingValue(expected_payload(1)));

            ThrowingValue::arm(trip);
            try {
                cache.insert_if_absent(2, ThrowingValue(expected_payload(2)));
            } catch (const Boom&) {
            }
            ThrowingValue::disarm();

            check_cache_is_intact(cache, 2);
        }
        CHECK_EQ(ThrowingValue::live(), 0);
    }
}

TEST(a_throwing_factory_propagates_and_leaves_the_cache_untouched) {
    ThrowingValue::disarm();
    {
        ThrowingCache cache(4);
        cache.put(0, ThrowingValue(expected_payload(0)));

        bool threw = false;
        try {
            cache.get_or_compute(1, []() -> ThrowingValue { throw Boom{}; });
        } catch (const Boom&) {
            threw = true;
        }

        CHECK(threw);
        CHECK_EQ(cache.size(), std::size_t{1});
        CHECK(!cache.contains(1));

        // The factory runs without the lock held, so a throw from it cannot
        // have left the mutex locked. If it had, this would deadlock.
        check_cache_is_intact(cache, 4);
    }
    CHECK_EQ(ThrowingValue::live(), 0);
}

TEST(the_cache_still_destroys_cleanly_after_a_throw) {
    ThrowingValue::disarm();
    {
        ThrowingCache cache(3);
        for (int key = 0; key < 3; ++key) {
            cache.put(key, ThrowingValue(expected_payload(key)));
        }
        ThrowingValue::arm(1);
        try {
            cache.put(9, ThrowingValue(expected_payload(9)));
        } catch (const Boom&) {
        }
        ThrowingValue::disarm();
        // Destructor runs here, on a cache that threw mid-insert.
    }
    CHECK_EQ(ThrowingValue::live(), 0);
}

}  // namespace

int main() { return lru_test::run_all("error handling"); }
