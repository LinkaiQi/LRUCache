// Time-to-live: per-cache default, per-entry override, and reclaiming memory.
//
// Build and run:  make examples

#include <lru/lru_cache.hpp>

#include <chrono>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

void show(const char* label, const std::optional<std::string>& value) {
    std::cout << "  " << label << (value ? *value : std::string("<expired or missing>")) << "\n";
}

}  // namespace

int main() {
    // Every entry expires 100ms after it is written unless told otherwise.
    lru::LRUCache<std::string, std::string> cache(100, 100ms);

    cache.put("session", "abc123");                    // uses the 100ms default
    cache.put("config", "{...}", std::nullopt);        // never expires
    cache.put("otp", "555123", 30ms);                  // shorter, per entry

    std::cout << "immediately:\n";
    show("session -> ", cache.get("session"));
    show("otp     -> ", cache.get("otp"));
    show("config  -> ", cache.get("config"));

    std::this_thread::sleep_for(50ms);
    std::cout << "\nafter 50ms:\n";
    show("session -> ", cache.get("session"));  // still alive
    show("otp     -> ", cache.get("otp"));      // gone
    show("config  -> ", cache.get("config"));

    std::this_thread::sleep_for(80ms);
    std::cout << "\nafter 130ms:\n";
    show("session -> ", cache.get("session"));  // gone
    show("config  -> ", cache.get("config"));   // still there: no TTL

    // Expiry is lazy. An entry nobody reads again keeps its slot until it is
    // evicted or purged, so reclaim it explicitly when that matters.
    cache.put("stale", "value", 1ms);
    std::this_thread::sleep_for(10ms);
    std::cout << "\nbefore purge: size=" << cache.size() << "\n";
    std::cout << "purged " << cache.purge_expired() << " entry(s)\n";
    std::cout << "after purge:  size=" << cache.size() << "\n";

    // Refresh-on-miss: recompute and re-arm the TTL in one atomic step.
    const std::string token = cache.get_or_compute(
        "session", [] { return std::string("regenerated-token"); }, 100ms);
    std::cout << "\nget_or_compute(session) -> " << token << "\n";

    const lru::CacheStats stats = cache.stats();
    std::cout << "stats -> hits=" << stats.hits << " misses=" << stats.misses
              << " expirations=" << stats.expirations << "\n";
    return 0;
}
