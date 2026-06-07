#include "object_store.h"
#include "blake3.h"
#include "posix_compat.h"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <set>
#include <utility>

namespace cas {

namespace fs = std::filesystem;

namespace {

bool validate_object_shards_writable(const std::string& objects_dir) {
#ifdef _WIN32
    (void)objects_dir;
    return true;
#else
    std::error_code ec;
    for (auto it = fs::directory_iterator(objects_dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::error_code type_ec;
        if (!it->is_directory(type_ec)) continue;

        std::string path = it->path().string();
        if (::access(path.c_str(), W_OK | X_OK) != 0) {
            std::fprintf(stderr,
                "agentvfs: init_layout: object shard '%s' is not writable: %s\n",
                path.c_str(), std::strerror(errno));
            return false;
        }
    }
    if (ec) {
        std::fprintf(stderr,
            "agentvfs: init_layout: scan object shards in '%s' failed: %s\n",
            objects_dir.c_str(), ec.message().c_str());
        return false;
    }
    return true;
#endif
}

} // namespace

ObjectStore::ObjectStore(const std::string& store_root)
    : store_root_(store_root)
    , objects_dir_(store_root + "/objects")
    , tmp_dir_(store_root + "/tmp")
{}

std::string ObjectStore::last_error() const {
    std::lock_guard<std::mutex> lk(error_mu_);
    return last_error_;
}

void ObjectStore::set_last_error(std::string error) const {
    std::lock_guard<std::mutex> lk(error_mu_);
    last_error_ = std::move(error);
}

bool ObjectStore::init_layout() {
    std::string refs_dir      = store_root_ + "/refs";
    std::string telemetry_dir = store_root_ + "/telemetry";
    const std::string* dirs[] = {
        &store_root_, &objects_dir_, &tmp_dir_, &refs_dir, &telemetry_dir,
    };
    for (const std::string* d : dirs) {
        std::error_code ec;
        // create_directory returns false (without ec) when the directory
        // already exists — exactly matching the old `mkdir != 0 &&
        // errno != EEXIST` behavior. On Windows, the path's parent must
        // already exist; create_directories would recurse, create_directory
        // does not.
        fs::create_directory(*d, ec);
        if (ec) {
            std::fprintf(stderr,
                "agentvfs: init_layout: create_directory('%s') failed: %s\n",
                d->c_str(), ec.message().c_str());
            return false;
        }
    }
    std::string ver_path = store_root_ + "/version";
    int fd = open(ver_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0644);
    if (fd >= 0) {
        ssize_t wr = write(fd, "1\n", 2);
        (void)wr;
        close(fd);
    }
    if (!validate_object_shards_writable(objects_dir_)) return false;
    return true;
}

void ObjectStore::cleanup_tmp() {
    std::error_code ec;
    for (auto it = fs::directory_iterator(tmp_dir_, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::string name = it->path().filename().string();
        // Skip dotfiles. directory_iterator already skips "." and ".." so
        // a non-empty name front-checked against '.' covers the rest.
        if (!name.empty() && name.front() == '.') continue;
        std::error_code rec;
        fs::remove(it->path(), rec);
    }
}

static std::string random_tmp_name() {
    static thread_local std::mt19937_64 rng(std::random_device{}());
    char buf[24];
    // %llx / unsigned long long because `unsigned long` is 32-bit on MSVC;
    // we want all 64 random bits in the tmp filename for collision safety.
    std::snprintf(buf, sizeof(buf), "tmp_%016llx", (unsigned long long)rng());
    return buf;
}

Hash ObjectStore::write_object(const char* type_tag, const uint8_t* body, size_t body_len) {
    set_last_error("");
    size_t tag_len = std::strlen(type_tag);

    blake3_hasher hasher;
    blake3_hasher_init(&hasher);
    blake3_hasher_update(&hasher, type_tag, tag_len);
    blake3_hasher_update(&hasher, body, body_len);
    Hash hash;
    blake3_hasher_finalize(&hasher, hash.data(), BLAKE3_OUT_LEN);

    if (object_exists(hash)) return hash;

    std::string shard = objects_dir_ + "/" + hash_to_hex(hash).substr(0, 2);
    {
        std::error_code ec;
        fs::create_directory(shard, ec);
        if (ec) {
            set_last_error(std::string("write ") + type_tag +
                           " object: create_directory('" + shard +
                           "') failed: " + ec.message());
            return ZERO_HASH;
        }
    }

    std::string tmp_path = tmp_dir_ + "/" + random_tmp_name();
    int fd = open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0444);
    if (fd < 0) {
        set_last_error(std::string("write ") + type_tag +
                       " object: open('" + tmp_path +
                       "') failed: " + std::strerror(errno));
        return ZERO_HASH;
    }

    bool ok = true;
    std::string write_error;
    if (::write(fd, type_tag, tag_len) != (ssize_t)tag_len) {
        ok = false;
        write_error = std::strerror(errno);
    }
    if (ok && body_len > 0 && ::write(fd, body, body_len) != (ssize_t)body_len) {
        ok = false;
        write_error = std::strerror(errno);
    }
    close(fd);

    if (!ok) {
        std::error_code rec;
        fs::remove(tmp_path, rec);
        set_last_error(std::string("write ") + type_tag +
                       " object: write('" + tmp_path +
                       "') failed: " + write_error);
        return ZERO_HASH;
    }

    std::string target = object_path(hash);
    std::error_code rec;
    fs::rename(tmp_path, target, rec);
    if (rec) {
        std::error_code rmec;
        fs::remove(tmp_path, rmec);
        set_last_error(std::string("write ") + type_tag +
                       " object: rename('" + tmp_path +
                       "', '" + target +
                       "') failed: " + rec.message());
        return ZERO_HASH;
    }

    {
        std::lock_guard<std::mutex> lk(pending_mu_);
        pending_.insert(hash);
    }
    return hash;
}

std::vector<Hash> ObjectStore::drain_pending() {
    std::lock_guard<std::mutex> lk(pending_mu_);
    std::vector<Hash> out(pending_.begin(), pending_.end());
    pending_.clear();
    return out;
}

void ObjectStore::restore_pending(const std::vector<Hash>& hashes) {
    std::lock_guard<std::mutex> lk(pending_mu_);
    for (const auto& h : hashes) pending_.insert(h);
}

size_t ObjectStore::pending_count() const {
    std::lock_guard<std::mutex> lk(pending_mu_);
    return pending_.size();
}

bool ObjectStore::read_object(const Hash& hash, const char* expected_tag, std::vector<uint8_t>& out) {
    std::string path = object_path(hash);

    std::error_code ec;
    std::uintmax_t fsize = fs::file_size(path, ec);
    if (ec) return false;

    int fd = open(path.c_str(), O_RDONLY | O_BINARY);
    if (fd < 0) return false;

    std::vector<uint8_t> raw(static_cast<size_t>(fsize));
    ssize_t n = read(fd, raw.data(), raw.size());
    close(fd);
    if (n != (ssize_t)fsize) return false;

    size_t tag_len = std::strlen(expected_tag);
    if (raw.size() < tag_len) return false;
    if (std::memcmp(raw.data(), expected_tag, tag_len) != 0) return false;

    out.assign(raw.begin() + tag_len, raw.end());
    return true;
}

Hash ObjectStore::write_blob(const uint8_t* data, size_t len) {
    // Hash: blake3("blob" || size_le64 || content)
    uint64_t size_le = len;
    std::vector<uint8_t> body(8 + len);
    std::memcpy(body.data(), &size_le, 8);
    std::memcpy(body.data() + 8, data, len);
    return write_object("blob", body.data(), body.size());
}

Hash ObjectStore::write_blob(const std::vector<uint8_t>& data) {
    return write_blob(data.data(), data.size());
}

bool ObjectStore::read_blob(const Hash& hash, std::vector<uint8_t>& out) {
    std::vector<uint8_t> body;
    if (!read_object(hash, "blob", body)) return false;
    if (body.size() < 8) return false;
    uint64_t size_le;
    std::memcpy(&size_le, body.data(), 8);
    if (body.size() - 8 != size_le) return false;
    out.assign(body.begin() + 8, body.end());
    return true;
}

Hash ObjectStore::write_tree(const std::vector<uint8_t>& serialized) {
    return write_object("tree", serialized.data(), serialized.size());
}

bool ObjectStore::read_tree(const Hash& hash, std::vector<uint8_t>& out) {
    return read_object(hash, "tree", out);
}

Hash ObjectStore::write_commit(const std::vector<uint8_t>& serialized) {
    return write_object("commit", serialized.data(), serialized.size());
}

bool ObjectStore::read_commit(const Hash& hash, std::vector<uint8_t>& out) {
    return read_object(hash, "commit", out);
}

bool ObjectStore::object_exists(const Hash& hash) const {
    std::error_code ec;
    return fs::exists(object_path(hash), ec) && !ec;
}

std::string ObjectStore::object_path(const Hash& hash) const {
    std::string hex = hash_to_hex(hash);
    return objects_dir_ + "/" + hex.substr(0, 2) + "/" + hex;
}

bool ObjectStore::fsync_objects(const std::vector<Hash>& hashes) {
#ifndef _WIN32
    // POSIX per-file fdatasync barrier. Windows skips this sweep:
    //   1. NTFS journals the metadata write at close time, so each
    //      object becomes durable as soon as write_object closes it.
    //   2. _commit (which posix_compat maps fdatasync to) requires a
    //      file opened for writing — re-opening O_RDONLY here would
    //      get EBADF and fail initialize() on every fresh Windows
    //      store, matching the symptom in CI.
    for (auto& h : hashes) {
        int fd = open(object_path(h).c_str(), O_RDONLY | O_BINARY);
        if (fd < 0) return false;
        if (fdatasync(fd) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return false;
        }
        if (close(fd) != 0) return false;
    }
#else
    (void)hashes;
#endif
    return true;
}

bool ObjectStore::fsync_shard_dirs(const std::vector<Hash>& hashes) {
#ifndef _WIN32
    // POSIX directory fsync barrier — see refs.cpp for the rationale.
    // On Windows the corresponding directory entries are journaled by NTFS,
    // so the whole sweep is a no-op there.
    std::set<std::string> shards;
    for (auto& h : hashes)
        shards.insert(objects_dir_ + "/" + hash_to_hex(h).substr(0, 2));
    if (shards.empty()) return true;

    for (auto& s : shards) {
        int fd = open(s.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) return false;
        if (fsync(fd) != 0) {
            int saved = errno;
            close(fd);
            errno = saved;
            return false;
        }
        if (close(fd) != 0) return false;
    }

    int objects_fd = open(objects_dir_.c_str(), O_RDONLY | O_DIRECTORY);
    if (objects_fd < 0) return false;
    if (fsync(objects_fd) != 0) {
        int saved = errno;
        close(objects_fd);
        errno = saved;
        return false;
    }
    if (close(objects_fd) != 0) return false;
#else
    (void)hashes;
#endif
    return true;
}

} // namespace cas
