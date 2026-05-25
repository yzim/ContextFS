// libfuse 2.9 adapter for macOS, backed by fuse-t's libfuse.dylib.
// Behaves identically to src/cas/platform/linux/fuse_adapter.cpp; only
// the signature shape differs. An OperationsCore extraction (v2) is
// the right place to unify with the Linux adapter — for v1 we accept
// the duplication and converge on behavior, not signatures.
#define FUSE_USE_VERSION 29
#include <fuse.h>

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
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>  // F_OK / R_OK / W_OK / X_OK
#include <vector>

namespace cas {

static Daemon* get_daemon() {
    return static_cast<Daemon*>(fuse_get_context()->private_data);
}

static std::shared_ptr<BranchContext> resolve_branch(Daemon* d) {
    return d->branch_for_pid(static_cast<Pid>(fuse_get_context()->pid));
}

static std::shared_ptr<BranchContext> branch_for_fh(
    Daemon* d, const std::shared_ptr<FhState>& s) {
    auto br = d->branch(s->branch_id);
    return br ? br : d->main_branch();
}

static int errno_to_err() {
    int e = errno ? errno : EIO;
    return -e;
}

// Drop Finder/Spotlight artifacts at the FS layer. Writes are silently
// no-op'd; reads return ENOENT.
static bool is_appledouble(const char* path) {
    if (!path) return false;
    const char* base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    if (std::strcmp(base, ".DS_Store") == 0) return true;
    if (base[0] == '.' && base[1] == '_') return true;
    return false;
}

static int cas_getattr(const char* path, struct stat* st) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path) || is_appledouble(path)) return -ENOENT;
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
        case EntryKind::Blob:
            st->st_mode = S_IFREG | (entry->mode & 07777); break;
        case EntryKind::Tree:
            st->st_mode = S_IFDIR | (entry->mode & 07777);
            st->st_nlink = 2;
            st->st_uid = fuse_get_context()->uid;
            st->st_gid = fuse_get_context()->gid;
            return 0;
        case EntryKind::Symlink:
            st->st_mode = S_IFLNK | 0777; break;
        default: return -ENOENT;
    }

    std::vector<uint8_t> blob;
    if (d->store().read_blob(entry->hash, blob)) {
        st->st_size = (off_t)blob.size();
    }
    // Surface read-your-writes via any open dirty fh for this path.
    // libfuse 2.9 getattr has no fi pointer; the Linux adapter takes
    // this same code path when fi is null.
    int64_t eff = d->dirty_fh_size_for_path(path, br->branch_id);
    if (eff >= 0) st->st_size = (off_t)eff;

    st->st_nlink = 1;
    st->st_uid = fuse_get_context()->uid;
    st->st_gid = fuse_get_context()->gid;
    // Synthesize mtime/ctime from the blob hash. fuse-t bridges through
    // macOS's NFS loopback, whose change-detection compares attribute
    // tuples (size + mtime + ctime). When pre- and post-rollback content
    // happens to be the same size — e.g., "v1\n" vs "v2\n" — and mtime
    // stays at zero, the NFS client treats the file as unchanged and
    // serves the cached page without round-tripping a fresh read to our
    // cas_read handler. Deriving mtime from the entry hash gives every
    // distinct content version a distinct attribute tuple, so the cache
    // invalidates whenever real content changes. (Identical content
    // dedupes to the same hash, which is the right preservation
    // behavior.) For dirty fh overrides we shift the hash slightly so
    // a write that hasn't been materialized yet still presents as
    // "different from disk".
    {
        uint64_t h64 = 0;
        for (int i = 0; i < 8; ++i)
            h64 = (h64 << 8) | entry->hash[i];
        if (eff >= 0) h64 ^= 0xFFFFFFFFu;  // distinguish dirty fh state.
        time_t mt = static_cast<time_t>(h64 & 0x7FFFFFFFu);
        // st_mtime / st_ctime / st_atime are macros that expand into
        // the timespec member's tv_sec on Apple; we leave tv_nsec at
        // the zero-initialized value from memset above.
        st->st_mtime = mt;
        st->st_ctime = mt;
        st->st_atime = mt;
    }
    return 0;
}

static int cas_open(const char* path, struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path) || is_appledouble(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

    auto br = resolve_branch(d);
    {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        auto entry = br->wt.lookup(path);
        if (!entry) return -ENOENT;
        if (entry->kind != EntryKind::Blob) return -EISDIR;

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
        if (truncated) state->base_cache_loaded = true;
        state->write_buf = std::make_unique<WriteBuffer>(base_blob, base_size);
        fi->fh = d->allocate_fh(std::move(state));
    }
    fi->direct_io = 1;
    return 0;
}

static int cas_create(const char* path, mode_t mode,
                      struct fuse_file_info* fi) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    // AppleDouble creates: pretend success, no tree mutation. Subsequent
    // read/write/release on this fi short-circuit via is_appledouble
    // before ever touching the fh table, so fh=0 is safe as a sentinel
    // here — it never reaches Daemon::release_fh. (Daemon::allocate_fh
    // returns ids starting at 1, so 0 cannot collide with a real fh.)
    if (is_appledouble(path)) { fi->fh = 0; fi->direct_io = 1; return 0; }

    auto br = resolve_branch(d);
    {
        std::lock_guard<std::mutex> lk(br->checkpoint_mu);
        Hash empty = d->store().write_blob(nullptr, 0);
        br->wt.insert(path, {EntryKind::Blob, empty,
                             (uint32_t)(mode & 07777) | 0100000});

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
    fi->direct_io = 1;
    return 0;
}

static int cas_read(const char* path, char* buf, size_t size, off_t offset,
                    struct fuse_file_info* fi) {
    if (is_appledouble(path)) return -ENOENT;
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return -ESTALE;
    d->ensure_base_cache(*s);
    size_t n = s->write_buf->read((uint64_t)offset, (uint8_t*)buf, size,
                                  s->base_cache);
    return (int)n;
}

static int cas_write(const char* path, const char* buf, size_t size,
                     off_t offset, struct fuse_file_info* fi) {
    if (is_appledouble(path)) return (int)size;
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return -ESTALE;
    s->write_buf->write((uint64_t)offset, (const uint8_t*)buf, size);
    return (int)size;
}

static int cas_flush(const char* path, struct fuse_file_info* fi) {
    if (is_appledouble(path)) return 0;
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return -ESTALE;
    if (!s->write_buf || !s->write_buf->is_dirty()) return 0;

    d->ensure_base_cache(*s);
    auto data = s->write_buf->materialize(s->base_cache);
    Hash new_blob = d->store().write_blob(data);
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

static int cas_fsync(const char* path, int /*datasync*/,
                     struct fuse_file_info* fi) {
    return cas_flush(path, fi);
}

static int cas_release(const char* path, struct fuse_file_info* fi) {
    if (is_appledouble(path)) return 0;
    Daemon* d = get_daemon();
    (void)cas_flush(path, fi);
    d->release_fh(fi->fh);
    fi->fh = 0;
    return 0;
}

static int cas_readdir(const char* path, void* buf, fuse_fill_dir_t filler,
                       off_t /*offset*/, struct fuse_file_info* /*fi*/) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    if (std::strcmp(path, "/") != 0) {
        auto e = br->wt.lookup(path);
        if (!e || e->kind != EntryKind::Tree) return -ENOENT;
    }
    // libfuse 2.9 filler: int(*)(void*, const char*, const struct stat*, off_t).
    filler(buf, ".",  nullptr, 0);
    filler(buf, "..", nullptr, 0);
    for (auto& [child_path, entry] : br->wt.list_dir(path)) {
        std::string base = child_path;
        auto slash = base.rfind('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base == ".agentvfs-store") continue;
        filler(buf, base.c_str(), nullptr, 0);
    }
    return 0;
}

static int cas_truncate(const char* path, off_t length) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (is_appledouble(path)) return 0;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);

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

static int cas_ftruncate(const char* path, off_t length,
                         struct fuse_file_info* fi) {
    if (is_appledouble(path)) return 0;
    Daemon* d = get_daemon();
    auto s = d->get_fh(fi->fh);
    if (!s) return -EBADF;
    auto br = branch_for_fh(d, s);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    if (s->stale) return -ESTALE;
    s->write_buf->truncate((uint64_t)length);
    return 0;
}

static int cas_access(const char* path, int mask) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path) || is_appledouble(path)) return -ENOENT;
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

static int cas_statfs(const char* /*path*/, struct statvfs* stat) {
    Daemon* d = get_daemon();
    if (statvfs(d->store_root().c_str(), stat) != 0) return errno_to_err();
    return 0;
}

static int cas_unlink(const char* path) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path)) return -EACCES;
    if (is_appledouble(path)) return 0;
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
    if (!br->wt.list_dir(path).empty()) return -ENOTEMPTY;
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
    br->wt.insert(path, {EntryKind::Tree, ZERO_HASH,
                         (uint32_t)(mode & 07777) | 040000});
    return 0;
}

static int cas_rename(const char* from, const char* to) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(from) || Daemon::is_hidden(to)) return -EACCES;
    if (is_appledouble(from) || is_appledouble(to)) return 0;
    if (auto* bs = d->bootstrap()) { bs->ensure_path(from); bs->ensure_path(to); }
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    auto e = br->wt.lookup(from);
    if (!e) return -ENOENT;
    if (e->kind == EntryKind::Tree) br->wt.rename_dir(from, to);
    else                            br->wt.rename_entry(from, to);
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
    if (Daemon::is_hidden(path) || is_appledouble(path)) return -ENOENT;
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

static int cas_chmod(const char* path, mode_t mode) {
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path) || is_appledouble(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    std::lock_guard<std::mutex> lk(br->checkpoint_mu);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    entry->mode = (entry->mode & S_IFMT) | (mode & 07777);
    br->wt.insert(path, *entry);
    return 0;
}

static int cas_chown(const char* path, uid_t uid, gid_t gid) {
    (void)uid;
    (void)gid;
    Daemon* d = get_daemon();
    if (Daemon::is_hidden(path) || is_appledouble(path)) return -ENOENT;
    if (auto* bs = d->bootstrap()) bs->ensure_path(path);
    auto br = resolve_branch(d);
    auto entry = br->wt.lookup(path);
    if (!entry) return -ENOENT;
    return 0;
}

int run_filesystem(Daemon& daemon, const MountOptions& opts) {
    struct fuse_operations ops{};
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
    ops.ftruncate = cas_ftruncate;
    ops.access    = cas_access;
    ops.statfs    = cas_statfs;
    ops.unlink    = cas_unlink;
    ops.rmdir     = cas_rmdir;
    ops.mkdir     = cas_mkdir;
    ops.rename    = cas_rename;
    ops.symlink   = cas_symlink;
    ops.readlink  = cas_readlink;
    ops.chmod     = cas_chmod;
    ops.chown     = cas_chown;
    // xattr callbacks left unset: libfuse returns -ENOSYS, which
    // clients treat equivalently to ENOTSUP. Spec §"Semantic gaps".

    // libfuse takes a non-const argv. Static mutable arrays for the
    // option flags rather than const_cast'ing string literals. The
    // statics are not reentrant — fine because run_filesystem runs
    // exactly once per process (fuse_main calls exit() on signal), and
    // mirrors the Linux adapter's same trade-off.
    static char prog_name[] = "agentvfs";
    static char fg_flag[]   = "-f";
    static char st_flag[]   = "-s";

    std::vector<char*> fargv;
    fargv.push_back(prog_name);
    fargv.push_back(const_cast<char*>(opts.mountpoint.c_str()));
    if (opts.foreground) fargv.push_back(fg_flag);
    if (opts.single_threaded) fargv.push_back(st_flag);
    for (auto& s : opts.passthrough_args) {
        fargv.push_back(const_cast<char*>(s.c_str()));
    }

    return fuse_main((int)fargv.size(), fargv.data(), &ops, &daemon);
}

}  // namespace cas
