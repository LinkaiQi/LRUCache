// Tests for the classic cache: the shared conformance suite plus tests for the
// LinkedList building block, which the production version does not have.

#include <lru/lru_cache_classic.hpp>

#include "conformance.hpp"
#include "test_harness.hpp"

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using lru::classic::LinkedList;
using lru::classic::LRUCache;
using lru::classic::Node;

// ---------------------------------------------------------------------------
// LinkedList
// ---------------------------------------------------------------------------

TEST(linked_list_starts_empty) {
    LinkedList list;
    CHECK(list.empty());
    CHECK(list.remove_last() == nullptr);
}

TEST(create_node_initialises_fields) {
    Node* node = LinkedList::CreateNode(7, "seven");
    CHECK_EQ(node->key, 7);
    CHECK_EQ(node->value, std::string("seven"));
    CHECK(node->previous == nullptr);
    CHECK(node->next == nullptr);
    delete node;
}

TEST(add_first_then_remove_last_is_fifo_by_recency) {
    LinkedList list;
    list.add_first(LinkedList::CreateNode(1, "one"));
    list.add_first(LinkedList::CreateNode(2, "two"));
    list.add_first(LinkedList::CreateNode(3, "three"));
    CHECK(!list.empty());

    Node* last = list.remove_last();
    CHECK(last != nullptr);
    CHECK_EQ(last->key, 1);
    delete last;

    last = list.remove_last();
    CHECK_EQ(last->key, 2);
    delete last;

    last = list.remove_last();
    CHECK_EQ(last->key, 3);
    delete last;

    CHECK(list.empty());
    CHECK(list.remove_last() == nullptr);
}

TEST(add_first_on_a_linked_node_moves_it_to_the_front) {
    LinkedList list;
    Node* one = LinkedList::CreateNode(1, "one");
    Node* two = LinkedList::CreateNode(2, "two");
    Node* three = LinkedList::CreateNode(3, "three");
    list.add_first(one);
    list.add_first(two);
    list.add_first(three);  // order: 3, 2, 1

    list.add_first(one);  // order: 1, 3, 2

    Node* last = list.remove_last();
    CHECK_EQ(last->key, 2);
    delete last;
    last = list.remove_last();
    CHECK_EQ(last->key, 3);
    delete last;
    last = list.remove_last();
    CHECK_EQ(last->key, 1);
    delete last;
    CHECK(list.empty());
}

TEST(add_first_on_the_head_is_a_no_op) {
    LinkedList list;
    Node* one = LinkedList::CreateNode(1, "one");
    Node* two = LinkedList::CreateNode(2, "two");
    list.add_first(one);
    list.add_first(two);  // order: 2, 1

    list.add_first(two);  // already the head
    list.add_first(two);

    Node* last = list.remove_last();
    CHECK_EQ(last->key, 1);
    delete last;
    last = list.remove_last();
    CHECK_EQ(last->key, 2);
    delete last;
    CHECK(list.empty());
}

TEST(add_first_on_a_single_element_list) {
    LinkedList list;
    Node* only = LinkedList::CreateNode(1, "one");
    list.add_first(only);
    list.add_first(only);  // head and tail at the same time

    Node* last = list.remove_last();
    CHECK(last == only);
    CHECK_EQ(last->key, 1);
    delete last;
    CHECK(list.empty());
}

TEST(add_first_ignores_nullptr) {
    LinkedList list;
    list.add_first(nullptr);
    CHECK(list.empty());
}

TEST(clear_empties_the_list) {
    LinkedList list;
    list.add_first(LinkedList::CreateNode(1, "one"));
    list.add_first(LinkedList::CreateNode(2, "two"));
    list.clear();
    CHECK(list.empty());
    CHECK(list.remove_last() == nullptr);

    // Reusable after clear.
    list.add_first(LinkedList::CreateNode(3, "three"));
    Node* last = list.remove_last();
    CHECK_EQ(last->key, 3);
    delete last;
}

// The destructor must free whatever is left; ASan and `make leaks` verify it.
TEST(destructor_frees_remaining_nodes) {
    LinkedList list;
    for (int i = 0; i < 100; ++i) {
        list.add_first(LinkedList::CreateNode(i, "value " + std::to_string(i)));
    }
    CHECK(!list.empty());
}

// ---------------------------------------------------------------------------
// LRUCache
// ---------------------------------------------------------------------------

TEST(non_positive_size_is_rejected) {
    bool threw_on_zero = false;
    try {
        LRUCache cache(0);
    } catch (const std::invalid_argument&) {
        threw_on_zero = true;
    }
    CHECK(threw_on_zero);

    bool threw_on_negative = false;
    try {
        LRUCache cache(-1);
    } catch (const std::invalid_argument&) {
        threw_on_negative = true;
    }
    CHECK(threw_on_negative);
}

TEST(cache_destructor_frees_its_nodes) {
    LRUCache cache(64);
    for (int i = 0; i < 500; ++i) {
        cache.put(i, "value " + std::to_string(i));
    }
    CHECK_EQ(cache.get(499), std::string("value 499"));
}

TEST(large_values_round_trip) {
    LRUCache cache(2);
    std::string value(4096, 'x');  // far past the small-string buffer
    cache.put(1, std::move(value));

    const auto stored = cache.get(1);
    CHECK(stored.has_value());
    CHECK_EQ(stored->size(), std::size_t{4096});
    CHECK_EQ(*stored, std::string(4096, 'x'));
}

}  // namespace

int main() {
    lru_test::register_conformance_tests("classic", [](std::size_t capacity) {
        return std::make_unique<LRUCache>(static_cast<int>(capacity));
    });
    return lru_test::run_all("classic LRUCache");
}
