// Runs the shared conformance suite against both caches in one binary.
//
// This is what guarantees the classic and production versions are observably
// identical rather than merely similar, so it deliberately lives in its own
// file instead of being registered twice from two different suites.

#include <lru/lru_cache.hpp>
#include <lru/lru_cache_classic.hpp>

#include "conformance.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <memory>
#include <string>

int main() {
    lru_test::register_conformance_tests("production", [](std::size_t capacity) {
        return std::make_unique<lru::LRUCache<int, std::string>>(capacity);
    });
    lru_test::register_conformance_tests("classic", [](std::size_t capacity) {
        return std::make_unique<lru::classic::LRUCache>(static_cast<int>(capacity));
    });
    return lru_test::run_all("conformance");
}
