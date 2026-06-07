#include "bootstrap.h"
#include <deque>
#include <filesystem>
#include <fstream>
#include <vector>

namespace cas {

namespace fs = std::filesystem;

static bool is_store_dir(const std::string& name) { return name == ".agentvfs-store"; }

static std::string source_abs(const std::string& root, const std::string& vpath) {
    return (vpath == "/") ? root : root + vpath;
}

static bool is_deleted(const std::optional<WorkingTreeEntry>& entry) {
    return entry.has_value() && entry->kind == EntryKind::Deleted;
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
                     WorkingTree& wt, InodeMap& inode_map)
    : source_root_(std::move(source_root))
    , store_(store), wt_(wt), inode_map_(inode_map) {}

Bootstrap::~Bootstrap() { stop_background(); }

void Bootstrap::ingest_entry(const std::string& vpath,
                             const std::string& source_abspath) {
    std::error_code ec;
    fs::file_status st = fs::symlink_status(source_abspath, ec);
    if (ec) return;
    uint32_t mode = encode_mode(st.type(), st.permissions());
    // NOTE: no inode_map_.set() here. Source-side (dev, ino) keys are never
    // consulted: the BPF probe reads dev/ino from the FUSE-delivered inode
    // (mount-side), and policy.install populates inode_map_ via a mount-path
    // lstat. Inserting source-side keys would be dead state.

    if (st.type() == fs::file_type::directory) {
        wt_.insert(vpath, {EntryKind::Tree, ZERO_HASH, mode});
    } else if (st.type() == fs::file_type::symlink) {
        auto target = fs::read_symlink(source_abspath, ec);
        std::string target_str = ec ? std::string() : target.string();
        Hash h = store_.write_blob(
            reinterpret_cast<const uint8_t*>(target_str.data()),
            target_str.size());
        if (h == ZERO_HASH) return;
        wt_.insert(vpath, {EntryKind::Symlink, h, mode});
    } else if (st.type() == fs::file_type::regular) {
        std::uintmax_t size = fs::file_size(source_abspath, ec);
        if (ec) return;
        std::ifstream in(source_abspath, std::ios::binary);
        if (!in) return;
        std::vector<uint8_t> data(static_cast<size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(size));
        data.resize(static_cast<size_t>(in.gcount()));
        Hash h = store_.write_blob(data);
        if (h == ZERO_HASH) return;
        wt_.insert(vpath, {EntryKind::Blob, h, mode});
    }
}

bool Bootstrap::ensure_path(const std::string& vpath) {
    if (vpath == "/" || vpath.empty()) return true;

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
        std::string src = source_abs(source_root_, *it);
        std::error_code ec;
        auto st = fs::symlink_status(src, ec);
        if (ec) return false;
        auto last_slash = it->rfind('/');
        std::string base = (last_slash == std::string::npos) ? *it : it->substr(last_slash + 1);
        if (is_store_dir(base)) return false;
        ingest_entry(*it, src);
    }
    return true;
}

void Bootstrap::start_background() {
    bg_ = std::thread([this] { walk_bg(); });
}

void Bootstrap::stop_background() {
    stop_ = true;
    if (bg_.joinable()) bg_.join();
}

void Bootstrap::walk_bg() {
    std::deque<std::string> q;
    q.push_back("/");
    while (!q.empty() && !stop_.load()) {
        std::string vpath = q.front(); q.pop_front();
        std::string abspath = source_abs(source_root_, vpath);
        std::error_code ec;
        // directory_iterator already skips "." and ".." — no manual filter
        // needed. The error_code overload prevents exceptions from a
        // missing/permission-denied directory escaping the bg thread.
        for (auto it = fs::directory_iterator(abspath, ec);
             !ec && it != fs::directory_iterator() && !stop_.load();
             it.increment(ec))
        {
            const auto& entry = *it;
            std::string name = entry.path().filename().string();
            if (vpath == "/" && is_store_dir(name)) continue;
            std::string child_v = (vpath == "/") ? ("/" + name) : (vpath + "/" + name);
            auto existing = wt_.lookup_raw(child_v);
            if (existing.has_value()) {
                std::error_code dec;
                if (existing->kind == EntryKind::Tree && entry.is_directory(dec))
                    q.push_back(child_v);
                continue;
            }
            std::error_code sec;
            auto st = entry.symlink_status(sec);
            if (sec) continue;
            ingest_entry(child_v, entry.path().string());
            if (st.type() == fs::file_type::directory) q.push_back(child_v);
        }
    }
    pending_.store(false);
}

} // namespace cas
