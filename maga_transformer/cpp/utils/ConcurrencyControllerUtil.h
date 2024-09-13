#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

namespace rtp_llm {

class ConcurrencyController {
public:
    ConcurrencyController(int max_concurrency = 1, bool block = false):
        max_concurrency_(max_concurrency),
        block_(block),
        current_concurrency_(0) {}

    int get_available_concurrency() {
        std::unique_lock<std::mutex> scopeLock(mtx_);
        return max_concurrency_ - current_concurrency_;
    }
    bool increment() {
        while (true) {
            std::unique_lock<std::mutex> scopeLock(mtx_);
            if (current_concurrency_ < max_concurrency_) {
                current_concurrency_ += 1;
                return true;
            }
            if (block_) {
                FT_LOG_INFO("reach concurrency_controller limit, blocking...");
                cv_.wait(scopeLock);
            } else {
                FT_LOG_INFO("reach concurrency_controller limit, rejecting...");
                return false;
            }
        }
    }
    void decrement() {
        std::unique_lock<std::mutex> scopeLock(mtx_);
        current_concurrency_ -= 1;
        cv_.notify_one();
    }

private:
    int max_concurrency_;
    bool block_;
    int current_concurrency_;
    std::mutex mtx_;
    std::condition_variable cv_;
};

} // namespace rtp_llm
