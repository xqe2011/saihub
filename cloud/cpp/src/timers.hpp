#pragma once

#include <cstdint>

namespace saihub {

// Intrusive doubly-linked list node for O(1) timer insertion/removal/LRU-bump.
struct TimerNode {
    TimerNode* prev = nullptr;
    TimerNode* next = nullptr;
    int64_t deadline_ms = 0;
    bool linked = false;
};

struct TimerList {
    TimerNode* head = nullptr;
    TimerNode* tail = nullptr;

    bool empty() const noexcept { return head == nullptr; }

    int64_t min_deadline() const noexcept {
        return head ? head->deadline_ms : INT64_MAX;
    }

    void push_back(TimerNode* node, int64_t deadline_ms) noexcept {
        if (node->linked) {
            remove(node);
        }
        node->deadline_ms = deadline_ms;
        node->prev = tail;
        node->next = nullptr;
        node->linked = true;
        if (tail) {
            tail->next = node;
        } else {
            head = node;
        }
        tail = node;
    }

    void remove(TimerNode* node) noexcept {
        if (!node->linked) return;
        if (node->prev) {
            node->prev->next = node->next;
        } else {
            head = node->next;
        }
        if (node->next) {
            node->next->prev = node->prev;
        } else {
            tail = node->prev;
        }
        node->prev = nullptr;
        node->next = nullptr;
        node->linked = false;
    }
};

} // namespace saihub
