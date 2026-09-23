// Quickstart: the whole API in one page.
//
// Build and run:  make examples

#include <lru/lru_cache.hpp>
#include <lru/version.hpp>

#include <iostream>
#include <optional>
#include <string>

namespace {

void show(const char* label, const std::optional<std::string>& value) {
    std::cout << "  " << label << (value ? *value : std::string("<miss>")) << "\n";
}

}  // namespace

int main() {
    std::cout << "lru_cache " << lru::Version::string() << "\n\n";

    lru::LRUCache<int, std::string> cache(2);  // capacity 2, no expiry

    cache.put(1, "one");
    cache.put(2, "two");
    show("get(1)      -> ", cache.get(1));  // hit, so key 1 is now most recently used

    cache.put(3, "three");                  // so key 2 is evicted, not key 1
    show("get(2)      -> ", cache.get(2));  // miss
    show("peek(3)     -> ", cache.peek(3));  // read without touching recency

    std::cout << "  contains(1) -> " << (cache.contains(1) ? "yes" : "no") << "\n";
    std::cout << "  erase(3)    -> " << (cache.erase(3) ? "removed" : "absent") << "\n";
    std::cout << "  size        -> " << cache.size() << " / " << cache.capacity() << "\n";

    // Atomic "insert only if missing". Doing this with contains() + put() would
    // be a race: two threads could both decide they were first.
    std::cout << "  insert_if_absent(1, ...) -> " << (cache.insert_if_absent(1, "ONE") ? "inserted" : "already present")
              << "\n";

    // Compute-on-miss. The factory runs without the cache lock held.
    const std::string value = cache.get_or_compute(42, [] {
        std::cout << "  (computing 42 ...)\n";
        return std::string("forty two");
    });
    std::cout << "  get_or_compute(42) -> " << value << "\n";

    const lru::CacheStats stats = cache.stats();
    std::cout << "  stats       -> hits=" << stats.hits << " misses=" << stats.misses
              << " evictions=" << stats.evictions << " expirations=" << stats.expirations << "\n";
    return 0;
}
