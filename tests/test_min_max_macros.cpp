// Guards against a Windows-only compile failure, on every platform.
//
// <windows.h> defines min() and max() as function-like macros unless NOMINMAX
// is defined first. Any unparenthesised call such as time_point::max() is then
// seen by the preprocessor as a macro invocation with no arguments, and the
// translation unit fails to compile. Plenty of Windows code includes
// <windows.h> before third-party headers, so the cache has to survive it.
//
// Pulling in the standard headers first mirrors reality: they are already
// guarded by the time the macros appear, so only our header is exposed.

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include "test_harness.hpp"

#define min(a, b) (((a) < (b)) ? (a) : (b))
#define max(a, b) (((a) > (b)) ? (a) : (b))

#include <lru/lru_cache.hpp>
#include <lru/lru_cache_classic.hpp>
#include <lru/version.hpp>

#undef min
#undef max

namespace {

using namespace std::chrono_literals;

TEST(headers_compile_with_min_max_macros_defined) {
    // Reaching this point at all is most of the test: the file would not have
    // compiled if the headers called max() unparenthesised.
    lru::LRUCache<int, std::string> cache(2);
    cache.put(1, "one");
    CHECK_EQ(cache.get(1), "one");
}

TEST(no_ttl_still_means_never_expires_with_macros_defined) {
    // The "never expires" sentinel is the value that touches time_point::max().
    lru::LRUCache<int, std::string> cache(2);
    cache.put(1, "one");
    CHECK(cache.contains(1));
    CHECK_EQ(cache.peek(1), "one");
}

TEST(ttl_paths_work_with_macros_defined) {
    lru::LRUCache<int, std::string> cache(4, 50ms);
    cache.put(1, "default ttl");
    cache.put(2, "pinned", std::nullopt);  // saturates to the max() sentinel
    CHECK_EQ(cache.get(1), "default ttl");
    CHECK_EQ(cache.get(2), "pinned");
    CHECK_EQ(cache.purge_expired(), std::size_t{0});
}

TEST(classic_cache_works_with_macros_defined) {
    lru::classic::LRUCache cache(2);
    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");  // evicts key 1
    CHECK_EQ(cache.get(1), std::nullopt);
    CHECK_EQ(cache.get(3), "three");
}

}  // namespace

int main() { return lru_test::run_all("min/max macro compatibility"); }
