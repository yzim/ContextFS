#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace cas {

// Watches registered cgroup directories for external deletion so
// BranchRouter can evict the registration without paying a per-resolve
// leaf revalidation stat. Deletion is the only event of interest:
// control-plane changes go through register/unregister, and cgroup
// migration is covered by the fresh per-request membership read (plus
// the eBPF fence when active).
//
// Mechanism: an inotify watch on the PARENT directory matching the
// child's name on IN_DELETE. kernfs (cgroup v2) does not deliver
// IN_DELETE_SELF for rmdir of the watched directory itself — verified
// on kernel 6.17 — but vfs_rmdir raises IN_DELETE on the parent
// unconditionally, which the 2026-07-12 root probe confirmed cgroupfs
// delivers. Registered cgroups sharing a parent share one kernel watch.
//
// Portable no-op off Linux: start() returns false and the daemon keeps
// synchronous leaf revalidation.
class CgroupWatch {
public:
    // Reports the path and the inode captured at watch() time — the
    // directory no longer exists when the callback runs, so the inode
    // lets the router evict the exact registration. Invoked from the
    // event thread; the callback must be thread-safe.
    using DeleteFn = std::function<void(const std::string& path,
                                        uint64_t inode)>;

    explicit CgroupWatch(DeleteFn on_delete);
    ~CgroupWatch();

    CgroupWatch(const CgroupWatch&) = delete;
    CgroupWatch& operator=(const CgroupWatch&) = delete;

    // Spawns the event thread. False on non-Linux or inotify failure.
    bool start();
    bool active() const;

    // Adds a delete watch for `dir` (watches its parent). Callers
    // watch() BEFORE BranchRouter::register_cgroup so a deletion racing
    // registration still fires the eviction after the map insert. False
    // when not started, the path is not a directory, or
    // inotify_add_watch fails (watch-count limits) — the caller must
    // then fall back to per-resolve revalidation.
    bool watch(const std::string& dir);
    void unwatch(const std::string& dir);

    // Joins the event thread. Idempotent; the destructor calls it.
    void stop();

private:
    struct Child {
        std::string path;
        uint64_t inode = 0;
    };
    struct Parent {
        std::string dir;
        // Keyed by the child directory's basename, matched against
        // IN_DELETE event names.
        std::unordered_map<std::string, Child> children;
    };

    // "/sys/fs/cgroup/cg1" -> ("/sys/fs/cgroup", "cg1"). False for
    // paths without a parent ("/", "", no slash).
    static bool split_path(const std::string& dir, std::string* parent,
                           std::string* name);

    void run();

    DeleteFn on_delete_;
    mutable std::mutex mu_;
    std::unordered_map<int, Parent> by_wd_;
    std::unordered_map<std::string, int> wd_by_parent_;
    std::thread thread_;
    int inotify_fd_ = -1;
    int wake_fd_ = -1;
    bool running_ = false;
};

} // namespace cas
