// An interactive shell for poking at a cache without writing any code.
//
//   make run
//   make run CAPACITY=3
//   echo "put a 1
//   get a
//   stats" | ./build/cache_shell
//
// Usage: cache_shell [capacity] [default_ttl_ms]

#include <lru/lru_cache.hpp>
#include <lru/version.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#if defined(_WIN32)
#include <io.h>
#define lru_isatty _isatty
#define lru_fileno _fileno
#else
#include <unistd.h>
#define lru_isatty isatty
#define lru_fileno fileno
#endif

namespace {

using Cache = lru::LRUCache<std::string, std::string>;

void print_help() {
    std::cout << "commands:\n"
                 "  put <key> <value> [ttl_ms]   insert or update\n"
                 "  add <key> <value> [ttl_ms]   insert only if absent\n"
                 "  get <key>                    read and mark most recently used\n"
                 "  peek <key>                   read without touching recency\n"
                 "  has <key>                    membership test\n"
                 "  del <key>                    remove an entry\n"
                 "  purge                        drop every expired entry\n"
                 "  clear                        drop everything\n"
                 "  size                         entries / capacity\n"
                 "  stats                        hits, misses, evictions, expirations\n"
                 "  reset                        zero the statistics\n"
                 "  help                         this text\n"
                 "  quit                         exit\n";
}

void print_value(const std::optional<std::string>& value) {
    if (value) {
        std::cout << *value << "\n";
    } else {
        std::cout << "(nil)\n";
    }
}

std::optional<std::chrono::milliseconds> parse_ttl(std::istringstream& line) {
    long long ttl_ms = 0;
    if (!(line >> ttl_ms)) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(ttl_ms);
}

// A key and then the rest of the line, minus an optional trailing TTL.
bool read_key_and_value(std::istringstream& line, std::string& key, std::string& value,
                        std::optional<std::chrono::milliseconds>& ttl) {
    if (!(line >> key) || !(line >> value)) {
        std::cout << "usage: put <key> <value> [ttl_ms]\n";
        return false;
    }
    ttl = parse_ttl(line);
    return true;
}

int run(Cache& cache, bool interactive) {
    std::string input;
    while (true) {
        if (interactive) {
            std::cout << "lru> " << std::flush;
        }
        if (!std::getline(std::cin, input)) {
            break;
        }

        std::istringstream line(input);
        std::string command;
        if (!(line >> command)) {
            continue;
        }

        try {
            if (command == "quit" || command == "exit") {
                break;
            } else if (command == "help") {
                print_help();
            } else if (command == "put" || command == "add") {
                std::string key;
                std::string value;
                std::optional<std::chrono::milliseconds> ttl;
                if (!read_key_and_value(line, key, value, ttl)) {
                    continue;
                }
                if (command == "put") {
                    ttl ? cache.put(key, value, *ttl) : cache.put(key, value);
                    std::cout << "OK\n";
                } else {
                    const bool inserted = ttl ? cache.insert_if_absent(key, value, *ttl)
                                              : cache.insert_if_absent(key, value);
                    std::cout << (inserted ? "OK\n" : "already present\n");
                }
            } else if (command == "get" || command == "peek" || command == "has" ||
                       command == "del") {
                std::string key;
                if (!(line >> key)) {
                    std::cout << "usage: " << command << " <key>\n";
                    continue;
                }
                if (command == "get") {
                    print_value(cache.get(key));
                } else if (command == "peek") {
                    print_value(cache.peek(key));
                } else if (command == "has") {
                    std::cout << (cache.contains(key) ? "yes\n" : "no\n");
                } else {
                    std::cout << (cache.erase(key) ? "deleted\n" : "not found\n");
                }
            } else if (command == "purge") {
                std::cout << "purged " << cache.purge_expired() << "\n";
            } else if (command == "clear") {
                cache.clear();
                std::cout << "OK\n";
            } else if (command == "size") {
                std::cout << cache.size() << " / " << cache.capacity() << "\n";
            } else if (command == "stats") {
                const lru::CacheStats stats = cache.stats();
                std::cout << "hits=" << stats.hits << " misses=" << stats.misses
                          << " evictions=" << stats.evictions
                          << " expirations=" << stats.expirations << "\n";
            } else if (command == "reset") {
                cache.reset_stats();
                std::cout << "OK\n";
            } else {
                std::cout << "unknown command '" << command << "'; try 'help'\n";
            }
        } catch (const std::exception& error) {
            std::cout << "error: " << error.what() << "\n";
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t capacity = 4;
    std::optional<std::chrono::milliseconds> default_ttl;

    try {
        if (argc > 1) {
            capacity = static_cast<std::size_t>(std::stoull(argv[1]));
        }
        if (argc > 2) {
            default_ttl = std::chrono::milliseconds(std::stoll(argv[2]));
        }
    } catch (const std::exception&) {
        std::cerr << "usage: " << argv[0] << " [capacity] [default_ttl_ms]\n";
        return 2;
    }

    try {
        Cache cache(capacity, default_ttl);

        const bool interactive = lru_isatty(lru_fileno(stdin)) != 0;
        if (interactive) {
            std::cout << "lru_cache " << lru::Version::string() << " shell, capacity "
                      << capacity;
            if (default_ttl) {
                std::cout << ", default ttl " << default_ttl->count() << "ms";
            }
            std::cout << "\nType 'help' for commands, 'quit' to exit.\n";
        }
        return run(cache, interactive);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 2;
    }
}
