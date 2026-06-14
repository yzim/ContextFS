#include "branch_router.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef __linux__
#include "posix_compat.h"
#endif

namespace cas {

BranchRouter::BranchRouter() = default;

BranchRouter::BranchRouter(Hooks hooks)
    : hooks_(std::move(hooks)) {}

uint64_t BranchRouter::cgroup_id(const std::string& path) const {
    return hooks_.cgroup_id ? hooks_.cgroup_id(path) : cgroup_id_from_path(path);
}

std::string BranchRouter::proc_cgroup(Pid pid) const {
    return hooks_.read_cgroup ? hooks_.read_cgroup(pid) : read_proc_cgroup(pid);
}

uint64_t BranchRouter::proc_starttime(Pid pid) const {
    return hooks_.read_starttime ? hooks_.read_starttime(pid) : read_proc_starttime(pid);
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

uint64_t BranchRouter::read_proc_starttime(Pid pid) {
#ifdef __linux__
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    // Format: "PID (comm) state ppid ... <21 more fields> starttime ..."
    // comm can contain spaces and parens, so scan for the LAST ')' and
    // parse fields after it. Field numbering from man 5 proc: 1=pid,
    // 2=comm, 3=state, so after ')' we're at field 3 onwards. We want
    // field 22 (starttime), i.e. the 20th whitespace-separated token
    // after the last ')'.
    const char* rparen = std::strrchr(buf, ')');
    if (!rparen) return 0;
    const char* p = rparen + 1;
    // Skip leading space
    while (*p == ' ') p++;
    // Skip fields 3..21 = 19 tokens, landing at field 22
    for (int i = 0; i < 19; i++) {
        while (*p && *p != ' ') p++;
        if (!*p) return 0;
        p++;
    }
    char* end = nullptr;
    unsigned long long st = std::strtoull(p, &end, 10);
    if (end == p) return 0;
    return (uint64_t)st;
#else
    (void)pid;
    return 0;
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
    // Invalidate pid cache entries that might have stale mappings
    generation_++;
    pid_cache_.clear();
    return true;
}

bool BranchRouter::unregister_cgroup(const std::string& cgroup_path) {
    uint64_t cg_id = cgroup_id(cgroup_path);
    if (!cg_id) return false;
    std::lock_guard<std::mutex> lk(mu_);
    bool erased = cgroup_branch_map_.erase(cg_id) > 0;
    if (erased) generation_++;
    pid_cache_.clear();
    return erased;
}

std::vector<uint64_t> BranchRouter::cgroup_path_ids(const std::string& cgroup_abs_path) const {
    std::vector<uint64_t> ids;
    std::string path = cgroup_abs_path;
    while (!path.empty()) {
        uint64_t id = cgroup_id(path);
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

void BranchRouter::invalidate_branch(uint32_t branch_id) {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = cgroup_branch_map_.begin(); it != cgroup_branch_map_.end();) {
        if (it->second == branch_id) it = cgroup_branch_map_.erase(it);
        else ++it;
    }
    generation_++;
    pid_cache_.clear();
}

uint32_t BranchRouter::resolve(Pid pid) {
    // Read starttime outside the lock — this is a /proc/<pid>/stat read
    // that can block. Only the cache lookup and write need the mutex.
    for (;;) {
        uint64_t now_start = proc_starttime(pid);
        uint64_t miss_generation = 0;

        {
            std::lock_guard<std::mutex> lk(mu_);
            auto it = pid_cache_.find(pid);
            if (it != pid_cache_.end() && it->second.starttime == now_start && now_start != 0) {
                return it->second.branch_id;
            }
            miss_generation = generation_;
        }

        // Cache miss: perform all syscalls outside the lock to avoid blocking
        // other threads during slow I/O (read /proc/<pid>/cgroup + stat()).
        std::string cg_rel = proc_cgroup(pid);
        std::string cg_abs;
        std::vector<uint64_t> cgroup_ids;
        if (!cg_rel.empty()) {
            cg_abs = "/sys/fs/cgroup" + cg_rel;
            cgroup_ids = cgroup_path_ids(cg_abs);
        }

        // Re-take the lock only for the map lookup and cache write.
        std::lock_guard<std::mutex> lk(mu_);
        auto cached = pid_cache_.find(pid);
        if (cached != pid_cache_.end() &&
            cached->second.starttime == now_start && now_start != 0) {
            return cached->second.branch_id;
        }
        if (generation_ != miss_generation) continue;

        uint32_t branch_id = 0;
        if (!cgroup_ids.empty()) branch_id = branch_for_cgroup_ids(cgroup_ids);
        pid_cache_[pid] = {branch_id, now_start};
        return branch_id;
    }
}

bool BranchRouter::has_cgroup_for_branch(uint32_t branch_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [_, bid] : cgroup_branch_map_)
        if (bid == branch_id) return true;
    return false;
}

} // namespace cas
