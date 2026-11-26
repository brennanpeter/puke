#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace puke {

// A small fixed-size thread pool with one operation: parallelFor.
//
// The calling thread always does work too, so parallelFor makes progress even
// if every worker is busy, and calling it from inside a task can't deadlock.
// A pool with 1 thread has no workers and runs everything on the caller.
class ThreadPool {
public:
    // threads: total threads doing work, including the caller.
    // 0 means one per hardware thread.
    explicit ThreadPool(int threads = 0);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    int threads() const { return static_cast<int>(workers_.size()) + 1; }

    // Calls fn(i) for every i in [0, count), spread across the pool, and
    // returns when all calls have finished.
    //
    // If a call throws, indices not yet started are skipped, and once the
    // running calls finish the exception is rethrown here. If several calls
    // threw, the one with the lowest index wins.
    void parallelFor(std::size_t count, const std::function<void(std::size_t)>& fn);

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    bool stopping_ = false;
};

}  // namespace puke
