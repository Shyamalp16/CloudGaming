#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdint>

struct Packet {
    std::vector<uint8_t> data;
    int64_t pts;
};

template<typename T>
class ThreadSafeQueue
{
public:
    void push(T value)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(value));
        m_cv.notify_one();
    }

    bool pop(T& value)
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty() || m_shutdown; });
        if (m_shutdown && m_queue.empty()) {
            return false;
        }
        value = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    void shutdown() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_shutdown = true;
        m_cv.notify_all();
    }

private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_shutdown = false;
};

extern ThreadSafeQueue<Packet> g_packetQueue;
