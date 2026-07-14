#include "bootstrap.h"
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace cas {

namespace fs = std::filesystem;

static bool is_reserved_root_name(const std::string& name) {
    return name == ".agentvfs-store" || name == ".agentvfs-control";
}

static std::string source_abs(const std::string& root, const std::string& vpath) {
    return (vpath == "/") ? root : root + vpath;
}

static bool is_deleted(const std::optional<WorkingTreeEntry>& entry) {
    return entry.has_value() && entry->kind == EntryKind::Deleted;
}

static void log_bootstrap_failure(const char* operation,
                                  const std::string& path,
                                  const std::string& error) {
    std::fprintf(stderr, "agentvfs: bootstrap: %s '%s' failed: %s\n",
                 operation, path.c_str(), error.c_str());
}

// Map a std::filesystem::file_type + POSIX permission bits into the same
// mode word the previous lstat-based code stored on WorkingTreeEntry. This
// preserves the wire/object-store representation: tree, blob, and symlink
// entries still carry the original mode bits readers expect.
static uint32_t encode_mode(fs::file_type type, fs::perms perms) {
    uint32_t mode = static_cast<uint32_t>(perms) & 07777;
    switch (type) {
        case fs::file_type::regular:   mode |= 0100000; break;  // S_IFREG
        case fs::file_type::directory: mode |= 0040000; break;  // S_IFDIR
        case fs::file_type::symlink:   mode |= 0120000; break;  // S_IFLNK
        default: break;
    }
    return mode;
}

Bootstrap::Bootstrap(std::string source_root, ObjectStore& store,
                     WorkingTree& wt, InodeMap& inode_map,
                     std::mutex& checkpoint_mu)
    : source_root_(std::move(source_root))
    , store_(store), wt_(wt), inode_map_(inode_map)
    , checkpoint_mu_(checkpoint_mu) {}

Bootstrap::~Bootstrap() { stop_background(); }

Bootstrap::IngestResult Bootstrap::ingest_entry(
    const std::string& vpath,
    const std::string& source_abspath,
    std::string& error) {
    error.clear();
    try {
        std::error_code ec;
        fs::file_status st = fs::symlink_status(source_abspath, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory ||
                ec == std::errc::not_a_directory)
                return IngestResult::Missing;
            error = "symlink_status: " + ec.message();
            return IngestResult::Failed;
        }
        if (st.type() == fs::file_type::not_found)
            return IngestResult::Missing;

        uint32_t mode = encode_mode(st.type(), st.permissions());
        // NOTE: no inode_map_.set() here. Source-side (dev, ino) keys are never
        // consulted: the BPF probe reads dev/ino from the FUSE-delivered inode
        // (mount-side), and policy.install populates inode_map_ via a mount-path
        // lstat. Inserting source-side keys would be dead state.

        if (st.type() == fs::file_type::directory) {
            wt_.insert_source(vpath, {EntryKind::Tree, ZERO_HASH, mode});
            return IngestResult::Inserted;
        }

        if (st.type() == fs::file_type::symlink) {
            auto target = fs::read_symlink(source_abspath, ec);
            if (ec) {
                error = "read_symlink: " + ec.message();
                return IngestResult::Failed;
            }
            std::string target_str = target.string();
            Hash h = store_.write_blob(
                reinterpret_cast<const uint8_t*>(target_str.data()),
                target_str.size());
            if (h == ZERO_HASH) {
                error = store_.last_error();
                if (error.empty()) error = "write symlink blob returned zero hash";
                return IngestResult::Failed;
            }
            wt_.insert_source(vpath, {EntryKind::Symlink, h, mode});
            return IngestResult::Inserted;
        }

        if (st.type() == fs::file_type::regular) {
            std::uintmax_t size = fs::file_size(source_abspath, ec);
            if (ec) {
                error = "file_size: " + ec.message();
                return IngestResult::Failed;
            }
            if (size > static_cast<std::uintmax_t>(
                           std::numeric_limits<size_t>::max()) ||
                size > static_cast<std::uintmax_t>(
                           std::numeric_limits<std::streamsize>::max())) {
                error = "file is too large to ingest";
                return IngestResult::Failed;
            }

            std::ifstream in(source_abspath, std::ios::binary);
            if (!in) {
                error = "open for reading failed";
                return IngestResult::Failed;
            }
            const auto expected = static_cast<std::streamsize>(size);
            std::vector<uint8_t> data(static_cast<size_t>(size));
            if (expected > 0) {
                in.read(reinterpret_cast<char*>(data.data()), expected);
                if (in.gcount() != expected || in.bad()) {
                    error = "short or failed file read";
                    return IngestResult::Failed;
                }
            }
            Hash h = store_.write_blob(data);
            if (h == ZERO_HASH) {
                error = store_.last_error();
                if (error.empty()) error = "write file blob returned zero hash";
                return IngestResult::Failed;
            }
            wt_.insert_source(vpath, {EntryKind::Blob, h, mode});
            return IngestResult::Inserted;
        }

        switch (st.type()) {
            case fs::file_type::block:
            case fs::file_type::character:
            case fs::file_type::fifo:
            case fs::file_type::socket:
                return IngestResult::Unsupported;
            default:
                error = "unknown filesystem entry type";
                return IngestResult::Failed;
        }
    } catch (const std::exception& ex) {
        error = ex.what();
        return IngestResult::Failed;
    }
}

bool Bootstrap::ensure_path(const std::string& vpath) {
    if (vpath == "/" || vpath.empty()) return true;

    // Serialize the complete CAS-write -> WT-publish sequence with main-branch
    // mutations, checkpoints, and GC live-root snapshots. Without this lock a
    // background/lazy ingest could overwrite a just-published whiteout or let
    // GC observe neither the pending object nor its eventual WT reference.
    std::lock_guard<std::mutex> checkpoint_lk(checkpoint_mu_);

    std::vector<std::string> missing;
    std::string cur = vpath;
    while (cur != "/") {
        auto existing = wt_.lookup_raw(cur);
        if (is_deleted(existing)) return false;
        if (existing.has_value()) break;
        missing.push_back(cur);
        auto slash = cur.rfind('/');
        cur = (slash == 0) ? "/" : cur.substr(0, slash);
    }

    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        auto existing = wt_.lookup_raw(*it);
        if (is_deleted(existing)) return false;
        if (existing.has_value()) continue;
        auto last_slash = it->rfind('/');
        std::string base = (last_slash == std::string::npos) ? *it : it->substr(last_slash + 1);
        if (is_reserved_root_name(base)) return false;
        std::string src = source_abs(source_root_, *it);
        std::string error;
        IngestResult result = ingest_entry(*it, src, error);
        if (result == IngestResult::Inserted) continue;
        if (result == IngestResult::Failed) {
            log_bootstrap_failure("ingest", src, error);
        }
        return false;
    }
    return true;
}

void Bootstrap::start_background() {
    {
        std::lock_guard<std::mutex> checkpoint_lk(checkpoint_mu_);
        wt_.begin_source_walk();
    }
    bg_ = std::thread([this] { walk_bg(); });
}

void Bootstrap::stop_background() {
    stop_ = true;
    if (bg_.joinable()) bg_.join();
}

void Bootstrap::walk_bg() {
    bool complete = true;
    try {
        std::deque<std::string> q;
        q.push_back("/");
        while (!q.empty()) {
            if (stop_.load()) {
                complete = false;
                break;
            }

            std::string vpath = q.front(); q.pop_front();
            std::string abspath = source_abs(source_root_, vpath);
            std::error_code ec;
            fs::directory_iterator it(abspath, ec);
            if (ec) {
                log_bootstrap_failure("enumerate", abspath, ec.message());
                complete = false;
                break;
            }

            const fs::directory_iterator end;
            while (it != end) {
                if (stop_.load()) {
                    complete = false;
                    break;
                }

                const auto& entry = *it;
                std::string name = entry.path().filename().string();
                if (!(vpath == "/" && is_reserved_root_name(name))) {
                    std::string child_v = (vpath == "/")
                        ? ("/" + name) : (vpath + "/" + name);
                    std::error_code sec;
                    fs::file_status st = entry.symlink_status(sec);
                    if (sec) {
                        log_bootstrap_failure("status", entry.path().string(),
                                              sec.message());
                        complete = false;
                        break;
                    }

                    {
                        std::lock_guard<std::mutex> checkpoint_lk(checkpoint_mu_);
                        auto existing = wt_.lookup_raw(child_v);
                        if (existing.has_value()) {
                            if (existing->kind == EntryKind::Tree &&
                                st.type() == fs::file_type::directory)
                                q.push_back(child_v);
                        } else {
                            std::string error;
                            IngestResult result = ingest_entry(
                                child_v, entry.path().string(), error);
                            if (result == IngestResult::Inserted) {
                                if (st.type() == fs::file_type::directory)
                                    q.push_back(child_v);
                            } else if (result != IngestResult::Unsupported) {
                                if (error.empty()) error = "source entry disappeared";
                                log_bootstrap_failure("ingest", entry.path().string(),
                                                      error);
                                complete = false;
                                break;
                            }
                        }
                    }
                }

                it.increment(ec);
                if (ec) {
                    log_bootstrap_failure("enumerate", abspath, ec.message());
                    complete = false;
                    break;
                }
            }
            if (!complete) break;
        }
    } catch (const std::exception& ex) {
        log_bootstrap_failure("walk", source_root_, ex.what());
        complete = false;
    }

    // Freeze point (mem-and-gc design): the fully ingested tree becomes the
    // immutable shared base so branch clones stop copying O(tree), and
    // tombstone hygiene activates (the walk is done, so a WT miss no longer
    // implies "not yet ingested from source"). Skipped on shutdown-interrupt:
    // an incomplete walk must keep falling through to the source dir.
    if (complete && !stop_.load()) {
        try {
            std::lock_guard<std::mutex> checkpoint_lk(checkpoint_mu_);
            wt_.fold_into_base();
        } catch (const std::exception& ex) {
            log_bootstrap_failure("publish", source_root_, ex.what());
            std::lock_guard<std::mutex> checkpoint_lk(checkpoint_mu_);
            wt_.begin_source_walk();
        }
    }
    pending_.store(false);
}

} // namespace cas
