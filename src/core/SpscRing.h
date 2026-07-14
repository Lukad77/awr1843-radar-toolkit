#pragma once
// SpscRing.h — bounded, blocking queue used between pipeline stages.
//
// This replaces the misuse of UnlockQueue (whose producer advanced `_out` in
// its overflow branch, racing the consumer and silently dropping/reordering
// frames). Here the contract is explicit and lossless-by-default:
//
//   push()     : BLOCKS when full  -> backpressure propagates upstream.
//   try_push() : NEVER blocks, returns false when full. Used by the socket
//                drain thread, which must never block (else the kernel drops
//                UDP). On false it routes the item to the FrameSpool instead.
//   pop()      : BLOCKS when empty, returns false only after close() && drained.
//   try_pop()  : NEVER blocks.
//   close()    : wakes all waiters for a clean shutdown.
//
// Correctness first: implemented with a mutex + two condition variables. It is
// safe for multiple producers/consumers; a lock-free variant is a later
// (Phase 6) optimization and can drop in behind the same interface.

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace radar {

template <class T>
class SpscRing {
public:
    explicit SpscRing(std::size_t capacity)
        : cap_(capacity < 1 ? 1 : capacity), buf_(cap_) {}

    // Blocking push with backpressure. Returns false iff the ring was closed.
    bool push(T v) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return count_ < cap_ || closed_; });
        if (closed_) return false;
        buf_[tail_] = std::move(v);
        tail_ = (tail_ + 1) % cap_;
        ++count_;
        not_empty_.notify_one();
        return true;
    }

    // Non-blocking push. Returns false when full or closed (never blocks, never drops).
    bool try_push(T v) {
        std::lock_guard<std::mutex> lk(m_);
        if (closed_ || count_ >= cap_) return false;
        buf_[tail_] = std::move(v);
        tail_ = (tail_ + 1) % cap_;
        ++count_;
        not_empty_.notify_one();
        return true;
    }

    // Blocking pop. Returns false only when the ring is closed AND drained.
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return count_ > 0 || closed_; });
        if (count_ == 0) return false; // closed & drained
        out = std::move(buf_[head_]);
        head_ = (head_ + 1) % cap_;
        --count_;
        not_full_.notify_one();
        return true;
    }

    // Non-blocking pop. Returns false when empty.
    bool try_pop(T& out) {
        std::lock_guard<std::mutex> lk(m_);
        if (count_ == 0) return false;
        out = std::move(buf_[head_]);
        head_ = (head_ + 1) % cap_;
        --count_;
        not_full_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(m_);
            closed_ = true;
        }
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return count_;
    }
    std::size_t capacity() const noexcept { return cap_; }
    bool closed() const {
        std::lock_guard<std::mutex> lk(m_);
        return closed_;
    }

private:
    mutable std::mutex m_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::size_t cap_;
    std::vector<T> buf_;
    std::size_t head_ = 0, tail_ = 0, count_ = 0;
    bool closed_ = false;
};

} // namespace radar
