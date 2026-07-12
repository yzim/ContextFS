// Root-only: verifies the kernel-behavior assumption behind CgroupWatch —
// that inotify IN_DELETE is delivered on the PARENT directory for cgroup
// v2 (kernfs) rmdir. (IN_DELETE_SELF on the removed directory itself is
// NOT delivered by kernfs — measured on kernel 6.17, 2026-07-12 — which
// is why CgroupWatch watches parents.) The tmpfs unit test proves the
// component; this test proves the filesystem. Exit codes: 0 PASS, 77
// environment cannot run the check (wrapper decides SKIP vs FAIL), 1 the
// kernel did not deliver the event — CgroupWatch must not replace
// per-resolve revalidation on such a kernel.
#include "cgroup_watch.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace cas;

int main() {
    std::string dir = "/sys/fs/cgroup/agentvfs-watch-test-" +
                      std::to_string(getpid());
    if (mkdir(dir.c_str(), 0755) != 0) {
        std::perror("mkdir cgroup");
        return 77;
    }
    struct stat st;
    if (stat(dir.c_str(), &st) != 0) {
        std::perror("stat cgroup");
        rmdir(dir.c_str());
        return 77;
    }

    std::mutex mu;
    std::condition_variable cv;
    bool fired = false;
    std::string seen_path;
    uint64_t seen_inode = 0;

    CgroupWatch watch([&](const std::string& path, uint64_t inode) {
        std::lock_guard<std::mutex> lk(mu);
        fired = true;
        seen_path = path;
        seen_inode = inode;
        cv.notify_all();
    });
    if (!watch.start()) {
        std::fprintf(stderr, "inotify unavailable\n");
        rmdir(dir.c_str());
        return 77;
    }
    if (!watch.watch(dir)) {
        std::fprintf(stderr, "inotify_add_watch failed on kernfs\n");
        rmdir(dir.c_str());
        return 1;
    }

    if (rmdir(dir.c_str()) != 0) {
        std::perror("rmdir cgroup");
        return 77;
    }

    std::unique_lock<std::mutex> lk(mu);
    if (!cv.wait_for(lk, std::chrono::seconds(5), [&] { return fired; })) {
        std::fprintf(stderr,
            "FAIL: parent IN_DELETE not delivered for kernfs rmdir\n");
        return 1;
    }
    if (seen_path != dir || seen_inode != (uint64_t)st.st_ino) {
        std::fprintf(stderr,
            "FAIL: event mismatch (path %s inode %llu, expected %s %llu)\n",
            seen_path.c_str(), (unsigned long long)seen_inode,
            dir.c_str(), (unsigned long long)st.st_ino);
        return 1;
    }
    lk.unlock();

    std::printf("PASS test_cgroup_watch_kernfs\n");
    return 0;
}
