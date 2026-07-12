#include "cgroup_watch.h"

#ifdef __linux__
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>
#endif

namespace cas {

CgroupWatch::CgroupWatch(DeleteFn on_delete)
    : on_delete_(std::move(on_delete)) {}

CgroupWatch::~CgroupWatch() { stop(); }

bool CgroupWatch::split_path(const std::string& dir, std::string* parent,
                             std::string* name) {
    std::string trimmed = dir;
    while (trimmed.size() > 1 && trimmed.back() == '/')
        trimmed.pop_back();
    std::string::size_type slash = trimmed.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= trimmed.size())
        return false;
    *parent = slash == 0 ? "/" : trimmed.substr(0, slash);
    *name = trimmed.substr(slash + 1);
    return true;
}

#ifdef __linux__

bool CgroupWatch::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_) return true;
    inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0) return false;
    wake_fd_ = eventfd(0, EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        close(inotify_fd_);
        inotify_fd_ = -1;
        return false;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

bool CgroupWatch::active() const {
    std::lock_guard<std::mutex> lk(mu_);
    return running_;
}

bool CgroupWatch::watch(const std::string& dir) {
    std::string parent, name;
    if (!split_path(dir, &parent, &name)) return false;
    // Capture the inode first: by the time the delete event is handled
    // the directory is gone, so this is the only chance to learn which
    // registration to evict. The caller invokes watch() before
    // register_cgroup, so a deletion after this stat fires the parent's
    // IN_DELETE with the watch already in place.
    struct stat st;
    if (stat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    std::lock_guard<std::mutex> lk(mu_);
    if (!running_) return false;
    // IN_MOVED_FROM is defensive only: cgroup v2 rejects rename(2), but
    // on other filesystems (unit tests) a rename also removes the entry
    // at the watched name. Re-adding an already-watched parent returns
    // the same wd with the same mask.
    int wd = inotify_add_watch(inotify_fd_, parent.c_str(),
                               IN_DELETE | IN_MOVED_FROM | IN_ONLYDIR);
    if (wd < 0) return false;
    Parent& entry = by_wd_[wd];
    entry.dir = parent;
    entry.children[name] = Child{dir, (uint64_t)st.st_ino};
    wd_by_parent_[parent] = wd;
    return true;
}

void CgroupWatch::unwatch(const std::string& dir) {
    std::string parent, name;
    if (!split_path(dir, &parent, &name)) return;
    std::lock_guard<std::mutex> lk(mu_);
    auto pit = wd_by_parent_.find(parent);
    if (pit == wd_by_parent_.end()) return;
    int wd = pit->second;
    auto wit = by_wd_.find(wd);
    if (wit == by_wd_.end()) return;
    wit->second.children.erase(name);
    if (wit->second.children.empty()) {
        // Erase the maps before removing the watch: the IN_IGNORED this
        // queues must not read as a parent teardown in the event thread.
        by_wd_.erase(wit);
        wd_by_parent_.erase(pit);
        if (running_) inotify_rm_watch(inotify_fd_, wd);
    }
}

void CgroupWatch::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) return;
        running_ = false;
        uint64_t one = 1;
        (void)!write(wake_fd_, &one, sizeof(one));
    }
    thread_.join();
    std::lock_guard<std::mutex> lk(mu_);
    close(inotify_fd_);
    close(wake_fd_);
    inotify_fd_ = -1;
    wake_fd_ = -1;
    by_wd_.clear();
    wd_by_parent_.clear();
}

void CgroupWatch::run() {
    for (;;) {
        struct pollfd fds[2] = {
            {inotify_fd_, POLLIN, 0},
            {wake_fd_, POLLIN, 0},
        };
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (fds[1].revents != 0) return;  // stop() woke us
        // Aligned per inotify(7); 4 KiB holds many back-to-back events.
        alignas(struct inotify_event) char buf[4096];
        ssize_t n = read(inotify_fd_, buf, sizeof(buf));
        if (n <= 0) continue;
        for (ssize_t off = 0;
             off + (ssize_t)sizeof(struct inotify_event) <= n;) {
            const auto* ev =
                reinterpret_cast<const struct inotify_event*>(buf + off);
            off += (ssize_t)sizeof(struct inotify_event) + ev->len;
            if (ev->mask & (IN_DELETE | IN_MOVED_FROM)) {
                // Name-bearing removal under a watched parent. Unrelated
                // names under the same parent simply miss the map.
                if (ev->len == 0) continue;
                std::string name(ev->name);
                Child child;
                bool tracked = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto wit = by_wd_.find(ev->wd);
                    if (wit == by_wd_.end()) continue;
                    auto cit = wit->second.children.find(name);
                    if (cit == wit->second.children.end()) continue;
                    child = cit->second;
                    tracked = true;
                    wit->second.children.erase(cit);
                    if (wit->second.children.empty()) {
                        wd_by_parent_.erase(wit->second.dir);
                        by_wd_.erase(wit);
                        inotify_rm_watch(inotify_fd_, ev->wd);
                    }
                }
                if (tracked && on_delete_) on_delete_(child.path, child.inode);
            } else if (ev->mask & (IN_IGNORED | IN_UNMOUNT)) {
                // The kernel dropped a parent watch we did not remove
                // (unmount, or the parent itself vanished). Losing the
                // watch silently would reopen the recreate hazard for
                // every child under it — evict them all instead.
                std::vector<Child> orphans;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    auto wit = by_wd_.find(ev->wd);
                    if (wit == by_wd_.end()) continue;
                    for (auto& [_, child] : wit->second.children)
                        orphans.push_back(std::move(child));
                    wd_by_parent_.erase(wit->second.dir);
                    by_wd_.erase(wit);
                }
                if (on_delete_)
                    for (const Child& child : orphans)
                        on_delete_(child.path, child.inode);
            }
        }
    }
}

#else  // !__linux__

bool CgroupWatch::start() { return false; }
bool CgroupWatch::active() const { return false; }
bool CgroupWatch::watch(const std::string&) { return false; }
void CgroupWatch::unwatch(const std::string&) {}
void CgroupWatch::stop() {}
void CgroupWatch::run() {}

#endif

} // namespace cas
