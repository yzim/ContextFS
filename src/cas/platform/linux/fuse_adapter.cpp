#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include "daemon.h"
#include "platform.h"
#include "branch_context.h"
#include "commit.h"
#include "tree_serialize.h"
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
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

static int cas_getattr(const char* path, struct stat* st, struct fuse_file_info* fi) {
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

    auto br = resolve_branch(d);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;

    switch (entry->kind) {
        case EntryKind::Blob:    st->st_mode = S_IFREG | (entry->mode & 07777); break;
        case EntryKind::Tree:    st->st_mode = S_IFDIR | (entry->mode & 07777); st->st_nlink = 2; st->st_uid = fuse_get_context()->uid; st->st_gid = fuse_get_context()->gid; return 0;
        case EntryKind::Symlink: st->st_mode = S_IFLNK | 0777; break;
        default: return -ENOENT;
    }

    if (entry->kind == EntryKind::Blob) {
        std::vector<uint8_t> blob;
        if (d->store().read_blob(entry->hash, blob)) st->st_size = (off_t)blob.size();
    } else if (entry->kind == EntryKind::Symlink) {
        std::vector<uint8_t> blob;
        if (d->store().read_blob(entry->hash, blob)) st->st_size = (off_t)blob.size();
    }

    if (fi) {
        auto s = d->get_fh(fi->fh);
        if (s && s->write_buf) {
            d->ensure_base_cache(*s);
            st->st_size = (off_t)s->write_buf->effective_size(s->base_cache.size());
        }
    } else {
        // Kernel may call GETATTR without a fh (e.g. some fstat() paths).
        // Surface read-your-writes by consulting any open dirty fh for path.
        int64_t eff = d->dirty_fh_size_for_path(path, br->branch_id);
        if (eff >= 0) st->st_size = (off_t)eff;
    }

    st->st_nlink = 1;
    st->st_uid = fuse_get_context()->uid;
    st->st_gid = fuse_get_context()->gid;
    return 0;
}

static int cas_open(const char* path, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    auto br = resolve_branch(d);
    {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
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
        if (truncated) {
            base_blob = d->store().write_blob(nullptr, 0);
            br->wt.insert(path, {EntryKind::Blob, base_blob, entry->mode});
        } else {
            std::vector<uint8_t> blob;
            if (base_blob != ZERO_HASH && d->store().read_blob(base_blob, blob))
                base_size = blob.size();
        }

        auto state = std::make_unique<FhState>();
        state->path = path;
        state->branch_id = br->branch_id;
        d->refs().read_ref(br->name, state->pinned_commit);
        state->base_blob = base_blob;
        state->base_size = base_size;
        if (truncated) state->base_cache_loaded = true;  // known empty
        state->write_buf = std::make_unique<WriteBuffer>(base_blob, base_size);
        fi->fh = d->allocate_fh(std::move(state));
    }

    // Bypass kernel page cache so reads always go through our handler —
    // required for read-your-writes and for observing ESTALE after rollback.
    fi->direct_io = 1;
    return 0;
}

static int cas_create(const char* path, mode_t mode, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;

    auto br = resolve_branch(d);
    {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
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
    if (s->stale) return -ESTALE;
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
        filler(buf, base.c_str(), nullptr, 0, (fuse_fill_dir_flags)0);
    }
    return 0;
}

static int cas_truncate(const char* path, off_t length, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    if (fi) {
        auto s = d->get_fh(fi->fh);
        if (!s) return -EBADF;
        auto br = branch_for_fh(d, s);
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        if (s->stale) return -ESTALE;
        s->write_buf->truncate((uint64_t)length);
        return 0;
    }

    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
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
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (std::strcmp(path, "/") == 0) return 0;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    if (mask == F_OK) return 0;

    // Spec §FUSE callbacks: MVP checks only the "other" mode bits; owner
    // uid/gid tracking is deferred. Single-agent scope makes this safe.
    uint32_t m = entry->mode;
    if ((mask & R_OK) && !(m & 0004)) return -EACCES;
    if ((mask & W_OK) && !(m & 0002)) return -EACCES;
    if ((mask & X_OK) && !(m & 0001)) return -EACCES;
    return 0;
}

static int cas_statfs(const char* path, struct statvfs* stat) {
    (void)path;
    Daemon* d = get_daemon();
    if (statvfs(d->store_root().c_str(), stat) != 0) return errno_to_err();
    return 0;
}

static int cas_unlink(const char* path) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    auto e = br->wt.lookup(path);
    if (!e) return -ENOENT;
    if (e->kind == EntryKind::Tree) return -EISDIR;
    br->wt.remove(path);
    return 0;
}

static int cas_rmdir(const char* path) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    auto e = br->wt.lookup(path);
    if (!e) return -ENOENT;
    if (e->kind != EntryKind::Tree) return -ENOTDIR;
    auto children = br->wt.list_dir(path);
    if (!children.empty()) return -ENOTEMPTY;
    br->wt.remove(path);
    return 0;
}

static int cas_mkdir(const char* path, mode_t mode) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->wt.lookup(path)) return -EEXIST;
    br->wt.insert(path, {EntryKind::Tree, ZERO_HASH, (uint32_t)(mode & 07777) | 040000});
    return 0;
}

static int cas_rename(const char* from, const char* to, unsigned int flags) {
    (void)flags;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(from) || Daemon::is_hidden(to)) return -EACCES;
    if (auto* bs = d->bootstrap()) { bs->ensure_path(from); bs->ensure_path(to); }
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
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
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(linkpath)) return -EACCES;
    if (auto* bs = d->bootstrap()) bs->ensure_path(linkpath);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (br->wt.lookup(linkpath)) return -EEXIST;
    size_t tlen = std::strlen(target);
    Hash link_blob = d->store().write_blob((const uint8_t*)target, tlen);
    br->wt.insert(linkpath, {EntryKind::Symlink, link_blob, 0120777});
    return 0;
}

static int cas_readlink(const char* path, char* buf, size_t size) {
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

static void fill_fuse_ops(struct fuse_operations& ops) {
    ops.getattr   = cas_getattr;
    ops.open      = cas_open;
    ops.create    = cas_create;
    ops.read      = cas_read;
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
}

int run_filesystem(Daemon& daemon, const MountOptions& opts) {
    struct fuse_operations ops {};
    fill_fuse_ops(ops);

    // libfuse takes a non-const argv. Use mutable static arrays for the
    // option flags rather than const_cast'ing string literals — fuse_main
    // doesn't modify them today, but the const_cast on a literal is UB if
    // it ever did, while a static char[] is a real writable buffer.
    static char prog_name[] = "agentvfs";
    static char fg_flag[]   = "-f";
    static char st_flag[]   = "-s";

    std::vector<char*> fargv;
    fargv.push_back(prog_name);
    fargv.push_back(const_cast<char*>(opts.mountpoint.c_str()));
    if (opts.foreground) fargv.push_back(fg_flag);
    if (opts.single_threaded) fargv.push_back(st_flag);
    for (auto& s : opts.passthrough_args) fargv.push_back(const_cast<char*>(s.c_str()));

    return fuse_main((int)fargv.size(), fargv.data(), &ops, &daemon);
}

} // namespace cas
