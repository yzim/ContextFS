// agentvfs_null_fuse: measurement-ceiling daemon for the fuse-io benchmark.
//
// Serves the benchmark fixture namespace entirely from memory through the
// same mount profile as AgentVFS on Linux — high-level libfuse, foreground,
// attr_timeout=0 / entry_timeout=0 / negative_timeout=0, direct_io on every
// open — but performs zero filesystem, store, or routing work per request.
// Whatever throughput agentvfs_fuse_io_bench measures against this mount is
// the host's hard ceiling for ANY same-mount FUSE architecture with the
// zero-TTL cache contract; the gap between AgentVFS and this daemon is the
// part userspace optimization can still reach.
//
//   agentvfs_null_fuse <mountpoint> [-s]
//
// Fixture geometry (must match run.sh defaults unless overridden):
//   NULLFUSE_DIRS           directories under /data          (default 100)
//   NULLFUSE_FILES_PER_DIR  files per directory              (default 100)
//   NULLFUSE_LARGE_BYTES    size of /large.bin               (default 1 GiB)
//
// git-status has no meaning here (no .git); every other benchmark case runs.

#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

int g_dirs = 100;
int g_files_per_dir = 100;
int64_t g_large_bytes = 1073741824;
time_t g_epoch = 0;

// Same 4 KiB block for every data file; content is irrelevant to the
// ceiling, only the transfer is.
unsigned char g_block[4096];
// 1 MiB zero window for large.bin reads.
std::vector<unsigned char> g_zeros(1024 * 1024, 0);

// Files created under /.bench-write by the write cases. Guarded for the
// multithreaded session.
std::mutex g_write_mu;
std::unordered_map<std::string, int64_t> g_write_files;

void fill_stat(struct stat* st, mode_t mode, nlink_t nlink, int64_t size) {
    std::memset(st, 0, sizeof(*st));
    st->st_mode = mode;
    st->st_nlink = nlink;
    st->st_size = size;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = st->st_mtime = st->st_ctime = g_epoch;
    st->st_blksize = 4096;
    st->st_blocks = (size + 511) / 512;
}

// Path classification for the synthetic namespace. Returns true and fills
// *st when the path exists.
bool classify(const char* path, struct stat* st) {
    if (std::strcmp(path, "/") == 0 || std::strcmp(path, "/data") == 0 ||
        std::strcmp(path, "/.bench-write") == 0) {
        fill_stat(st, S_IFDIR | 0755, 2, 4096);
        return true;
    }
    if (std::strcmp(path, "/large.bin") == 0) {
        fill_stat(st, S_IFREG | 0644, 1, g_large_bytes);
        return true;
    }
    int dir = -1, index = -1;
    char trailing = 0, extra = 0;
    if (std::sscanf(path, "/data/d%3d%c", &dir, &trailing) == 1) {
        if (dir >= 0 && dir < g_dirs) {
            fill_stat(st, S_IFDIR | 0755, 2, 4096);
            return true;
        }
        return false;
    }
    if (std::sscanf(path, "/data/d%3d/f%5d.da%c%c",
                    &dir, &index, &trailing, &extra) == 3 &&
        trailing == 't') {
        if (dir >= 0 && dir < g_dirs &&
            index >= dir * g_files_per_dir &&
            index < (dir + 1) * g_files_per_dir) {
            fill_stat(st, S_IFREG | 0644, 1, (int64_t)sizeof(g_block));
            return true;
        }
        return false;
    }
    if (std::strncmp(path, "/.bench-write/", 14) == 0) {
        std::lock_guard<std::mutex> lk(g_write_mu);
        auto it = g_write_files.find(path);
        if (it == g_write_files.end()) return false;
        fill_stat(st, S_IFREG | 0644, 1, it->second);
        return true;
    }
    return false;
}

int nf_getattr(const char* path, struct stat* st, struct fuse_file_info*) {
    return classify(path, st) ? 0 : -ENOENT;
}

int nf_open(const char* path, struct fuse_file_info* fi) {
    struct stat st;
    if (!classify(path, &st)) return -ENOENT;
    if (S_ISDIR(st.st_mode)) return -EISDIR;
    fi->direct_io = 1;
    return 0;
}

int nf_create(const char* path, mode_t, struct fuse_file_info* fi) {
    if (std::strncmp(path, "/.bench-write/", 14) != 0) return -EACCES;
    {
        std::lock_guard<std::mutex> lk(g_write_mu);
        g_write_files[path] = 0;
    }
    fi->direct_io = 1;
    return 0;
}

int nf_read(const char* path, char* buf, size_t size, off_t offset,
            struct fuse_file_info*) {
    if (std::strcmp(path, "/large.bin") == 0) {
        if (offset >= g_large_bytes) return 0;
        size_t n = size;
        if ((int64_t)n > g_large_bytes - offset)
            n = (size_t)(g_large_bytes - offset);
        size_t done = 0;
        while (done < n) {
            size_t chunk = std::min(n - done, g_zeros.size());
            std::memcpy(buf + done, g_zeros.data(), chunk);
            done += chunk;
        }
        return (int)n;
    }
    struct stat st;
    if (!classify(path, &st)) return -ENOENT;
    if (offset >= st.st_size) return 0;
    size_t n = size;
    if ((int64_t)n > st.st_size - offset) n = (size_t)(st.st_size - offset);
    if (n > sizeof(g_block)) n = sizeof(g_block);
    std::memcpy(buf, g_block + offset, n);
    return (int)n;
}

int nf_write(const char* path, const char*, size_t size, off_t offset,
             struct fuse_file_info*) {
    std::lock_guard<std::mutex> lk(g_write_mu);
    auto it = g_write_files.find(path);
    if (it == g_write_files.end()) return -ENOENT;
    int64_t end = (int64_t)offset + (int64_t)size;
    if (end > it->second) it->second = end;
    return (int)size;
}

int nf_truncate(const char* path, off_t size, struct fuse_file_info*) {
    std::lock_guard<std::mutex> lk(g_write_mu);
    auto it = g_write_files.find(path);
    if (it == g_write_files.end()) return -EACCES;
    it->second = size;
    return 0;
}

int nf_unlink(const char* path) {
    std::lock_guard<std::mutex> lk(g_write_mu);
    return g_write_files.erase(path) ? 0 : -ENOENT;
}

int nf_readdir(const char* path, void* buf, fuse_fill_dir_t filler, off_t,
               struct fuse_file_info*, enum fuse_readdir_flags) {
    filler(buf, ".", nullptr, 0, (enum fuse_fill_dir_flags)0);
    filler(buf, "..", nullptr, 0, (enum fuse_fill_dir_flags)0);
    char name[32];
    if (std::strcmp(path, "/") == 0) {
        filler(buf, "data", nullptr, 0, (enum fuse_fill_dir_flags)0);
        filler(buf, ".bench-write", nullptr, 0, (enum fuse_fill_dir_flags)0);
        filler(buf, "large.bin", nullptr, 0, (enum fuse_fill_dir_flags)0);
        return 0;
    }
    if (std::strcmp(path, "/data") == 0) {
        for (int d = 0; d < g_dirs; ++d) {
            std::snprintf(name, sizeof(name), "d%03d", d);
            filler(buf, name, nullptr, 0, (enum fuse_fill_dir_flags)0);
        }
        return 0;
    }
    int dir = -1;
    char trailing = 0;
    if (std::sscanf(path, "/data/d%3d%c", &dir, &trailing) == 1 &&
        dir >= 0 && dir < g_dirs) {
        for (int f = 0; f < g_files_per_dir; ++f) {
            std::snprintf(name, sizeof(name), "f%05d.dat",
                          dir * g_files_per_dir + f);
            filler(buf, name, nullptr, 0, (enum fuse_fill_dir_flags)0);
        }
        return 0;
    }
    if (std::strcmp(path, "/.bench-write") == 0) return 0;
    return -ENOENT;
}

int64_t env_int(const char* name, int64_t fallback) {
    const char* raw = std::getenv(name);
    if (!raw || !*raw) return fallback;
    char* end = nullptr;
    long long v = std::strtoll(raw, &end, 10);
    return (end && *end == '\0' && v > 0) ? (int64_t)v : fallback;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "Usage: agentvfs_null_fuse <mountpoint> [-s]\n");
        return 2;
    }
    g_dirs = (int)env_int("NULLFUSE_DIRS", 100);
    g_files_per_dir = (int)env_int("NULLFUSE_FILES_PER_DIR", 100);
    g_large_bytes = env_int("NULLFUSE_LARGE_BYTES", 1073741824);
    g_epoch = time(nullptr);
    std::memset(g_block, 'x', sizeof(g_block));

    struct fuse_operations ops {};
    ops.getattr = nf_getattr;
    ops.open = nf_open;
    ops.create = nf_create;
    ops.read = nf_read;
    ops.write = nf_write;
    ops.truncate = nf_truncate;
    ops.unlink = nf_unlink;
    ops.readdir = nf_readdir;

    // Mirror the AgentVFS Linux adapter's mount profile exactly.
    static char prog_name[] = "agentvfs_null_fuse";
    static char fg_flag[] = "-f";
    static char st_flag[] = "-s";
    static char opt_flag[] = "-o";
    static char cache_opts[] =
        "attr_timeout=0,entry_timeout=0,negative_timeout=0";

    bool single_threaded = false;
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], "-s") == 0) single_threaded = true;

    std::vector<char*> fargv;
    fargv.push_back(prog_name);
    fargv.push_back(argv[1]);
    fargv.push_back(fg_flag);
    if (single_threaded) fargv.push_back(st_flag);
    fargv.push_back(opt_flag);
    fargv.push_back(cache_opts);

    return fuse_main((int)fargv.size(), fargv.data(), &ops, nullptr);
}
