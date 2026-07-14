#pragma once
// BufferPool.h — thread-safe object pool to eliminate per-frame allocation.
//
// acquire() returns a std::shared_ptr<T> whose custom deleter *returns* the
// object to the pool instead of freeing it. The pool keeps itself alive (via
// shared_from_this captured in the deleter) as long as any buffer is
// outstanding, so returns are always safe.
//
// Typical use:
//   auto pool = BufferPool<FrameBuffer>::create(
//       []{ return std::make_unique<FrameBuffer>(shape); }, nullptr, /*prealloc*/16);
//   auto fb = pool->acquire();   // recycled buffer, ready to fill

#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace radar {

template <class T>
class BufferPool : public std::enable_shared_from_this<BufferPool<T>> {
public:
    using Factory = std::function<std::unique_ptr<T>()>;
    using Reset = std::function<void(T&)>;

    static std::shared_ptr<BufferPool> create(Factory factory, Reset reset = {},
                                              std::size_t prealloc = 0) {
        auto p = std::shared_ptr<BufferPool>(new BufferPool(std::move(factory), std::move(reset)));
        for (std::size_t i = 0; i < prealloc; ++i) p->free_.push_back(p->factory_());
        return p;
    }

    std::shared_ptr<T> acquire() {
        std::unique_ptr<T> obj;
        {
            std::lock_guard<std::mutex> lk(m_);
            if (!free_.empty()) {
                obj = std::move(free_.back());
                free_.pop_back();
            }
        }
        if (!obj) obj = factory_();
        if (reset_) reset_(*obj);

        auto self = this->shared_from_this();
        T* raw = obj.release();
        return std::shared_ptr<T>(raw, [self](T* p) {
            std::unique_ptr<T> up(p);
            std::lock_guard<std::mutex> lk(self->m_);
            self->free_.push_back(std::move(up));
        });
    }

    std::size_t free_count() const {
        std::lock_guard<std::mutex> lk(m_);
        return free_.size();
    }

private:
    BufferPool(Factory factory, Reset reset)
        : factory_(std::move(factory)), reset_(std::move(reset)) {}

    mutable std::mutex m_;
    Factory factory_;
    Reset reset_;
    std::vector<std::unique_ptr<T>> free_;
};

} // namespace radar
