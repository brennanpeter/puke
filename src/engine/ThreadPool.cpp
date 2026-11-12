#include "comp/engine/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <memory>

namespace comp {
namespace {

// Shared state for one parallelFor call. Helpers queued on the pool hold a
// shared_ptr to it, so it outlives the call even if a helper only gets
// scheduled after all the work is done.
struct Job {
    Job(std::size_t n, const std::function<void(std::size_t)>& f) : count(n), fn(f) {}

    const std::size_t count;
    const std::function<void(std::size_t)>& fn;  // valid until `finished == count`
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};

    std::mutex mutex;
    std::condition_variable allDone;
    std::size_t finished = 0;  // indices claimed and dealt with; guarded by mutex
    std::size_t errorIndex = std::numeric_limits<std::size_t>::max();
    std::exception_ptr error;

    // Claims and runs indices until none are left.
    void work() {
        for (;;) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) return;

            std::exception_ptr err;
            if (!failed.load(std::memory_order_relaxed)) {
                try {
                    fn(i);
                } catch (...) {
                    err = std::current_exception();
                    failed.store(true, std::memory_order_relaxed);
                }
            }

            std::lock_guard lock(mutex);
            if (err && i < errorIndex) {
                errorIndex = i;
                error = err;
            }
            if (++finished == count) allDone.notify_all();
        }
    }
};

}  // namespace

ThreadPool::ThreadPool(int threads) {
    if (threads <= 0) threads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
    workers_.reserve(threads - 1);
    for (int i = 1; i < threads; ++i) workers_.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : workers_) t.join();
}

void ThreadPool::workerLoop() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;  // stopping, and nothing left to run
            task = std::move(queue_.front());
            queue_.pop_front();
        }
        task();
    }
}

void ThreadPool::parallelFor(std::size_t count, const std::function<void(std::size_t)>& fn) {
    if (count == 0) return;
    if (count == 1 || workers_.empty()) {
        for (std::size_t i = 0; i < count; ++i) fn(i);  // exceptions propagate directly
        return;
    }

    auto job = std::make_shared<Job>(count, fn);
    const std::size_t helpers = std::min(workers_.size(), count - 1);
    {
        std::lock_guard lock(mutex_);
        for (std::size_t h = 0; h < helpers; ++h) queue_.emplace_back([job] { job->work(); });
    }
    if (helpers == 1) wake_.notify_one();
    else wake_.notify_all();

    job->work();

    std::exception_ptr error;
    {
        std::unique_lock lock(job->mutex);
        job->allDone.wait(lock, [&] { return job->finished == count; });
        // Take the exception out of the job: a late helper may hold the last
        // reference to the job and destroy it on its own thread.
        error = std::move(job->error);
    }
    if (error) std::rethrow_exception(error);
}

}  // namespace comp
