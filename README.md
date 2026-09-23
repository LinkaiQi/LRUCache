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

* **O(1)** `get` and `put`
* **Thread-safe**, verified under ThreadSanitizer
* **Atomic** `insert_if_absent` and `get_or_compute`
* **Optional TTL** with an injectable clock
* **No hidden threads**, no dependencies

## Design

```
        map:  key ──► Node*
                       │
  head ◄──► Node ◄──► Node ◄──► Node ◄──► tail
  (MRU)                                  (LRU, evicted first)
```

A doubly linked list tracks recency and an `unordered_map` maps a key to its
node, so moving an entry to the front is pure pointer surgery, with no rehashing
and no allocation. Nodes are owned exactly once and the map holds non-owning
pointers.

## Performance

`make bench`, Apple M-series, clang 21, `-O2`, capacity 10,000, 2M operations,
single-threaded.

| workload | classic | production | delta |
|---|---|---|---|
| insert churn (every put evicts) | 30.9 ns/op | 20.7 ns/op | **33% faster** |
| insert churn, 128-byte values | 44.3 ns/op | 31.4 ns/op | **29% faster** |
| hot reads (every get hits) | 10.6 ns/op | 9.6 ns/op | 10% faster |
| mixed 30% write / 70% read | 40.4 ns/op | 35.0 ns/op | 13% faster |

## Try it

Linux and macOS:

```sh
git clone https://github.com/LinkaiQi/LRUCache && cd LRUCache
make run
```

Windows, where `make` is not available:

```powershell
git clone https://github.com/LinkaiQi/LRUCache
cd LRUCache
cmake -S . -B build
cmake --build build --config Release
.\build\Release\cache_shell.exe
```

Either way you get an interactive shell, so you can try the cache without
writing any code:

```
lru> put a 1
OK
lru> get a
1
lru> put session token 50     # optional trailing TTL, in milliseconds
OK
lru> stats
hits=1 misses=0 evictions=0 expirations=0
```

Type `help` inside the shell for the full command list.

## Install

Copy `include/lru` into your project, or use CMake.

```cmake
include(FetchContent)
FetchContent_Declare(lru_cache
    GIT_REPOSITORY https://github.com/LinkaiQi/LRUCache.git
    GIT_TAG v1.0.1)
FetchContent_MakeAvailable(lru_cache)

target_link_libraries(your_target PRIVATE lru::lru_cache)
```

Or install it first and use `find_package`:

```sh
cmake -S . -B build && cmake --install build
```

```cmake
find_package(lru_cache 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE lru::lru_cache)
```

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
| `get(key)` | most recently used | returns `std::optional<Value>` |
| `peek(key)` | unchanged | read without disturbing LRU order or statistics |
| `contains(key)` | unchanged | membership test |
| `erase(key)` | n/a | returns `true` if an entry was removed |
| `clear()` | n/a | drops all entries, keeps capacity |
| `purge_expired()` | n/a | drops expired entries, returns how many |
| `size()` `capacity()` `empty()` | n/a | `size()` includes expired entries not yet purged |
| `default_ttl()` | n/a | the cache's default TTL, if any |
| `stats()` `reset_stats()` | n/a | hits, misses, evictions, expirations |

`get` and `peek` return **by value**. A reference would dangle as soon as
another thread evicted the entry.

## TTL

Opt-in. A cache with no TTL never reads the clock.

```cpp
using namespace std::chrono_literals;

lru::LRUCache<std::string, std::string> cache(1000, 5min);  // default for every entry

cache.put("session", token);                 // expires in 5 minutes
cache.put("otp", code, 30s);                 // per-entry override
cache.put("config", blob, std::nullopt);     // never expires

cache.purge_expired();                       // reclaim entries nobody read again
```

Expiry is lazy. An entry is reported as absent the moment its deadline passes,
but its memory is only reclaimed when it is read, evicted or purged. There is no
background thread, so cleanup never happens behind your back.

`Clock` is a template parameter, so tests can drive expiry by hand instead of
sleeping. `tests/test_lru_cache_ttl.cpp` has a manual clock to copy.

## Thread safety

Every public method is safe to call concurrently. A `std::shared_mutex` would
buy nothing, because `get` updates the recency order and therefore mutates the
cache as much as `put` does.

Compound operations built from separate calls are still races, and no amount of
internal locking can fix that:

```cpp
if (!cache.contains(key)) {          // two threads can both get here
    cache.put(key, expensive());     // and both compute, and both write
}
```

Use the atomic versions instead:

```cpp
cache.insert_if_absent(key, value);                     // exactly one winner
auto value = cache.get_or_compute(key, [] { ... });     // compute on miss
```

`get_or_compute` runs the factory without holding the lock, so a slow factory
never blocks other threads and may safely call back into the same cache.
Concurrent callers may each compute a value for the same key, the first to
finish wins, and the others discard their work.

Move construction and move assignment are *not* thread-safe. Like any standard
container, do not move a cache other threads are using.

## Error handling

| Situation | Behaviour |
|---|---|
| Key missing or expired | `std::nullopt`, not an exception |
| Capacity of zero | `std::invalid_argument` |
| Non-positive TTL | `std::invalid_argument`, before anything is modified |
| `Key` or `Value` throws | propagated, the cache stays consistent |
| Allocation failure | propagated, nothing is leaked |

Operations give the basic guarantee. The map and the list always agree, so the
cache is never left corrupt.

## Two versions

| | `lru/lru_cache_classic.hpp` | `lru/lru_cache.hpp` |
|---|---|---|
| Structure | standalone `LinkedList` class | list folded into the cache |
| Types | concrete `int` to `std::string` | templated |
| API | `put`, `get` | the full table above |
| TTL | no | yes |
| Best for | reading the algorithm | shipping |

The classic version exists to be read. Both are checked against the same
conformance suite, so they behave identically.

## Building

```sh
make test   # build and run every test suite
make run    # interactive cache shell
make bench  # classic against production benchmark
make check  # everything CI runs, including the sanitizers and a leak check
make help   # the full list
```

CMake works everywhere, and is the only option on Windows because the Makefile
needs a POSIX shell:

```sh
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

CI builds and tests on Linux, macOS and Windows. Linux and macOS also run
AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer and a leak check.

## Tests

105 tests with no test framework to install, one binary per concern in `tests/`.
Three are worth knowing about:

* **conformance** runs a single suite against both caches, including 20,000
  random operations diffed against a deliberately naive reference LRU.
* **concurrency** releases 8 workers through a start gate so the operations
  really overlap, covering racing `get_or_compute`, `erase` and `clear` against
  live traffic, and counters that have to add up exactly.
* **error handling** throws from every copy and move an insert performs, then
  checks that the cache is still consistent and that nothing leaked.

The concurrency and error handling suites were validated against deliberately
broken builds, so they fail when the code is wrong rather than passing by luck.

## License

MIT. See [LICENSE](LICENSE).
