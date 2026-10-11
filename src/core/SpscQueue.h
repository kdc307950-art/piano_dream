#pragma once
//
// core/SpscQueue.h —— 单生产者 / 单消费者无锁环形队列
//
// 用途：输入线程（主线程）push，音频回调线程 pop。
// 约束：容量为 2 的幂；push/pop 不分配内存、不加锁、不阻塞，
//       满足音频实时线程的安全要求（见 README「音频线程约束」）。
//
#include <array>
#include <atomic>
#include <cstddef>

namespace piano {

template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    /// 生产者线程调用。队列满时返回 false（丢弃本条，绝不阻塞音频线程）。
    bool push(const T& value) noexcept {
        const std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t next = (w + 1) & kMask;
        if (next == read_.load(std::memory_order_acquire)) return false;  // full
        buffer_[w] = value;
        write_.store(next, std::memory_order_release);
        return true;
    }

    /// 消费者线程（音频回调）调用。
    bool pop(T& out) noexcept {
        const std::size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;    // empty
        out = buffer_[r];
        read_.store((r + 1) & kMask, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return read_.load(std::memory_order_acquire) == write_.load(std::memory_order_acquire);
    }

    std::size_t size() const noexcept {
        return (write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire)) & kMask;
    }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

} // namespace piano
