#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

namespace streamforge {

// Single-producer/single-consumer ring. One slot is reserved to distinguish full
// from empty, so usable capacity is Capacity - 1. clear() requires both sides stopped.
template <typename T, std::size_t Capacity>
class SpscRingBuffer final {
    static_assert(Capacity >= 2, "SPSC ring capacity must be at least two");
    static_assert(std::is_move_assignable_v<T>, "SPSC item must be move assignable");
    static_assert(std::is_move_constructible_v<T>, "SPSC item must be move constructible");

public:
    [[nodiscard]] bool tryPush(T&& value) {
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);
        if (next == m_tail.load(std::memory_order_acquire)) return false;

        m_slots[head].emplace(std::move(value));
        m_head.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool tryPop(T& value) {
        const std::size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return false;

        value = std::move(*m_slots[tail]);
        m_slots[tail].reset();
        m_tail.store(increment(tail), std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return m_tail.load(std::memory_order_acquire) ==
               m_head.load(std::memory_order_acquire);
    }

    void clear() noexcept {
        for (auto& slot : m_slots) slot.reset();
        const std::size_t head = m_head.load(std::memory_order_relaxed);
        m_tail.store(head, std::memory_order_relaxed);
    }

    [[nodiscard]] static constexpr std::size_t usableCapacity() noexcept {
        return Capacity - 1;
    }

private:
    [[nodiscard]] static constexpr std::size_t increment(std::size_t index) noexcept {
        return (index + 1U) % Capacity;
    }

    std::array<std::optional<T>, Capacity> m_slots{};
    alignas(64) std::atomic<std::size_t> m_head{0};
    alignas(64) std::atomic<std::size_t> m_tail{0};
};

} // namespace streamforge
