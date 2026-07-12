#include "branch_router.h"
#include <cerrno>
#include <cstdio>
#include <vector>

#ifdef __linux__
#include "posix_compat.h"
#endif

namespace cas {

BranchRouter::BranchRouter() = default;

BranchRouter::BranchRouter(Hooks hooks)
    : hooks_(std::move(hooks)) {}

void BranchRouter::attach_fence(std::function<uint64_t()> generation,
                                 std::function<bool(Pid)> track) {
    hooks_.fence_generation = std::move(generation);
    hooks_.fence_track = std::move(track);
}

void BranchRouter::fence_cache_store(Pid pid, uint32_t branch_id,
                                      uint64_t gen) {
    // Caller holds mu_.
    if (pid_fence_cache_.size() >= kFenceCacheCap) pid_fence_cache_.clear();
    pid_fence_cache_[pid] = {branch_id, gen};
}

uint64_t BranchRouter::cgroup_id(const std::string& path) const {
    return hooks_.cgroup_id ? hooks_.cgroup_id(path) : cgroup_id_from_path(path);
}

std::string BranchRouter::proc_cgroup(Pid pid) const {
    return hooks_.read_cgroup ? hooks_.read_cgroup(pid) : read_proc_cgroup(pid);
}

uint64_t BranchRouter::cgroup_id_from_path(const std::string& path) {
    // cgroup v2 only meaningfully exists on Linux. The id is the cgroup
    // directory's inode — std::filesystem doesn't expose st_ino
    // portably, so we keep a POSIX stat call here behind the Linux gate
    // to preserve the exact id semantics the test suite expects.
#ifdef __linux__
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return 0;
    return (uint64_t)st.st_ino;
#else
    (void)path;
    return 0;
#endif
}

std::string BranchRouter::read_proc_cgroup(Pid pid) {
#ifdef __linux__
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/cgroup", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return {};
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return {};
    buf[n] = '\0';
    // cgroup v2 line: "0::<path>\n"
    const char* p = buf;
    while (*p) {
        if (p[0] == '0' && p[1] == ':' && p[2] == ':') {
            const char* start = p + 3;
            const char* end = start;
            while (*end && *end != '\n') end++;
            return std::string(start, end);
        }
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
    return {};
#else
    (void)pid;
    return {};
#endif
}

bool BranchRouter::register_cgroup(const std::string& cgroup_path,
                                    uint32_t branch_id) {
    uint64_t cg_id = cgroup_id(cgroup_path);
    if (!cg_id) {
        std::fprintf(stderr,
            "agentvfs: BranchRouter::register_cgroup: invalid cgroup directory '%s'\n",
            cgroup_path.c_str());
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    cgroup_branch_map_[cg_id] = branch_id;
    // Invalidate the path and fence caches; the registration set changed.
    generation_++;
    path_branch_cache_.clear();
    pid_fence_cache_.clear();
    return true;
}

bool BranchRouter::unregister_cgroup(const std::string& cgroup_path) {
    uint64_t cg_id = cgroup_id(cgroup_path);
    if (!cg_id) return false;
    std::lock_guard<std::mutex> lk(mu_);
    bool erased = cgroup_branch_map_.erase(cg_id) > 0;
    if (erased) generation_++;
    path_branch_cache_.clear();
    pid_fence_cache_.clear();
    return erased;
}

std::vector<uint64_t> BranchRouter::cgroup_path_ids(
        const std::string& cgroup_abs_path, uint64_t leaf_cgroup_id) const {
    std::vector<uint64_t> ids;
    std::string path = cgroup_abs_path;
    bool leaf = true;
    while (!path.empty()) {
        uint64_t id = leaf ? leaf_cgroup_id : cgroup_id(path);
        leaf = false;
        if (id != 0) ids.push_back(id);

        if (path == "/sys/fs/cgroup") break;
        std::string::size_type slash = path.find_last_of('/');
        if (slash == std::string::npos || slash <= sizeof("/sys/fs/cgroup") - 1) break;
        path.resize(slash);
    }
    return ids;
}

uint32_t BranchRouter::branch_for_cgroup_ids(const std::vector<uint64_t>& ids) const {
    for (uint64_t id : ids) {
        auto it = cgroup_branch_map_.find(id);
        if (it != cgroup_branch_map_.end()) return it->second;
    }
    return 0;
}

void BranchRouter::evict_cgroup_id(uint64_t cg_id) {
    std::lock_guard<std::mutex> lk(mu_);
    cgroup_branch_map_.erase(cg_id);
    generation_++;
    path_branch_cache_.clear();
    pid_fence_cache_.clear();
}

void BranchRouter::set_leaf_revalidation(bool on) {
    revalidate_leaf_.store(on, std::memory_order_relaxed);
}

void BranchRouter::invalidate_branch(uint32_t branch_id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = cgroup_branch_map_.begin(); it != cgroup_branch_map_.end();) {
        if (it->second == branch_id) it = cgroup_branch_map_.erase(it);
        else ++it;
    }
    generation_++;
    path_branch_cache_.clear();
    pid_fence_cache_.clear();
}

uint32_t BranchRouter::resolve(Pid pid) {
    bool fence_on = static_cast<bool>(hooks_.fence_generation);
    if (fence_on) {
        uint64_t gen = hooks_.fence_generation();
        std::lock_guard<std::mutex> lk(mu_);
        auto it = pid_fence_cache_.find(pid);
        if (it != pid_fence_cache_.end() && it->second.generation == gen)
            return it->second.branch_id;
    }

    // Track BEFORE sampling the generation and reading membership: once
    // tracked, any later migration or exit of this pid bumps the counter,
    // so an entry stored below can never satisfy the fast path after the
    // pid's routing changed.
    bool fence_cache = false;
    uint64_t fence_gen = 0;
    if (fence_on && hooks_.fence_track(pid)) {
        fence_cache = true;
        fence_gen = hooks_.fence_generation();
    }

    // Read current membership outside the lock — a procfs read that can
    // block. Reading fresh on every request is what makes routing strict
    // under cgroup migration: there is no per-pid state to go stale.
    std::string cg_rel = proc_cgroup(pid);
    if (cg_rel.empty()) return 0;

    std::string cg_abs = "/sys/fs/cgroup" + cg_rel;
    // With the delete watch active the daemon disables per-resolve leaf
    // revalidation: external rmdir of a registered cgroup arrives as
    // evict_cgroup_id() instead, and a warm resolve performs no stat.
    bool check_leaf = revalidate_leaf_.load(std::memory_order_relaxed);
    uint64_t leaf_cgroup_id = check_leaf ? cgroup_id(cg_abs) : 0;
    uint64_t miss_generation = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = path_branch_cache_.find(cg_rel);
        if (it != path_branch_cache_.end()
                && (!check_leaf
                    || it->second.leaf_cgroup_id == leaf_cgroup_id)) {
            if (fence_cache)
                fence_cache_store(pid, it->second.branch_id, fence_gen);
            return it->second.branch_id;
        }
        miss_generation = generation_;
    }

    // Memo miss: ancestor walk (one stat() per remaining level) outside the
    // lock. The leaf id is read here if the hit path skipped it — the walk
    // needs it, and storing it keeps entries revalidation-ready.
    if (!check_leaf) leaf_cgroup_id = cgroup_id(cg_abs);
    std::vector<uint64_t> ids = cgroup_path_ids(cg_abs, leaf_cgroup_id);

    std::lock_guard<std::mutex> lk(mu_);
    uint32_t branch_id = ids.empty() ? 0 : branch_for_cgroup_ids(ids);
    // Memoize (and fence-cache) only when no register/unregister raced
    // the walk — a racing resolve may serve the fresh answer, it just
    // must not cache across the change.
    if (generation_ == miss_generation) {
        if (path_branch_cache_.size() >= kPathCacheCap)
            path_branch_cache_.clear();
        path_branch_cache_.insert_or_assign(
            cg_rel, PathEntry{leaf_cgroup_id, branch_id});
        if (fence_cache) fence_cache_store(pid, branch_id, fence_gen);
    }
    return branch_id;
}

bool BranchRouter::has_cgroup_for_branch(uint32_t branch_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [_, bid] : cgroup_branch_map_)
        if (bid == branch_id) return true;
    return false;
}

} // namespace cas
