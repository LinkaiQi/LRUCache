// lru/lru_cache.hpp: a thread-safe, fixed-capacity LRU cache with optional TTL.
//
// Design notes
// ------------
//  * O(1) put/get: an intrusive doubly linked list tracks recency (head = most
//    recently used, tail = least recently used) and a hash map gives constant
//    time lookup from key to node.
//  * Nodes are owned by the cache. Every node reachable from m_head is owned
//    exactly once and is also referenced by exactly one map entry.
//  * All operations take an exclusive lock. A shared mutex would not help:
//    get() updates recency, so reads mutate state too.
//  * At capacity, put() recycles the evicted node instead of freeing it and
//    allocating a new one.
//  * TTL is opt-in and lazy. Entries expire on access and nothing is reclaimed
//    in the background. Call purge_expired() to reclaim entries that expired but
//    were never touched again. An entry with no TTL costs no clock reads.
//  * Exception safety: the basic guarantee. The cache is always left in a
//    consistent state (map and list agree); a failed put may have already
//    evicted the least recently used entry. Hash and key comparison are
//    assumed not to throw.
//
// Thread safety
// -------------
// Every public method is safe to call concurrently. Move construction and move
// assignment are not: like any standard container, a cache must not be moved
// while other threads are using it.
//
// Compound operations such as "insert only if missing" are racy when built from
// separate calls. Use insert_if_absent() or get_or_compute() instead of
// if (!cache.contains(k)) cache.put(k, ...).
//
// Requires C++17.

#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace lru {

struct CacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;    ///< entries dropped to make room
    std::size_t expirations = 0;  ///< entries dropped because their TTL had passed
};

template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>,
          typename Clock = std::chrono::steady_clock>
class LRUCache {
public:
    using key_type = Key;
    using mapped_type = Value;
    using size_type = std::size_t;
    using clock_type = Clock;
    using duration = typename Clock::duration;
    using time_point = typename Clock::time_point;

    /// @param capacity     maximum number of entries. Must be greater than zero.
    /// @param default_ttl  time to live applied to entries inserted without an
    ///                     explicit TTL. std::nullopt means entries never expire.
    /// @throws std::invalid_argument if capacity is zero or default_ttl is not positive.
    explicit LRUCache(size_type capacity, std::optional<duration> default_ttl = std::nullopt)
        : m_capacity(capacity), m_default_ttl(default_ttl) {
        if (capacity == 0) {
            throw std::invalid_argument("LRUCache: capacity must be greater than zero");
        }
        validate_ttl(default_ttl);
        m_map.reserve(capacity);
    }

    ~LRUCache() { destroy_nodes(); }

    // Copying would double free the nodes, and is almost always a mistake anyway.
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    // A moved-from cache keeps its capacity and default TTL, and is left empty
    // but usable.
    LRUCache(LRUCache&& other) {
        std::lock_guard<std::mutex> lock(other.m_mutex);
        steal_from(other);
    }

    LRUCache& operator=(LRUCache&& other) {
        if (this != &other) {
            std::scoped_lock lock(m_mutex, other.m_mutex);
            destroy_nodes();
            steal_from(other);
        }
        return *this;
    }

    // Writes

    /// Inserts or updates key and marks it most recently used, evicting the
    /// least recently used entry when the cache is full. Uses the default TTL.
    void put(const Key& key, Value value) { put_impl(key, std::move(value), m_default_ttl); }

    /// As above, with an explicit TTL. Pass std::nullopt so this entry never expires.
    /// @throws std::invalid_argument if ttl is present and not positive.
    void put(const Key& key, Value value, std::optional<duration> ttl) {
        put_impl(key, std::move(value), ttl);
    }

    /// Inserts only if the key is absent or expired. Atomic, unlike
    /// `if (!contains(k)) put(k, v)`.
    /// @return true if the value was inserted.
    bool insert_if_absent(const Key& key, Value value) {
        return insert_if_absent(key, std::move(value), m_default_ttl);
    }

    bool insert_if_absent(const Key& key, Value value, std::optional<duration> ttl) {
        const time_point expiry = expiry_from(ttl);
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            if (!is_expired(it->second)) {
                return false;
            }
            drop(it);
            ++m_stats.expirations;
        }
        insert_new(key, std::move(value), expiry);
        return true;
    }

    /// Returns the cached value, computing and storing it first if absent or expired.
    ///
    /// The factory runs *without* the lock held, so a slow factory never blocks
    /// other threads and can safely touch this cache. The trade-off is that
    /// concurrent callers may each compute a value for the same key. The first
    /// one to finish wins and the others discard their result.
    template <typename Factory>
    Value get_or_compute(const Key& key, Factory&& factory) {
        return get_or_compute(key, std::forward<Factory>(factory), m_default_ttl);
    }

    template <typename Factory>
    Value get_or_compute(const Key& key, Factory&& factory, std::optional<duration> ttl) {
        if (std::optional<Value> existing = get(key)) {
            return std::move(*existing);
        }
        Value computed = factory();
        const time_point expiry = expiry_from(ttl);

        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            if (!is_expired(it->second)) {
                ++m_stats.hits;
                move_to_front(it->second);
                return it->second->value;  // another thread won the race
            }
            drop(it);
            ++m_stats.expirations;
        }
        insert_new(key, std::move(computed), expiry);
        return m_head->value;  // insert_new always puts the new node at the front
    }

    // Reads

    /// Returns the value and marks the entry most recently used. An expired
    /// entry counts as a miss and is removed.
    ///
    /// The value is returned by value so it stays valid after the lock is
    /// released: returning a reference would dangle the moment another thread
    /// evicted the entry.
    std::optional<Value> get(const Key& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            ++m_stats.misses;
            return std::nullopt;
        }
        if (is_expired(it->second)) {
            drop(it);
            ++m_stats.expirations;
            ++m_stats.misses;
            return std::nullopt;
        }
        ++m_stats.hits;
        move_to_front(it->second);
        return it->second->value;
    }

    /// Reads a value without affecting recency or statistics. Expired entries
    /// are reported as absent but, because this method is const, are not removed.
    std::optional<Value> peek(const Key& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end() || is_expired(it->second)) {
            return std::nullopt;
        }
        return it->second->value;
    }

    /// Does not affect recency. Expired entries are reported as absent.
    bool contains(const Key& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        return it != m_map.end() && !is_expired(it->second);
    }

    // Removal

    /// @return true if an entry was removed.
    bool erase(const Key& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_map.find(key);
        if (it == m_map.end()) {
            return false;
        }
        drop(it);
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        destroy_nodes();
    }

    /// Removes every entry whose TTL has passed. Expiration is otherwise lazy,
    /// so this is how memory is reclaimed for entries that are never read again.
    /// @return the number of entries removed.
    size_type purge_expired() {
        std::lock_guard<std::mutex> lock(m_mutex);
        const time_point now = Clock::now();
        size_type removed = 0;
        Node* node = m_head;
        while (node != nullptr) {
            Node* next = node->next;
            if (is_expired_at(node, now)) {
                m_map.erase(node->key);
                detach(node);
                delete node;
                ++removed;
            }
            node = next;
        }
        m_stats.expirations += removed;
        return removed;
    }

    // Observers

    /// Number of stored entries, including any that have expired but have not
    /// been purged yet. Use purge_expired() first for a live count.
    size_type size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_map.size();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_map.empty();
    }

    size_type capacity() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_capacity;
    }

    std::optional<duration> default_ttl() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_default_ttl;
    }

    CacheStats stats() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_stats;
    }

    void reset_stats() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats = CacheStats{};
    }

private:
    struct Node {
        Node* prev = nullptr;
        Node* next = nullptr;
        Key key;
        Value value;
        time_point expires_at;

        Node(const Key& node_key, Value&& node_value, time_point expiry)
            : key(node_key), value(std::move(node_value)), expires_at(expiry) {}
    };

    using Map = std::unordered_map<Key, Node*, Hash, KeyEqual>;

    static void validate_ttl(const std::optional<duration>& ttl) {
        if (ttl && *ttl <= duration::zero()) {
            throw std::invalid_argument("LRUCache: ttl must be positive");
        }
    }

    // Never reads the clock when there is no TTL, which keeps the no-TTL path
    // exactly as cheap as it was before TTL existed.
    static time_point expiry_from(const std::optional<duration>& ttl) {
        if (!ttl) {
            return time_point::max();
        }
        validate_ttl(ttl);
        const time_point now = Clock::now();
        if (*ttl >= time_point::max() - now) {
            return time_point::max();  // saturate instead of overflowing
        }
        return now + *ttl;
    }

    static bool never_expires(time_point expiry) noexcept { return expiry == time_point::max(); }

    static bool is_expired_at(const Node* node, time_point now) noexcept {
        return !never_expires(node->expires_at) && node->expires_at <= now;
    }

    static bool is_expired(const Node* node) {
        return !never_expires(node->expires_at) && node->expires_at <= Clock::now();
    }

    void put_impl(const Key& key, Value&& value, const std::optional<duration>& ttl) {
        const time_point expiry = expiry_from(ttl);  // throws before anything is locked
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_map.find(key);
        if (it != m_map.end()) {
            Node* node = it->second;
            node->value = std::move(value);
            node->expires_at = expiry;
            move_to_front(node);
            return;
        }
        insert_new(key, std::move(value), expiry);
    }

    // Caller must hold the lock and must have established that key is absent.
    void insert_new(const Key& key, Value&& value, time_point expiry) {
        // At capacity, reuse the least recently used node rather than freeing it
        // and allocating a replacement. In steady state this removes one free
        // and one allocation per insertion, and assigning over the old value
        // lets it reuse its own storage (a std::string keeps its buffer).
        if (m_map.size() >= m_capacity && m_tail != nullptr) {
            Node* recycled = m_tail;
            m_map.erase(recycled->key);
            detach(recycled);
            ++m_stats.evictions;
            try {
                recycled->key = key;
                recycled->value = std::move(value);
                recycled->expires_at = expiry;
                m_map.emplace(recycled->key, recycled);
            } catch (...) {
                // Owned by nobody at this point, so drop it. The cache is one
                // entry smaller but still perfectly consistent.
                delete recycled;
                throw;
            }
            push_front(recycled);
            return;
        }

        // Allocate before touching the cache so an allocation failure leaves it
        // untouched. The unique_ptr owns the node until the map insertion, the
        // only step after this point that can throw, succeeds.
        auto node = std::make_unique<Node>(key, std::move(value), expiry);
        m_map.emplace(node->key, node.get());
        push_front(node.release());
    }

    // Removes the entry an iterator points at. Caller must hold the lock.
    void drop(typename Map::iterator it) {
        Node* node = it->second;
        m_map.erase(it);
        detach(node);
        delete node;
    }

    void detach(Node* node) noexcept {
        if (node->prev != nullptr) {
            node->prev->next = node->next;
        } else {
            m_head = node->next;
        }
        if (node->next != nullptr) {
            node->next->prev = node->prev;
        } else {
            m_tail = node->prev;
        }
        node->prev = nullptr;
        node->next = nullptr;
    }

    void push_front(Node* node) noexcept {
        node->prev = nullptr;
        node->next = m_head;
        if (m_head != nullptr) {
            m_head->prev = node;
        }
        m_head = node;
        if (m_tail == nullptr) {
            m_tail = node;
        }
    }

    void move_to_front(Node* node) noexcept {
        if (node == m_head) {
            return;
        }
        detach(node);
        push_front(node);
    }

    void destroy_nodes() noexcept {
        Node* node = m_head;
        while (node != nullptr) {
            Node* next = node->next;
            delete node;
            node = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
        m_map.clear();
    }

    // Caller must hold both locks. *this must own no nodes.
    void steal_from(LRUCache& other) {
        m_capacity = other.m_capacity;
        m_default_ttl = other.m_default_ttl;
        m_map = std::move(other.m_map);
        m_head = other.m_head;
        m_tail = other.m_tail;
        m_stats = other.m_stats;

        other.m_map.clear();
        other.m_head = nullptr;
        other.m_tail = nullptr;
        other.m_stats = CacheStats{};
    }

    mutable std::mutex m_mutex;
    size_type m_capacity = 0;
    std::optional<duration> m_default_ttl;
    Node* m_head = nullptr;  // most recently used
    Node* m_tail = nullptr;  // least recently used
    Map m_map;
    CacheStats m_stats;
};

}  // namespace lru
