#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace angelcopy {

// Bounded-by-nothing batch queue between a single walking producer and a
// worker pool: the pool is already working on earlier directories while later
// ones are still being enumerated (streaming — a collect-then-execute phase
// left the dialog at 0% for an entire 500k-file walk once; see CLAUDE.md).
// One mechanism, three uses: the copy walk (Item), the scan classification
// (ScanFileItem) and the deleter (DelItem). Producers keep batches
// single-directory and split them at ~256 files — that split is the safety
// net that stops one flat directory from pinning a single worker.
template <typename T>
struct BatchQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<T>> q;
    bool closed = false;

    void Push(std::vector<T>&& batch) {
        {
            std::lock_guard<std::mutex> l(m);
            q.push_back(std::move(batch));
        }
        cv.notify_one();
    }
    bool Pop(std::vector<T>& out) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return closed || !q.empty(); });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        return true;
    }
    void Close() {
        {
            std::lock_guard<std::mutex> l(m);
            closed = true;
        }
        cv.notify_all();
    }
};

} // namespace angelcopy
