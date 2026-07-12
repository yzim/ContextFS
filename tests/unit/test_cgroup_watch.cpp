#include "cgroup_watch.h"
#include <cstdio>
#include <cstdlib>

#ifdef __linux__

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

// Deletion callbacks arrive from the watch's event thread; the test
// synchronizes on this collector. tmpfs directories stand in for cgroup
// directories — the parent-watch inotify semantics are what is under
// test here, and the kernfs-specific delivery guarantee has its own
// root-gated system test (test_cgroup_watch_kernfs).
struct DeleteEvents {
    std::mutex mu;
    std::condition_variable cv;
    std::vector<std::pair<std::string, uint64_t>> events;

    void push(const std::string& path, uint64_t inode) {
        std::lock_guard<std::mutex> lk(mu);
        events.emplace_back(path, inode);
        cv.notify_all();
    }
    bool wait_for_count(size_t count, int timeout_ms) {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           [&] { return events.size() >= count; });
    }
    size_t count() {
        std::lock_guard<std::mutex> lk(mu);
        return events.size();
    }
};

int main() {
    char root[] = "/tmp/cas-cgroup-watch-XXXXXX";
    REQUIRE(mkdtemp(root) != nullptr);
    // a and b share a parent (one kernel watch internally); c lives in
    // a separate parent directory.
    std::string dir_a = std::string(root) + "/a";
    std::string dir_b = std::string(root) + "/b";
    std::string sub = std::string(root) + "/sub";
    std::string dir_c = sub + "/c";
    std::string dir_x = std::string(root) + "/x";
    REQUIRE(mkdir(dir_a.c_str(), 0700) == 0);
    REQUIRE(mkdir(dir_b.c_str(), 0700) == 0);
    REQUIRE(mkdir(sub.c_str(), 0700) == 0);
    REQUIRE(mkdir(dir_c.c_str(), 0700) == 0);
    struct stat st_a, st_c;
    REQUIRE(stat(dir_a.c_str(), &st_a) == 0);
    REQUIRE(stat(dir_c.c_str(), &st_c) == 0);

    DeleteEvents seen;
    CgroupWatch watch([&seen](const std::string& path, uint64_t inode) {
        seen.push(path, inode);
    });

    // Contract: watch() refuses before start() and for missing paths.
    REQUIRE(!watch.watch(dir_a));
    REQUIRE(!watch.active());
    REQUIRE(watch.start());
    REQUIRE(watch.active());
    REQUIRE(!watch.watch(std::string(root) + "/missing"));
    REQUIRE(!watch.watch("/"));

    REQUIRE(watch.watch(dir_a));
    REQUIRE(watch.watch(dir_b));
    REQUIRE(watch.watch(dir_c));

    // Deleting an UNWATCHED sibling under a watched parent must not
    // report (the parent watch sees it; the name misses the map).
    REQUIRE(mkdir(dir_x.c_str(), 0700) == 0);
    REQUIRE(rmdir(dir_x.c_str()) == 0);
    REQUIRE(!seen.wait_for_count(1, 300));

    // External deletion of a watched directory fires exactly one
    // callback carrying the path and the inode captured at watch time.
    REQUIRE(rmdir(dir_a.c_str()) == 0);
    REQUIRE(seen.wait_for_count(1, 5000));
    REQUIRE(seen.events[0].first == dir_a);
    REQUIRE(seen.events[0].second == (uint64_t)st_a.st_ino);

    // A watched directory in a different parent reports independently.
    REQUIRE(rmdir(dir_c.c_str()) == 0);
    REQUIRE(seen.wait_for_count(2, 5000));
    REQUIRE(seen.events[1].first == dir_c);
    REQUIRE(seen.events[1].second == (uint64_t)st_c.st_ino);

    // Explicit unwatch (control-plane unregister) suppresses the event:
    // the deletion that follows must not report.
    watch.unwatch(dir_b);
    REQUIRE(rmdir(dir_b.c_str()) == 0);
    REQUIRE(!seen.wait_for_count(3, 300));
    REQUIRE(seen.count() == 2);

    watch.stop();
    watch.stop();  // idempotent
    REQUIRE(!watch.active());

    REQUIRE(rmdir(sub.c_str()) == 0);
    REQUIRE(rmdir(root) == 0);
    std::printf("PASS test_cgroup_watch\n");
    return 0;
}

#else  // !__linux__

int main() {
    std::printf("SKIP test_cgroup_watch: Linux inotify only\n");
    return 0;
}

#endif
