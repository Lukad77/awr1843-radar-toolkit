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
#include <cstring>  // ���� memcpy ��Ҫ��ͷ�ļ�
#include <thread>   // ���� std::this_thread ��Ҫ��ͷ�ļ�
#include <chrono>   // ���� std::chrono ��Ҫ��ͷ�ļ�

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
        if (_buffer)  // �޸������ж�
            delete[] _buffer;
    }

    uint32_t Put(const T* buffer, uint32_t len) {
        // �޸��ڴ���ʹ��
        uint32_t out_val = _out.load(std::memory_order_acquire);
        if (len > _size - (_in.load(std::memory_order_relaxed) - out_val))//allow override
            _out.fetch_add(len, std::memory_order_release);

        uint32_t in_val = _in.load(std::memory_order_relaxed);
        uint32_t l = std::min(len, _size - (in_val & (_size - 1)));
        std::memcpy(_buffer + (in_val & (_size - 1)), buffer, l * sizeof(T));
        std::memcpy(_buffer, buffer + l, (len - l) * sizeof(T));

        _in.fetch_add(len, std::memory_order_release);
        return len;
    }

    /**
 * �ӻ��λ������л�ȡ����
 *
 * @param buffer ���ڴ洢��ȡ���ݵĻ�����ָ��
 * @param len    �����ȡ������Ԫ�ظ���
 * @return       ʵ�ʻ�ȡ������Ԫ�ظ���
 *
 * �ú�����һ���̰߳�ȫ�Ļ��λ�������ȡ������ʹ��ԭ�Ӳ������ڴ�����
 * ��ȷ�����̻߳����µ�����һ���ԡ�
 */
    uint32_t Get(T* buffer, uint32_t len) {
        // ��ȡ��ǰд��λ�úͶ�ȡλ��
        uint32_t in_val = _in.load(std::memory_order_acquire);
        uint32_t out_val = _out.load(std::memory_order_relaxed);

        // ����ɶ�ȡ�������������������󳤶�
        len = std::min(len, in_val - out_val);

        // �����һ�ο�������ȡ�������������ǻ��λ������ı߽������
        uint32_t l = std::min(len, _size - (out_val & (_size - 1)));

        // �ӻ��λ��������Ƶ�һ�����ݵ�Ŀ�껺����
        std::memcpy(buffer, _buffer + (out_val & (_size - 1)), l * sizeof(T));

        // �����Ҫ�����Ƶڶ������ݣ����λ��������Ʋ��֣�
        std::memcpy(buffer + l, _buffer, (len - l) * sizeof(T));

        // ���¶�ȡλ��ָ��
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