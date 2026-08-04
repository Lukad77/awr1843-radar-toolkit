#pragma once
// SpscRing.h — 用于流水线各级之间的有界阻塞队列。
//
// 替代被误用的 UnlockQueue（其生产者在溢出分支里推进 `_out`，
// 与消费者竞争并静默丢弃/乱序帧）。这里的契约是显式且默认无损的：
//
//   push()     : 满则阻塞 -> 背压向上游传播。
//   try_push() : 永不阻塞，满则返回 false。供 socket 排空线程使用，
//                它绝不能阻塞（否则内核丢 UDP 包）。返回 false 时
//                改将数据路由到 FrameSpool。
//   pop()      : 空则阻塞，仅在 close() 且排空后返回 false。
//   try_pop()  : 永不阻塞。
//   close()    : 唤醒所有等待者，干净地关闭。
//
// 正确性优先：用 mutex + 两个条件变量实现。对多生产者/多消费者
// 也安全；无锁变体是后续（Phase 6）优化，可在同一接口后直接替换。

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace radar {

template <class T> class SpscRing {
public:
  explicit SpscRing(std::size_t capacity)
      : cap_(capacity < 1 ? 1 : capacity), buf_(cap_) {}

  // 带背压的阻塞 push。仅当环已关闭时返回 false。
  bool push(T v) {
    std::unique_lock<std::mutex> lk(m_);
    not_full_.wait(lk, [&] { return count_ < cap_ || closed_; });
    if (closed_)
      return false;
    buf_[tail_] = std::move(v);
    tail_ = (tail_ + 1) % cap_;
    ++count_;
    not_empty_.notify_one();
    return true;
  }

  // 非阻塞 push。满或已关闭时返回 false（永不阻塞、永不丢数据）。
  bool try_push(T v) {
    std::lock_guard<std::mutex> lk(m_);
    if (closed_ || count_ >= cap_)
      return false;
    buf_[tail_] = std::move(v);
    tail_ = (tail_ + 1) % cap_;
    ++count_;
    not_empty_.notify_one();
    return true;
  }

  // 阻塞 pop。仅当环已关闭且已排空时返回 false。
  bool pop(T &out) {
    std::unique_lock<std::mutex> lk(m_);
    not_empty_.wait(lk, [&] { return count_ > 0 || closed_; });
    if (count_ == 0)
      return false; // 已关闭且已排空
    out = std::move(buf_[head_]);
    head_ = (head_ + 1) % cap_;
    --count_;
    not_full_.notify_one();
    return true;
  }

  // 非阻塞 pop。空时返回 false。
  bool try_pop(T &out) {
    std::lock_guard<std::mutex> lk(m_);
    if (count_ == 0)
      return false;
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
