# LRUCache

[![CI](https://github.com/LinkaiQi/LRUCache/actions/workflows/ci.yml/badge.svg)](https://github.com/LinkaiQi/LRUCache/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)
[![header-only](https://img.shields.io/badge/header--only-yes-brightgreen.svg)](include/lru)

A thread-safe, fixed-capacity LRU cache for C++17 with optional per-entry TTL.
Header-only, no dependencies.

```cpp
#include <lru/lru_cache.hpp>

lru::LRUCache<std::string, std::string> cache(1024);

cache.put("user:42", "Ada");
if (auto name = cache.get("user:42")) {
    std::cout << *name << "\n";
}
```

* **O(1)** `get` and `put`, using an intrusive linked list plus a hash map
* **Thread-safe**: every operation is locked, and verified under ThreadSanitizer
* **Atomic compound operations**: `insert_if_absent`, `get_or_compute`
* **Optional TTL**: per-cache default, per-entry override, injectable clock
* **No hidden threads** and no allocations on the hot path
* **Tested**: 89 tests, ~27k assertions, ASan + UBSan + TSan + leak-checked in CI

## Try it in 30 seconds

Linux and macOS:

```sh
git clone https://github.com/LinkaiQi/LRUCache && cd LRUCache
make run
```

Windows, where `make` is not available, so build with CMake instead:

```powershell
git clone https://github.com/LinkaiQi/LRUCache
cd LRUCache
cmake -S . -B build
cmake --build build --config Release
.\build\Release\cache_shell.exe
```

Either way you get an interactive shell, so you can play with the cache without
writing any code:

```
lru> put a 1
OK
lru> put b 2
OK
lru> get a
1
lru> put session token 50     # optional trailing TTL, in milliseconds
OK
lru> stats
hits=1 misses=0 evictions=0 expirations=0
```

It reads piped input too, which makes it easy to script:

```sh
printf 'put session abc 50\nhas session\nstats\n' | ./build/cache_shell 100
```

```powershell
"put session abc 50", "has session", "stats" | .\build\Release\cache_shell.exe 100
```

Pass `<capacity> <default_ttl_ms>` to configure the cache, and type `help`
inside the shell for the full command list.

## Installing

### Copy the header

The library is header-only. Copy `include/lru/` into your project and add
`include/` to your include path. Nothing else is required.

### CMake with FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(lru_cache
    GIT_REPOSITORY https://github.com/LinkaiQi/LRUCache.git
    GIT_TAG v1.0.1)
FetchContent_MakeAvailable(lru_cache)

target_link_libraries(your_target PRIVATE lru::lru_cache)
```

### CMake with an installed package

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --install build
```

```cmake
find_package(lru_cache 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE lru::lru_cache)
```

Both paths are exercised in CI, so a broken package is a failing build rather
than a surprise for you.

## API

```cpp
template <typename Key,
          typename Value,
          typename Hash     = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Clock    = std::chrono::steady_clock>
class lru::LRUCache;
```

| Method | Recency | Notes |
|---|---|---|
| `put(key, value [, ttl])` | most recently used | inserts or updates, evicting the LRU entry when full |
| `insert_if_absent(key, value [, ttl])` | most recently used | atomic, returns `true` if inserted |
| `get_or_compute(key, factory [, ttl])` | most recently used | computes on miss, factory runs **without** the lock |
| `get(key)` | most recently used | `std::optional<Value>`; counts a hit or miss |
| `peek(key)` | unchanged | read without disturbing LRU order or statistics |
| `contains(key)` | unchanged | membership test |
| `erase(key)` | n/a | returns `true` if an entry was removed |
| `clear()` | n/a | drops all entries, keeps capacity |
| `purge_expired()` | n/a | drops expired entries, returns how many |
| `size()` `capacity()` `empty()` | n/a | `size()` includes expired-but-unpurged entries |
| `default_ttl()` | n/a | the cache's default TTL, if any |
| `stats()` `reset_stats()` | n/a | hits, misses, evictions, expirations |

`get` and `peek` return **by value**. Returning a reference or pointer would be
a dangling-pointer bug waiting to happen: once the lock is released, another
thread can evict the entry out from under the caller.

## TTL

TTL is opt-in. A cache with no TTL never reads the clock, so you pay nothing for
a feature you do not use.

```cpp
using namespace std::chrono_literals;

lru::LRUCache<std::string, std::string> cache(1000, 5min);  // default for every entry

cache.put("session", token);                 // expires in 5 minutes
cache.put("otp", code, 30s);                 // per-entry override
cache.put("config", blob, std::nullopt);     // never expires

cache.purge_expired();                       // reclaim entries nobody read again
```

**Expiration is lazy.** An entry expires logically the moment its deadline
passes, and `get`, `peek` and `contains` all report it as absent right away.
Its memory is only reclaimed when it is read, evicted, or purged. There is no
background thread, so cleanup never happens behind your back. Call
`purge_expired()` if you need the memory back on a schedule you control.

A non-positive TTL throws `std::invalid_argument` rather than silently storing
an entry that is already dead.

### Testing code that uses TTL

`Clock` is a template parameter, so you can drive expiry by hand instead of
sleeping in your tests:

```cpp
struct ManualClock {
    using duration   = std::chrono::milliseconds;
    using rep        = duration::rep;
    using period     = duration::period;
    using time_point = std::chrono::time_point<ManualClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept { return s_now; }
    static void advance(duration by) { s_now += by; }
    static time_point s_now;
};

lru::LRUCache<int, std::string, std::hash<int>, std::equal_to<int>, ManualClock> cache(8, 100ms);
cache.put(1, "one");
ManualClock::advance(101ms);
assert(!cache.get(1));       // no sleeping, fully deterministic
```

The default is `std::chrono::steady_clock`, which is monotonic and therefore
immune to wall-clock adjustments.

## Thread safety

Every public method is safe to call concurrently. The suite hammers the cache
from 8 threads under ThreadSanitizer in CI.

A `std::shared_mutex` would buy nothing here, because `get` updates the recency
order and therefore mutates the cache as much as `put` does. If lock contention
shows up in a profile, shard the key space across N independent caches rather
than trying to make one cache lock-free.

**Compound operations built from separate calls are still races.** This is a bug
no amount of internal locking can fix:

```cpp
if (!cache.contains(key)) {          // thread A and thread B can both get here
    cache.put(key, expensive());     // ...and both compute and both write
}
```

Use the atomic operations instead:

```cpp
cache.insert_if_absent(key, value);                     // exactly one winner
auto value = cache.get_or_compute(key, [] { ... });     // compute on miss
```

`get_or_compute` runs the factory **without** holding the lock, so a slow
factory never blocks other threads and may safely call back into the same
cache. The trade-off is explicit: concurrent callers may each compute a value
for the same key, the first to finish wins, and the others discard their work.

Move construction and move assignment are *not* thread-safe. Like any standard
container, do not move a cache other threads are using.

## Error handling

| Situation | Behaviour |
|---|---|
| Key missing or expired | `std::nullopt`, not an exception |
| Capacity of zero | `std::invalid_argument` from the constructor |
| Non-positive TTL | `std::invalid_argument`, thrown before anything is locked or modified |
| `Key`/`Value` constructor or assignment throws | propagated, the cache stays consistent |
| Allocation failure | propagated, nothing is leaked |

Operations provide the **basic guarantee**: the map and the list always agree,
so the cache is never left corrupt. A `put` that throws may have already evicted
the least recently used entry. The map is reserved to capacity up front, so
insertions do not rehash. `Hash` and `KeyEqual` are assumed not to throw.

## Two versions

| | `lru/lru_cache_classic.hpp` | `lru/lru_cache.hpp` |
|---|---|---|
| | **classic** | **production** |
| Structure | standalone `LinkedList` class | list folded into the cache |
| Types | concrete `int` → `std::string` | templated |
| API | `put`, `get` | the full table above |
| TTL | no | yes |
| Eviction | frees the node, allocates a new one | recycles the node |
| Best for | reading and understanding the algorithm | shipping |

The classic version exists to be read. It keeps the shape of a textbook
implementation, where the recency list is its own class with
`CreateNode`/`add_first`/`remove_last`. It is correct, leak-free and
thread-safe, just smaller in scope. Both versions are registered against the
same conformance suite, which is what guarantees they behave identically rather
than merely similarly.

## Design

```
        map:  key ──► Node*
                       │
  head ◄──► Node ◄──► Node ◄──► Node ◄──► tail
  (MRU)                                  (LRU, evicted first)
```

A doubly linked list tracks recency and an `unordered_map` maps a key to its
node, so moving an entry to the front is pure pointer surgery, with no
rehashing and no allocation. Nodes are owned exactly once, and the map holds non-owning pointers.

## Performance

`make bench`, Apple M-series, clang 21, `-O2`, capacity 10,000, 2M operations,
single-threaded (so this measures the data structure, not lock contention):

| workload | classic | production | delta |
|---|---|---|---|
| insert churn (every put evicts) | 30.9 ns/op | 20.7 ns/op | **33% faster** |
| insert churn, 128-byte values | 44.3 ns/op | 31.4 ns/op | **29% faster** |
| hot reads (every get hits) | 10.6 ns/op | 9.6 ns/op | 10% faster |
| mixed 30% write / 70% read | 40.4 ns/op | 35.0 ns/op | 13% faster |

The insert gap is node recycling: at capacity the production cache reuses the
evicted node instead of freeing it and allocating a replacement, which removes
one `free` and one `malloc` per insertion and lets the old value reuse its own
storage. A `std::string` keeps its buffer, so the 128-byte case avoids a second
allocation too. Run-to-run variance is a few percent, so re-run the benchmark on
your own hardware rather than trusting this table.

## Building

The Makefile is the quickest route on Linux and macOS.

```sh
make test      # build and run every test suite
make run       # interactive cache shell
make examples  # build and run the examples
make bench     # classic vs production benchmark
make asan      # tests under AddressSanitizer
make ubsan     # tests under UndefinedBehaviorSanitizer
make tsan      # tests under ThreadSanitizer
make leaks     # leak check (macOS `leaks`, LeakSanitizer elsewhere)
make check     # everything CI runs
make help      # this list
```

CMake works everywhere, and is the only option on Windows because the Makefile
relies on a POSIX shell.

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Everything compiles clean under `-Wall -Wextra -Wpedantic -Wshadow -Wconversion
-Wsign-conversion -Wold-style-cast -Werror` with GCC and Clang, and under
`/W4 /WX /permissive-` with MSVC.

## Supported platforms

Every push is built and tested on Linux, macOS and Windows.

| Platform | Compiler | Covered by CI |
|---|---|---|
| Linux | GCC or Clang | build, tests, ASan, UBSan, TSan, leak check |
| macOS | AppleClang | build, tests, ASan, UBSan, TSan, leak check |
| Windows | MSVC | build and tests through CMake and CTest |

The headers include a regression test for the `min` and `max` macros that
`<windows.h>` defines unless `NOMINMAX` is set, so including this library after
`<windows.h>` compiles cleanly. That test runs on every platform.

## Tests

Self-contained, with no test framework to install. 89 tests, about 27,000
assertions.

`tests/conformance.hpp` holds the behaviour every cache in this repository must
exhibit, written against the smallest common API. Both versions are registered
against it. Two entries are worth calling out:

* **`randomized_operations_match_reference_model`** runs 20,000 random
  put/get operations against a deliberately naive vector-based LRU and asserts
  every returned value matches. This catches subtly wrong pointer updates that
  hand-written examples miss.
* **`concurrent_access_is_safe`** hammers the cache from 8 threads under
  ThreadSanitizer.

On top of that: TTL expiry driven by a manual clock (deterministic, no sleeps),
`LinkedList` unit tests covering every insert and removal position, move
semantics, value copy/move/destruction accounting, and a packaging job that
installs the library and consumes it from a separate project.

## Layout

| Path | Purpose |
|---|---|
| `include/lru/lru_cache.hpp` | the production cache |
| `include/lru/lru_cache_classic.hpp` | the classic, readable version |
| `include/lru/version.hpp` | version macros and `lru::Version` |
| `examples/` | quickstart, TTL walkthrough, interactive shell |
| `tests/` | conformance suite, harness, per-version suites |
| `benchmarks/` | comparison benchmark |
| `cmake/` | package config template |

## License

MIT. See [LICENSE](LICENSE).
