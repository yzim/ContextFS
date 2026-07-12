#include "routing_fence.h"

#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

void release_worker(std::mutex& mutex, std::condition_variable& cv,
                    bool& should_exit, std::thread& worker) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        should_exit = true;
    }
    cv.notify_one();
    worker.join();
}

} // namespace

int main() {
    if (::geteuid() != 0) {
        std::cout << "SKIP cas_test_routing_fence_exit: needs root\n";
        return 0;
    }

    cas::RoutingFence fence;
    if (!fence.load_and_attach()) {
        std::cerr << "SKIP: routing fence did not load and attach\n";
        return 77;
    }

    std::mutex mutex;
    std::condition_variable cv;
    cas::Pid worker_tid = 0;
    bool should_exit = false;

    std::thread worker([&] {
        std::unique_lock<std::mutex> lock(mutex);
        worker_tid = static_cast<cas::Pid>(::gettid());
        cv.notify_one();
        cv.wait(lock, [&] { return should_exit; });
    });

    cas::Pid tracked_tid = 0;
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return worker_tid != 0; });
        tracked_tid = worker_tid;
    }

    if (tracked_tid == static_cast<cas::Pid>(::getpid())) {
        release_worker(mutex, cv, should_exit, worker);
        std::cerr << "FAIL: worker TID unexpectedly equals process leader PID\n";
        return 1;
    }
    if (!fence.track(tracked_tid)) {
        release_worker(mutex, cv, should_exit, worker);
        std::cerr << "FAIL: could not track worker TID " << tracked_tid << '\n';
        return 1;
    }

    const uint64_t before = fence.generation();
    release_worker(mutex, cv, should_exit, worker);
    const uint64_t after = fence.generation();

    if (after <= before) {
        std::cerr << "FAIL: non-leader TID exit did not advance routing fence "
                  << "generation (tid=" << tracked_tid << ", before=" << before
                  << ", after=" << after << ")\n";
        return 1;
    }

    // tracked_pids has exactly 4096 slots. Filling every slot with PID values
    // outside Linux's valid PID range proves the exiting worker's exact key
    // was deleted instead of merely causing an unrelated generation bump.
    constexpr uint32_t kTrackedPidCapacity = 4096;
    constexpr cas::Pid kSyntheticPidBase = 1'000'000'000;
    for (uint32_t i = 0; i < kTrackedPidCapacity; ++i) {
        const cas::Pid synthetic_pid =
            kSyntheticPidBase + static_cast<cas::Pid>(i);
        if (!fence.track(synthetic_pid)) {
            std::cerr << "FAIL: tracked TID key was not reclaimed; map filled at "
                      << i << " of " << kTrackedPidCapacity << " entries\n";
            return 1;
        }
    }

    std::cout << "PASS cas_test_routing_fence_exit: tid=" << tracked_tid
              << " generation=" << before << "->" << after
              << " reclaimed_capacity=" << kTrackedPidCapacity << '\n';
    return 0;
}
