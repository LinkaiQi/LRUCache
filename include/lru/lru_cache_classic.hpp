// lru_cache_classic.hpp: the original design.
//
// This is the "classic" version: it keeps the shape of the original sketch on
// purpose. A standalone LinkedList class owns the recency list, the cache is
// concrete (int -> std::string) rather than templated, and the public surface is
// just put() and get().
//
// For a templated cache with peek/contains/erase/clear/stats, move support and
// node recycling, use lru_cache.hpp instead. The two are behaviourally identical
// for put/get, and tests/conformance.hpp runs the same suite against both.
//
// O(1) put/get: the doubly linked list tracks recency (head = most recently
// used, tail = least recently used) and the hash map gives constant time lookup
// from key to node, so neither operation ever walks the list. This matches
// lru_cache.hpp. The two differ only in the constant factor, because at
// capacity this version frees the evicted node and allocates a new one where
// lru_cache.hpp overwrites the evicted node in place.
//
// Ownership: LinkedList owns every node it holds and frees them in its
// destructor. LRUCache::m_map holds non-owning pointers to those same nodes. A
// node detached with remove_last() is handed back to the caller, which becomes
// responsible for deleting it.
//
// Requires C++17.

#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace lru {
namespace classic {

struct Node {
    int key;
    std::string value;
    Node* previous = nullptr;
    Node* next = nullptr;

    Node(int node_key, std::string node_value)
        : key(node_key), value(std::move(node_value)) {}
};

// Doubly linked list of nodes, most recently used first.
class LinkedList {
public:
    LinkedList() = default;
    ~LinkedList() { clear(); }

    // The list owns raw pointers, so copying it would double free.
    LinkedList(const LinkedList&) = delete;
    LinkedList& operator=(const LinkedList&) = delete;

    static Node* CreateNode(int key, std::string value) {
        return new Node(key, std::move(value));
    }

    // Inserts the node at the front. If the node is already in the list it is
    // unlinked first, so this doubles as "mark as most recently used".
    void add_first(Node* node) noexcept {
        if (node == nullptr || node == m_head) {
            return;
        }
        detach(node);
        node->previous = nullptr;
        node->next = m_head;
        if (m_head != nullptr) {
            m_head->previous = node;
        }
        m_head = node;
        if (m_tail == nullptr) {
            m_tail = node;
        }
    }

    // Detaches and returns the least recently used node, or nullptr when the
    // list is empty. Ownership passes to the caller.
    Node* remove_last() noexcept {
        Node* last = m_tail;
        if (last == nullptr) {
            return nullptr;
        }
        detach(last);
        return last;
    }

    void clear() noexcept {
        Node* node = m_head;
        while (node != nullptr) {
            Node* next = node->next;
            delete node;
            node = next;
        }
        m_head = nullptr;
        m_tail = nullptr;
    }

    bool empty() const noexcept { return m_head == nullptr; }

private:
    // Unlinks a node from the list. Safe to call on a node that is not linked:
    // a fresh node has null neighbours and is neither head nor tail, so every
    // branch below is skipped.
    void detach(Node* node) noexcept {
        if (node->previous != nullptr) {
            node->previous->next = node->next;
        } else if (m_head == node) {
            m_head = node->next;
        }
        if (node->next != nullptr) {
            node->next->previous = node->previous;
        } else if (m_tail == node) {
            m_tail = node->previous;
        }
        node->previous = nullptr;
        node->next = nullptr;
    }

    Node* m_head = nullptr;  // most recently used
    Node* m_tail = nullptr;  // least recently used
};

class LRUCache {
public:
    // Throws std::invalid_argument when size is not positive.
    explicit LRUCache(int size) {
        if (size <= 0) {
            throw std::invalid_argument("LRUCache: size must be greater than zero");
        }
        m_size = static_cast<std::size_t>(size);
        m_map.reserve(m_size);
    }

    // Owns raw nodes and holds a mutex, so it is neither copyable nor movable.
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    void put(int key, std::string val) {
        std::lock_guard<std::mutex> lock(m_mutex);

        // check if data already in the cache, if yes, move the data to be most
        // recently used (front)
        auto it = m_map.find(key);
        if (it != m_map.end()) {
            it->second->value = std::move(val);
            m_linkedList.add_first(it->second);
            return;
        }

        // delete the oldest data
        if (m_map.size() >= m_size) {
            Node* oldest = m_linkedList.remove_last();
            if (oldest != nullptr) {
                m_map.erase(oldest->key);
                delete oldest;
            }
        }

        // insert into LRU cache. The node is not owned by the list until
        // add_first() succeeds, so a throwing map insertion must free it here.
        Node* node = LinkedList::CreateNode(key, std::move(val));
        try {
            m_map.emplace(key, node);
        } catch (...) {
            delete node;
            throw;
        }
        m_linkedList.add_first(node);
    }

    std::optional<std::string> get(int key) {
        std::optional<std::string> result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_map.find(key);
            if (it == m_map.end()) {
                return std::nullopt;
            }
            // cache will put the data to the front as most recently used
            m_linkedList.add_first(it->second);
            // Copied while the lock is held: returning a reference would dangle
            // as soon as another thread evicted this entry.
            result = it->second->value;
        }
        return result;
    }

private:
    std::size_t m_size = 0;                // capacity
    LinkedList m_linkedList;               // owns the nodes
    std::unordered_map<int, Node*> m_map;  // non-owning pointers into the list
    mutable std::mutex m_mutex;
};

}  // namespace classic
}  // namespace lru
