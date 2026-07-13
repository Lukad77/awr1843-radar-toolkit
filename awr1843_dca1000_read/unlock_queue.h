#pragma once
//
// Created by wyb on 17-6-27.
// Modified (add template) by Weifan Gao on 23-04-30
//

#ifndef UNLOCKQUEUE_UNLOCK_QUEUE_H
#define UNLOCKQUEUE_UNLOCK_QUEUE_H

#include <stdint.h>
#include <atomic>
#include <algorithm>
#include <cstring>  // 添加 memcpy 需要的头文件
#include <thread>   // 添加 std::this_thread 需要的头文件
#include <chrono>   // 添加 std::chrono 需要的头文件

static inline bool is_power_of_2(uint32_t num) {
    return (num != 0 && (num & (num - 1)) == 0);
}

static inline uint32_t hightest_one_bit(uint32_t num) {
    num |= (num >> 1);
    num |= (num >> 2);
    num |= (num >> 4);
    num |= (num >> 8);
    num |= (num >> 16);
    return num - (num >> 1);
}

static inline uint32_t roundup_pow_of_two(uint32_t num) {
    return num > 1 ? hightest_one_bit((num - 1) << 1) : 1;
}

template <class T>
class UnlockQueue {
public:
    UnlockQueue(uint32_t size) : _in(0), _out(0) {
        _size = roundup_pow_of_two(size);
        _buffer = new T[_size];
    }

    ~UnlockQueue() {
        if (_buffer)  // 修复条件判断
            delete[] _buffer;
    }

    uint32_t Put(const T* buffer, uint32_t len) {
        // 修复内存序使用
        uint32_t out_val = _out.load(std::memory_order_acquire);
        if (len > _size - (_in.load(std::memory_order_relaxed) - out_val))//allow override
            _out.fetch_add(len, std::memory_order_release);

        uint32_t in_val = _in.load(std::memory_order_relaxed);
        uint32_t l = min(len, _size - (in_val & (_size - 1)));
        std::memcpy(_buffer + (in_val & (_size - 1)), buffer, l * sizeof(T));
        std::memcpy(_buffer, buffer + l, (len - l) * sizeof(T));

        _in.fetch_add(len, std::memory_order_release);
        return len;
    }

    /**
 * 从环形缓冲区中获取数据
 *
 * @param buffer 用于存储获取数据的缓冲区指针
 * @param len    请求获取的数据元素个数
 * @return       实际获取的数据元素个数
 *
 * 该函数是一个线程安全的环形缓冲区读取操作，使用原子操作和内存屏障
 * 来确保多线程环境下的数据一致性。
 */
    uint32_t Get(T* buffer, uint32_t len) {
        // 获取当前写入位置和读取位置
        uint32_t in_val = _in.load(std::memory_order_acquire);
        uint32_t out_val = _out.load(std::memory_order_relaxed);

        // 计算可读取的数据量，不超过请求长度
        len = min(len, in_val - out_val);

        // 计算第一段可连续读取的数据量（考虑环形缓冲区的边界情况）
        uint32_t l = min(len, _size - (out_val & (_size - 1)));

        // 从环形缓冲区复制第一段数据到目标缓冲区
        std::memcpy(buffer, _buffer + (out_val & (_size - 1)), l * sizeof(T));

        // 如果需要，复制第二段数据（环形缓冲区回绕部分）
        std::memcpy(buffer + l, _buffer, (len - l) * sizeof(T));

        // 更新读取位置指针
        _out.fetch_add(len, std::memory_order_release);
        return len;
    }

    uint32_t Get_wait(T* buffer, uint32_t len, uint32_t timeout_ms) {
        uint32_t cnt = 0;
        while (length() < len) {
            if (cnt++ >= timeout_ms)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return Get(buffer, len);
    }

    inline uint32_t size() { return _size; }
    inline uint32_t length() {
        return _in.load(std::memory_order_acquire) - _out.load(std::memory_order_acquire);
    }
    inline bool empty() {
        return _in.load(std::memory_order_acquire) <= _out.load(std::memory_order_acquire);
    }

private:
    T* _buffer;
    uint32_t _size;
    std::atomic<uint32_t> _in;
    std::atomic<uint32_t> _out;
};

#endif //UNLOCKQUEUE_UNLOCK_QUEUE_H