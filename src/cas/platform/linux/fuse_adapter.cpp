#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "daemon.h"
#include "platform.h"
#include "branch_context.h"
#include "commit.h"
#include "tree_serialize.h"
#include "fast_control.h"
#include "fuse_read_buffer.h"
#include "hash.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

namespace cas {

static Daemon* get_daemon() {
    return static_cast<Daemon*>(fuse_get_context()->private_data);
}

// Resolve the branch for a path-based FUSE op from the caller's PID. The
// returned shared_ptr keeps the BranchContext alive for the op's duration
// even if delete_branch races.
static std::shared_ptr<BranchContext> resolve_branch(Daemon* d) {
    // FUSE delivers the caller's PID as a pid_t. cas::Pid is int32_t and
    // wide enough to hold every value pid_t represents on Linux/macOS.
    return d->branch_for_pid(static_cast<Pid>(fuse_get_context()->pid));
}

// For FH-based ops: use the handle's pinned branch_id, not the caller's.
static std::shared_ptr<BranchContext> branch_for_fh(
    Daemon* d,
    const std::shared_ptr<FhState>& s) {
    auto br = d->branch(s->branch_id);
    return br ? br : d->main_branch();
}

static int errno_to_err() {
    int e = errno ? errno : EIO;
    return -e;
}

static bool is_fast_control_path(const char* path) {
    return path && std::strcmp(path, AGENTVFS_FAST_CONTROL_PATH) == 0;
}

static bool is_fast_control_descendant(const char* path) {
    constexpr size_t kControlLen = sizeof(AGENTVFS_FAST_CONTROL_PATH) - 1;
    return path &&
           std::strncmp(path, AGENTVFS_FAST_CONTROL_PATH, kControlLen) == 0 &&
           path[kControlLen] == '/';
}

static bool touches_fast_control_path(const char* path) {
    return is_fast_control_path(path) || is_fast_control_descendant(path);
}

static bool touches_fast_control_path(const char* a, const char* b) {
    return touches_fast_control_path(a) || touches_fast_control_path(b);
}

static int fast_control_mutation_error(const char* path) {
    return is_fast_control_descendant(path) ? -ENOTDIR : -EACCES;
}

static void fill_fast_control_attr(struct stat* st) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0600;
    st->st_nlink = 1;
    st->st_uid = fuse_get_context()->uid;
    st->st_gid = fuse_get_context()->gid;
    st->st_size = 0;
}

static size_t bounded_strlen(const char* s, size_t max) {
    size_t n = 0;
    while (n < max && s[n] != '\0') n++;
    return n;
}

static int cas_getattr(const char* path, struct stat* st, struct fuse_file_info* fi) {
    if (is_fast_control_path(path)) {
        fill_fast_control_attr(st);
        return 0;
    }
    if (is_fast_control_descendant(path)) return -ENOTDIR;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    std::memset(st, 0, sizeof(*st));

    if (std::strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        st->st_uid = fuse_get_context()->uid;
        st->st_gid = fuse_get_context()->gid;
        return 0;
    }

    // For an fh-based getattr (e.g. fstat), resolve the branch from the handle
    // — not the caller's current cgroup — to honor file-handle branch pinning.
    // Do the working-tree lookup AND the handle-size access under the branch's
    // checkpoint_mu so they observe a consistent snapshot relative to
    // checkpoint/rollback.
    std::shared_ptr<FhState> s;
    std::shared_ptr<BranchContext> br;
    if (fi) {
        s = d->get_fh(fi->fh);
        br = s ? branch_for_fh(d, s) : resolve_branch(d);
    } else {
        br = resolve_branch(d);
    }

    std::unique_lock<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (s && s->stale) return -ESTALE;
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;

    switch (entry->kind) {
        case EntryKind::Blob:    st->st_mode = S_IFREG | (entry->mode & 07777); break;
        case EntryKind::Tree:    st->st_mode = S_IFDIR | (entry->mode & 07777); st->st_nlink = 2; st->st_uid = fuse_get_context()->uid; st->st_gid = fuse_get_context()->gid; return 0;
        case EntryKind::Symlink: st->st_mode = S_IFLNK | 0777; break;
        default: return -ENOENT;
    }

    if (s && s->write_buf) {
        // Live handle: derive size from base_size + write-buffer overlay without
        // materializing the base payload (ensure_base_cache deliberately NOT
        // called — base_size is already populated at open time). The lock
        // above guards the read against concurrent checkpoint flushes.
        st->st_size = static_cast<off_t>(s->write_buf->effective_size(s->base_size));
    } else if (fi) {
        // fi was non-null but the handle was stale/absent: fall back to the
        // committed entry's blob size (header-validated on first sight, then
        // served from the immutable-size cache), so a corrupt blob still
        // surfaces EIO instead of size 0.
        if (entry->hash != ZERO_HASH) {
            uint64_t payload_size = 0;
            int err = d->store().blob_payload_size(entry->hash, payload_size);
            if (err != 0) return -err;
            st->st_size = static_cast<off_t>(payload_size);
        }
    } else {
        // Path-only GETATTR (e.g. some stat() paths). Blob sizes are
        // immutable per content hash, so a warm stat performs no object
        // open — and no disk I/O under checkpoint_mu; corrupt blobs report
        // EIO. Then consult any open dirty fh for the path
        // (read-your-writes, correctness contract 5).
        if (entry->hash != ZERO_HASH) {
            uint64_t payload_size = 0;
            int err = d->store().blob_payload_size(entry->hash, payload_size);
            if (err != 0) return -err;
            st->st_size = static_cast<off_t>(payload_size);
        }
        int64_t eff = d->dirty_fh_size_for_path(path, br->branch_id);
        if (eff >= 0) st->st_size = static_cast<off_t>(eff);
    }

    st->st_nlink = 1;
    st->st_uid = fuse_get_context()->uid;
    st->st_gid = fuse_get_context()->gid;
    return 0;
}

static int cas_open(const char* path, struct fuse_file_info* fi) {
    if (is_fast_control_path(path)) {
        int accmode = fi->flags & O_ACCMODE;
        if (accmode != O_RDONLY) return -EACCES;
        if (fi->flags & (O_TRUNC | O_APPEND)) return -EACCES;
        fi->direct_io = 1;
        return 0;
    }
    if (is_fast_control_descendant(path)) return -ENOTDIR;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    auto br = resolve_branch(d);
    {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        if (br->retired) return -ESTALE;
        auto entry = br->wt.lookup(path);
        if (!entry) return -ENOENT;
        if (entry->kind != EntryKind::Blob) return -EISDIR;

        // O_TRUNC via open() — FUSE delivers it atomically here by default
        // (FUSE_CAP_ATOMIC_O_TRUNC). Replace the blob with an empty one before
        // initializing FhState so the first write doesn't overlay onto the old
        // content's tail.
        Hash base_blob = entry->hash;
        uint64_t base_size = 0;
        bool truncated = (fi->flags & O_TRUNC) != 0;
        BlobView view;
        if (truncated) {
            base_blob = d->store().write_blob(nullptr, 0);
            br->wt.insert(path, {EntryKind::Blob, base_blob, entry->mode});
        } else if (base_blob != ZERO_HASH) {
            int blob_error = d->store().open_blob(base_blob, view);
            if (blob_error != 0) return -blob_error;
            base_size = view.payload_size();
        }

        auto state = std::make_unique<FhState>();
        state->path = path;
        state->branch_id = br->branch_id;
        d->refs().read_ref(br->name, state->pinned_commit);
        state->base_blob = base_blob;
        state->base_size = base_size;
        if (truncated) state->base_cache_loaded = true;  // known empty
        state->write_buf = std::make_unique<WriteBuffer>(base_blob, base_size);
        // Retain the fd-backed view only for read-capable, non-truncating
        // opens. Create/O_TRUNC handles stay ineligible (fd_read_eligible
        // remains false); the default BlobView is already invalid.
        // Eligibility also requires a valid view: a ZERO_HASH entry (failed
        // write_blob) has no object to open, and its reads must fall through
        // to the overlay path, which serves it as an empty file — matching
        // getattr — rather than EIO from an invalid fd.
        int accmode = fi->flags & O_ACCMODE;
        state->fd_read_eligible =
            !truncated && accmode != O_WRONLY && static_cast<bool>(view);
        if (state->fd_read_eligible) state->blob_view = std::move(view);
        fi->fh = d->allocate_fh(std::move(state));
    }

    // Bypass kernel page cache so reads always go through our handler —
    // required for read-your-writes and for observing ESTALE after rollback.
    fi->direct_io = 1;
    return 0;
}

static int cas_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;

    auto br = resolve_branch(d);
    {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        if (br->retired) return -ESTALE;
        Hash empty = d->store().write_blob(nullptr, 0);
        br->wt.insert(path, {EntryKind::Blob, empty, (uint32_t)(mode & 07777) | 0100000});

        auto state = std::make_unique<FhState>();
        state->path = path;
        state->branch_id = br->branch_id;
        d->refs().read_ref(br->name, state->pinned_commit);
        state->base_blob = empty;
        state->base_size = 0;
        state->base_cache_loaded = true;
        state->write_buf = std::make_unique<WriteBuffer>(empty, 0);
        fi->fh = d->allocate_fh(std::move(state));
    }

    // Bypass kernel page cache — see cas_open.
    fi->direct_io = 1;
    return 0;
}

static int cas_read(const char*, char* buf, size_t size, off_t offset,
                     struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    // Serialize against the branch's checkpoint flush, concurrent
    // write-buffer mutations, and stale-handle invalidation.
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (s->stale) return -ESTALE;
    d->ensure_base_cache(*s);
    size_t n = s->write_buf->read((uint64_t)offset, (uint8_t*)buf, size, s->base_cache);
    return (int)n;
}

static int cas_write(const char*, const char* buf, size_t size, off_t offset,
                      struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (s->stale) return -ESTALE;
    // First mutation on this handle: once it mutates it must never again serve
    // reads from the immutable blob fd (which would miss in-flight writes).
    s->fd_read_eligible = false;
    s->write_buf->write((uint64_t)offset, (const uint8_t*)buf, size);
    return (int)size;
}

static int cas_flush(const char*, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    // Serialize against the branch's checkpoint flush, concurrent
    // write-buffer mutations, and stale-handle invalidation.
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (s->stale) return -ESTALE;
    if (!s->write_buf || !s->write_buf->is_dirty()) return 0;

    d->ensure_base_cache(*s);
    auto data = s->write_buf->materialize(s->base_cache);
    Hash new_blob = d->store().write_blob(data);
    // Fix I5: refuse to corrupt the working tree on write_blob failure.
    if (new_blob == ZERO_HASH) return -EIO;

    auto existing = br->wt.lookup(s->path);
    uint32_t mode = existing ? existing->mode : 0100644;
    br->wt.insert(s->path, {EntryKind::Blob, new_blob, mode});

    s->base_blob = new_blob;
    s->base_size = data.size();
    s->base_cache = std::move(data);
    s->base_cache_loaded = true;
    s->write_buf->clear();
    return 0;
}

static int cas_fsync(const char* path, int /*datasync*/, struct fuse_file_info* fi) {
    return cas_flush(path, fi);
}

static int cas_release(const char* path, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    (void)cas_flush(path, fi);
    d->release_fh(fi->fh);
    fi->fh = 0;
    return 0;
}

static int cas_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                        off_t, struct fuse_file_info*, enum fuse_readdir_flags) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    if (std::strcmp(path, "/") != 0) {
        auto e = br->wt.lookup(path);
        if (!e || e->kind != EntryKind::Tree) return -ENOENT;
    }
    filler(buf, ".",  nullptr, 0, (fuse_fill_dir_flags)0);
    filler(buf, "..", nullptr, 0, (fuse_fill_dir_flags)0);
    for (auto& [child_path, entry] : br->wt.list_dir(path)) {
        std::string base = child_path;
        auto slash = base.rfind('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base == ".agentvfs-store") continue;
        if (base == ".agentvfs-control") continue;
        filler(buf, base.c_str(), nullptr, 0, (fuse_fill_dir_flags)0);
    }
    return 0;
}

static int cas_truncate(const char* path, off_t length, struct fuse_file_info* fi) {
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    if (fi) {
        auto s = d->get_fh(fi->fh);
        if (!s) return -EBADF;
        auto br = branch_for_fh(d, s);
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        if (br->retired) return -ESTALE;
        if (s->stale) return -ESTALE;
        // Same mutation rule as cas_write: truncate forfeits fd-backed reads.
        s->fd_read_eligible = false;
        s->write_buf->truncate((uint64_t)length);
        return 0;
    }

    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    if (entry->kind != EntryKind::Blob) return -EISDIR;

    std::vector<uint8_t> blob;
    if (entry->hash != ZERO_HASH) d->store().read_blob(entry->hash, blob);
    blob.resize((size_t)length, 0);
    Hash new_blob = d->store().write_blob(blob);
    br->wt.insert(path, {EntryKind::Blob, new_blob, entry->mode});
    return 0;
}

static int cas_access(const char* path, int mask) {
    if (is_fast_control_path(path)) {
        if (mask & W_OK) return -EACCES;
        if (mask & X_OK) return -EACCES;
        return 0;
    }
    if (is_fast_control_descendant(path)) return -ENOTDIR;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (std::strcmp(path, "/") == 0) return 0;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    if (mask == F_OK) return 0;

    // getattr always sets st_uid/st_gid to the caller's own, so the
    // caller is always the "owner" of the file in this FS's semantics.
    // Check owner permission bits (0400/0200/0100), not other bits.
    uint32_t m = entry->mode;
    if ((mask & R_OK) && !(m & 0400)) return -EACCES;
    if ((mask & W_OK) && !(m & 0200)) return -EACCES;
    if ((mask & X_OK) && !(m & 0100)) return -EACCES;
    return 0;
}

static int cas_statfs(const char* path, struct statvfs* stat) {
    (void)path;
    Daemon* d = get_daemon();
    if (statvfs(d->store_root().c_str(), stat) != 0) return errno_to_err();
    return 0;
}

static int cas_unlink(const char* path) {
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    auto e = br->wt.lookup(path);
    if (!e) return -ENOENT;
    if (e->kind == EntryKind::Tree) return -EISDIR;
    br->wt.remove(path);
    return 0;
}

static int cas_rmdir(const char* path) {
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    auto e = br->wt.lookup(path);
    if (!e) return -ENOENT;
    if (e->kind != EntryKind::Tree) return -ENOTDIR;
    auto children = br->wt.list_dir(path);
    if (!children.empty()) return -ENOTEMPTY;
    br->wt.remove(path);
    return 0;
}

static int cas_mkdir(const char* path, mode_t mode) {
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (br->wt.lookup(path)) return -EEXIST;
    br->wt.insert(path, {EntryKind::Tree, ZERO_HASH, (uint32_t)(mode & 07777) | 040000});
    return 0;
}

static int cas_rename(const char* from, const char* to, unsigned int flags) {
    (void)flags;
    if (touches_fast_control_path(from, to)) return -EACCES;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(from) || Daemon::is_hidden(to)) return -EACCES;
    if (auto* bs = d->bootstrap()) { bs->ensure_path(from); bs->ensure_path(to); }
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    auto e = br->wt.lookup(from);
    if (!e) return -ENOENT;

    if (e->kind == EntryKind::Tree) {
        br->wt.rename_dir(from, to);
    } else {
        br->wt.rename_entry(from, to);
    }
    return 0;
}

static int cas_symlink(const char* target, const char* linkpath) {
    if (touches_fast_control_path(linkpath)) return fast_control_mutation_error(linkpath);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(linkpath)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(linkpath);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (br->wt.lookup(linkpath)) return -EEXIST;
    size_t tlen = std::strlen(target);
    Hash link_blob = d->store().write_blob((const uint8_t*)target, tlen);
    br->wt.insert(linkpath, {EntryKind::Symlink, link_blob, 0120777});
    return 0;
}

static int cas_readlink(const char* path, char* buf, size_t size) {
    if (is_fast_control_path(path)) return -EINVAL;
    if (is_fast_control_descendant(path)) return -ENOTDIR;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    auto e = br->wt.lookup(path);
    if (!e) return -ENOENT;
    if (e->kind != EntryKind::Symlink) return -EINVAL;
    std::vector<uint8_t> blob;
    if (!d->store().read_blob(e->hash, blob)) return -EIO;
    size_t n = std::min(size - 1, blob.size());
    std::memcpy(buf, blob.data(), n);
    buf[n] = '\0';
    return 0;
}

static int cas_utimens(const char* path, const struct timespec tv[2],
                        struct fuse_file_info* fi) {
    (void)tv;
    (void)fi;
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    return 0;
}

static int cas_chmod(const char* path, mode_t mode, struct fuse_file_info* fi) {
    (void)fi;
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    entry->mode = (entry->mode & S_IFMT) | (mode & 07777);
    br->wt.insert(path, *entry);
    return 0;
}

static int cas_chown(const char* path, uid_t uid, gid_t gid,
                      struct fuse_file_info* fi) {
    (void)uid;
    (void)gid;
    (void)fi;
    if (touches_fast_control_path(path)) return fast_control_mutation_error(path);
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    return 0;
}

static int cas_ioctl(const char* path, int cmd, void* arg,
                     struct fuse_file_info* fi, unsigned int flags, void* data) {
    (void)arg;
    (void)fi;
    (void)flags;
    if (!is_fast_control_path(path)) return -ENOTTY;
    if (static_cast<unsigned int>(cmd) != AGENTVFS_IOC_CHECKPOINT) return -ENOTTY;

    auto* req = reinterpret_cast<agentvfs_checkpoint_ioc*>(data);
    if (!req) return -EINVAL;
    if (req->version != AGENTVFS_FAST_CONTROL_VERSION) {
        req->result_errno = EINVAL;
        std::snprintf(req->error, sizeof(req->error), "unsupported fast control version");
        return 0;
    }
    if (req->flags != 0) {
        req->result_errno = EINVAL;
        std::snprintf(req->error, sizeof(req->error), "unsupported flags");
        return 0;
    }
    size_t label_len = bounded_strlen(req->label, sizeof(req->label));
    if (label_len == sizeof(req->label)) {
        req->result_errno = EINVAL;
        std::snprintf(req->error, sizeof(req->error), "checkpoint label is not nul-terminated");
        return 0;
    }

    Daemon* d = get_daemon();
    auto br = resolve_branch(d);
    auto r = d->checkpoint_branch(br, std::string(req->label, label_len));
    if (!r.ok) {
        req->result_errno = EIO;
        std::snprintf(req->error, sizeof(req->error), "%s", r.error.c_str());
        return 0;
    }

    std::snprintf(req->commit_hex, sizeof(req->commit_hex), "%s",
                  hash_to_hex(r.commit_hash).c_str());
    req->result_errno = 0;
    req->error[0] = '\0';
    return 0;
}

// Memory-backed read fallback for mutated handles (those that lost
// fd_read_eligibility). Allocates a fuse_bufvec whose single buffer is a
// malloc'd region filled via write_buf->read (the same overlay logic cas_read
// uses). libfuse copies `count` bytes out of the `size`-byte allocation.
static int make_overlay_read_buf(Daemon* d, const std::shared_ptr<FhState>& s,
                                 size_t size, off_t offset,
                                 struct fuse_bufvec** out) {
    if (!out) return -EINVAL;
    *out = nullptr;
    if (offset < 0) return -EINVAL;
    auto* result = static_cast<struct fuse_bufvec*>(
        std::malloc(sizeof(struct fuse_bufvec)));
    if (!result) return -ENOMEM;
    void* memory = size ? std::malloc(size) : nullptr;
    if (size && !memory) {
        std::free(result);
        return -ENOMEM;
    }
    d->ensure_base_cache(*s);
    size_t count = s->write_buf->read(
        static_cast<uint64_t>(offset), static_cast<uint8_t*>(memory),
        size, s->base_cache);
    std::memset(result, 0, sizeof(*result));
    result->count = 1;
    result->buf[0].size = count;
    result->buf[0].fd = -1;
    result->buf[0].mem = memory;
    *out = result;
    return 0;
}

// FUSE read_buf op: serves zero-copy (fd-backed) reads for clean handles and
// falls back to the memory-backed overlay for handles that have been written
// or truncated. fd_read_eligible flips to false on the first mutation, so once
// a handle mutates it always reads through the overlay (which honors the
// post-write content, never the stale immutable blob fd).
//
// fd lifetime invariant: the fd placed in the returned bufvec is consumed by
// libfuse (spliced into the read reply) AFTER this callback returns — outside
// checkpoint_mu and after the local FhState shared_ptr is dropped. That is
// safe only because the kernel holds the file reference for the duration of
// read(2), so it cannot dispatch RELEASE for this fh while a READ is in
// flight, and Daemon::release_fh is the only place the FhState (and with it
// the retained blob_view fd) is dropped. Rollback marks handles stale but
// never closes their fds. Never add a daemon-side path that closes or
// replaces blob_view outside FhState destruction at release.
static int cas_read_buf(const char*, struct fuse_bufvec** out, size_t size,
                        off_t offset, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->retired) return -ESTALE;
    if (s->stale) return -ESTALE;
    if (offset < 0) return -EINVAL;
    if (s->fd_read_eligible)
        return make_fd_read_buf(s->blob_view, size, offset, out);
    return make_overlay_read_buf(d, s, size, offset, out);
}

static void fill_fuse_ops(struct fuse_operations& ops) {
    ops.getattr   = cas_getattr;
    ops.open      = cas_open;
    ops.create    = cas_create;
    ops.read      = cas_read;
    ops.read_buf  = cas_read_buf;
    ops.write     = cas_write;
    ops.flush     = cas_flush;
    ops.fsync     = cas_fsync;
    ops.release   = cas_release;
    ops.readdir   = cas_readdir;
    ops.truncate  = cas_truncate;
    ops.access    = cas_access;
    ops.statfs    = cas_statfs;
    ops.unlink    = cas_unlink;
    ops.rmdir     = cas_rmdir;
    ops.mkdir     = cas_mkdir;
    ops.rename    = cas_rename;
    ops.symlink   = cas_symlink;
    ops.readlink  = cas_readlink;
    ops.utimens   = cas_utimens;
    ops.chmod     = cas_chmod;
    ops.chown     = cas_chown;
    ops.ioctl     = cas_ioctl;
}

int run_filesystem(Daemon& daemon, const MountOptions& opts) {
    // Every read-capable, non-truncating open handle now retains an object
    // fd (BlobView) until release, so the per-daemon fd budget can grow with
    // the number of concurrent readers. Raise RLIMIT_NOFILE to its hard
    // ceiling at startup so large fan-out workloads don't hit EMFILE.
    struct rlimit nofile {};
    if (getrlimit(RLIMIT_NOFILE, &nofile) == 0 &&
        nofile.rlim_cur < nofile.rlim_max) {
        nofile.rlim_cur = nofile.rlim_max;
        (void)setrlimit(RLIMIT_NOFILE, &nofile);
    }

    struct fuse_operations ops {};
    fill_fuse_ops(ops);

    // libfuse takes a non-const argv. Use mutable static arrays for the
    // option flags rather than const_cast'ing string literals — fuse_main
    // doesn't modify them today, but the const_cast on a literal is UB if
    // it ever did, while a static char[] is a real writable buffer.
    static char prog_name[] = "agentvfs";
    static char fg_flag[]   = "-f";
    static char st_flag[]   = "-s";
    // attr_timeout / entry_timeout / negative_timeout = 0 are critical (same
    // as the macOS backend): the kernel dentry/attr cache is shared across
    // ALL processes, while we serve per-branch views and mutate the tree via
    // rollback/merge. With libfuse's 1s defaults, a still-cached positive
    // dentry makes the kernel skip LOOKUP and send OPEN for a path the
    // working tree no longer has — O_CREAT then fails ENOENT instead of
    // reaching CREATE (observed as create-after-rollback failures).
    static char opt_flag[]  = "-o";
    static char cache_opts[] = "attr_timeout=0,entry_timeout=0,negative_timeout=0";

    std::vector<char*> fargv;
    fargv.push_back(prog_name);
    fargv.push_back(const_cast<char*>(opts.mountpoint.c_str()));
    if (opts.foreground) fargv.push_back(fg_flag);
    if (opts.single_threaded) fargv.push_back(st_flag);
    fargv.push_back(opt_flag);
    fargv.push_back(cache_opts);
    for (auto& s : opts.passthrough_args) fargv.push_back(const_cast<char*>(s.c_str()));

    return fuse_main((int)fargv.size(), fargv.data(), &ops, &daemon);
}

} // namespace cas
