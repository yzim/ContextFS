#include "refs.h"
#include "posix_compat.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

namespace cas {

namespace fs = std::filesystem;

Refs::Refs(const std::string& store_root)
    : refs_dir_(store_root + "/refs")
{}

std::vector<std::string> Refs::list_refs() const {
    std::vector<std::string> names;
    std::error_code ec;
    // directory_iterator already skips "." / "..". Use the error_code
    // overload so a missing/permission-denied refs dir doesn't throw.
    for (auto it = fs::directory_iterator(refs_dir_, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::error_code rec;
        if (!it->is_regular_file(rec)) continue;
        names.push_back(it->path().filename().string());
    }

    std::sort(names.begin(), names.end());
    return names;
}

bool Refs::read_ref(const std::string& branch, Hash& out) const {
    std::string path = refs_dir_ + "/" + branch;
    int fd = open(path.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) return false;
    char buf[65];
    ssize_t n = read(fd, buf, 64);
    close(fd);
    if (n < 64) return false;
    buf[64] = '\0';
    return hex_to_hash(buf, out);
}

bool Refs::write_ref(const std::string& branch, const Hash& commit_hash,
                     const std::string& tmp_dir) {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    char name[32];
    // %llx / unsigned long long because `unsigned long` is 32-bit on MSVC.
    std::snprintf(name, sizeof(name), "ref_%016llx", (unsigned long long)rng());
    std::string tmp_path = tmp_dir + "/" + name;

    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "agentvfs: refs: open('%s'): %s\n",
                     tmp_path.c_str(), std::strerror(errno));
        return false;
    }

    std::string hex = hash_to_hex(commit_hash) + "\n";
    bool ok = (::write(fd, hex.c_str(), hex.size()) == (ssize_t)hex.size());
    int werr = errno;
    if (ok) ok = (fsync(fd) == 0);
    int ferr = errno;
    close(fd);
    if (!ok) {
        std::fprintf(stderr, "agentvfs: refs: write/fsync('%s'): %s\n",
                     tmp_path.c_str(), std::strerror(werr ? werr : ferr));
        std::error_code rec;
        fs::remove(tmp_path, rec);
        return false;
    }

    std::string ref_path = refs_dir_ + "/" + branch;

#ifndef _WIN32
    // POSIX: durability requires fsync of the containing directory after
    // a rename. We open the dir read-only, rename in, then fsync the dir
    // fd so a subsequent crash either sees the old ref or the new one,
    // never the rename-without-fsync window. Windows has no equivalent
    // need (NTFS metadata journal makes the rename durable on its own),
    // so the whole dance is skipped there.
    int dir_fd = open(refs_dir_.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        std::fprintf(stderr, "agentvfs: refs: open refs dir '%s': %s\n",
                     refs_dir_.c_str(), std::strerror(errno));
        std::error_code rec;
        fs::remove(tmp_path, rec);
        return false;
    }

    if (rename(tmp_path.c_str(), ref_path.c_str()) != 0) {
        int saved = errno;
        close(dir_fd);
        std::fprintf(stderr, "agentvfs: refs: rename('%s' -> '%s'): %s\n",
                     tmp_path.c_str(), ref_path.c_str(), std::strerror(saved));
        std::error_code rec;
        fs::remove(tmp_path, rec);
        errno = saved;
        return false;
    }

    if (fsync(dir_fd) != 0) {
        int saved = errno;
        close(dir_fd);
        std::fprintf(stderr, "agentvfs: refs: fsync refs dir '%s': %s\n",
                     refs_dir_.c_str(), std::strerror(saved));
        errno = saved;
        return false;
    }
    if (close(dir_fd) != 0) {
        std::fprintf(stderr, "agentvfs: refs: close refs dir '%s': %s\n",
                     refs_dir_.c_str(), std::strerror(errno));
        return false;
    }
#else
    std::error_code rec;
    fs::rename(tmp_path, ref_path, rec);
    if (rec) {
        std::fprintf(stderr, "agentvfs: refs: rename('%s' -> '%s'): %s\n",
                     tmp_path.c_str(), ref_path.c_str(), rec.message().c_str());
        std::error_code rmec;
        fs::remove(tmp_path, rmec);
        return false;
    }
#endif
    return true;
}

bool Refs::remove_ref(const std::string& branch) {
    if (branch == "main") return false;
    std::string path = refs_dir_ + "/" + branch;
#ifndef _WIN32
    int dir_fd = open(refs_dir_.c_str(), O_RDONLY | O_DIRECTORY);
    if (dir_fd < 0) {
        std::fprintf(stderr, "agentvfs: refs: open refs dir '%s': %s\n",
                     refs_dir_.c_str(), std::strerror(errno));
        return false;
    }
    if (unlink(path.c_str()) != 0) {
        int saved = errno;
        close(dir_fd);
        std::fprintf(stderr, "agentvfs: refs: unlink('%s'): %s\n",
                     path.c_str(), std::strerror(saved));
        errno = saved;
        return false;
    }
    if (fsync(dir_fd) != 0) {
        int saved = errno;
        close(dir_fd);
        std::fprintf(stderr, "agentvfs: refs: fsync refs dir '%s': %s\n",
                     refs_dir_.c_str(), std::strerror(saved));
        errno = saved;
        return false;
    }
    if (close(dir_fd) != 0) {
        std::fprintf(stderr, "agentvfs: refs: close refs dir '%s': %s\n",
                     refs_dir_.c_str(), std::strerror(errno));
        return false;
    }
    return true;
#else
    std::error_code ec;
    return fs::remove(path, ec) && !ec;
#endif
}

} // namespace cas
