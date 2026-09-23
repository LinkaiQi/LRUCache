// Compares the classic cache against the production cache on the same
// workloads. Single-threaded, so it measures the data structure rather than
// lock contention.
//
// Build and run:  make bench

#include <lru/lru_cache.hpp>
#include <lru/lru_cache_classic.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// Keeps the optimiser from deleting work whose result is never inspected.
std::uint64_t g_sink = 0;

struct Result {
    double ns_per_op;
};

template <typename Fn>
Result time_ops(std::size_t operations, Fn&& fn) {
    const auto start = Clock::now();
    fn();
    const auto elapsed = Clock::now() - start;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    return Result{static_cast<double>(ns) / static_cast<double>(operations)};
}

std::vector<int> make_keys(std::size_t count, int key_space, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<int> keys;
    keys.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        keys.push_back(static_cast<int>(rng() % static_cast<unsigned>(key_space)));
    }
    return keys;
}

// Every put misses and therefore evicts: the allocation path is what is being
// measured.
template <typename Cache>
Result bench_insert_churn(Cache& cache, std::size_t operations) {
    return time_ops(operations, [&cache, operations] {
        for (std::size_t i = 0; i < operations; ++i) {
            cache.put(static_cast<int>(i), "value");
        }
        g_sink += cache.get(static_cast<int>(operations - 1)).has_value() ? 1u : 0u;
    });
}

// Same, but with values too large for the small-string buffer, so the classic
// cache pays for a string allocation on every insert as well.
template <typename Cache>
Result bench_insert_churn_large_values(Cache& cache, std::size_t operations) {
    const std::string value(128, 'x');
    return time_ops(operations, [&cache, operations, &value] {
        for (std::size_t i = 0; i < operations; ++i) {
            cache.put(static_cast<int>(i), value);
        }
        g_sink += cache.get(static_cast<int>(operations - 1)).has_value() ? 1u : 0u;
    });
}

// Mostly hits: measures lookup plus the move-to-front pointer work.
template <typename Cache>
Result bench_hot_reads(Cache& cache, const std::vector<int>& keys) {
    for (int key : keys) {
        cache.put(key, "value");
    }
    return time_ops(keys.size(), [&cache, &keys] {
        for (int key : keys) {
            if (auto value = cache.get(key)) {
                g_sink += value->size();
            }
        }
    });
}

// Realistic mix: 30% writes, 70% reads over a key space four times the capacity.
template <typename Cache>
Result bench_mixed(Cache& cache, const std::vector<int>& keys) {
    return time_ops(keys.size(), [&cache, &keys] {
        std::size_t i = 0;
        for (int key : keys) {
            if (i % 10 < 3) {
                cache.put(key, "value");
            } else if (auto value = cache.get(key)) {
                g_sink += value->size();
            }
            ++i;
        }
    });
}

void print_row(const char* name, Result classic, Result production) {
    const double delta = (classic.ns_per_op - production.ns_per_op) / classic.ns_per_op * 100.0;
    std::cout << "  " << std::left << std::setw(28) << name << std::right << std::fixed
              << std::setprecision(1) << std::setw(10) << classic.ns_per_op << std::setw(12)
              << production.ns_per_op << std::setw(10) << delta << "%\n";
}

}  // namespace

int main() {
    constexpr std::size_t kCapacity = 10000;
    constexpr std::size_t kOperations = 2000000;

    std::cout << "LRU cache benchmark (ns/op, lower is better)\n"
              << "capacity=" << kCapacity << " operations=" << kOperations << "\n\n"
              << "  " << std::left << std::setw(28) << "workload" << std::right << std::setw(10)
              << "classic" << std::setw(12) << "production" << std::setw(11) << "delta" << "\n"
              << "  " << std::string(60, '-') << "\n";

    {
        lru::classic::LRUCache classic(static_cast<int>(kCapacity));
        lru::LRUCache<int, std::string> production(kCapacity);
        const Result a = bench_insert_churn(classic, kOperations);
        const Result b = bench_insert_churn(production, kOperations);
        print_row("insert churn (all evict)", a, b);
    }
    {
        lru::classic::LRUCache classic(static_cast<int>(kCapacity));
        lru::LRUCache<int, std::string> production(kCapacity);
        const Result a = bench_insert_churn_large_values(classic, kOperations / 2);
        const Result b = bench_insert_churn_large_values(production, kOperations / 2);
        print_row("insert churn (128B values)", a, b);
    }
    {
        const std::vector<int> keys = make_keys(kOperations, static_cast<int>(kCapacity) / 2, 1);
        lru::classic::LRUCache classic(static_cast<int>(kCapacity));
        lru::LRUCache<int, std::string> production(kCapacity);
        const Result a = bench_hot_reads(classic, keys);
        const Result b = bench_hot_reads(production, keys);
        print_row("hot reads (all hit)", a, b);
    }
    {
        const std::vector<int> keys = make_keys(kOperations, static_cast<int>(kCapacity) * 4, 2);
        lru::classic::LRUCache classic(static_cast<int>(kCapacity));
        lru::LRUCache<int, std::string> production(kCapacity);
        const Result a = bench_mixed(classic, keys);
        const Result b = bench_mixed(production, keys);
        print_row("mixed 30% write / 70% read", a, b);
    }

    std::cout << "\n  (checksum " << g_sink << ")\n";
    return 0;
}
