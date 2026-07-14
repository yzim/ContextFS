#include "tree_serialize.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <sstream>

namespace cas {
namespace fs = std::filesystem;

std::vector<uint8_t> serialize_tree_entries(
    const std::vector<std::pair<std::string, WorkingTreeEntry>>& children) {
    auto sorted = children;
    std::sort(sorted.begin(), sorted.end(),
              [](auto& a, auto& b) { return a.first < b.first; });

    std::vector<uint8_t> buf;
    uint32_t count = sorted.size();
    buf.resize(4);
    std::memcpy(buf.data(), &count, 4);

    for (auto& [name, entry] : sorted) {
        uint16_t name_len = name.size();
        size_t pos = buf.size();
        buf.resize(pos + 2 + name_len + 4 + 1 + 32);
        std::memcpy(buf.data() + pos, &name_len, 2);
        std::memcpy(buf.data() + pos + 2, name.c_str(), name_len);
        std::memcpy(buf.data() + pos + 2 + name_len, &entry.mode, 4);
        buf[pos + 2 + name_len + 4] = static_cast<uint8_t>(entry.kind);
        std::memcpy(buf.data() + pos + 2 + name_len + 5, entry.hash.data(), 32);
    }
    return buf;
}

bool deserialize_tree_entries(
    const std::vector<uint8_t>& body,
    std::vector<std::tuple<std::string, uint32_t, EntryKind, Hash>>& out) {
    if (body.size() < 4) return false;
    uint32_t count;
    std::memcpy(&count, body.data(), 4);
    size_t pos = 4;
    out.clear();
    out.reserve(count);
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 2 > body.size()) return false;
        uint16_t name_len;
        std::memcpy(&name_len, body.data() + pos, 2);
        pos += 2;
        if (pos + name_len + 4 + 1 + 32 > body.size()) return false;
        std::string name(reinterpret_cast<const char*>(body.data() + pos), name_len);
        pos += name_len;
        uint32_t mode;
        std::memcpy(&mode, body.data() + pos, 4);
        pos += 4;
        EntryKind kind = static_cast<EntryKind>(body[pos]);
        pos += 1;
        Hash hash;
        std::memcpy(hash.data(), body.data() + pos, 32);
        pos += 32;
        out.emplace_back(std::move(name), mode, kind, hash);
    }
    return true;
}

Hash serialize_working_tree(
    const WorkingTree& wt,
    ObjectStore& store,
    std::vector<Hash>& written_hashes,
    std::vector<Hash>* referenced_leaf_hashes,
    std::string* error) {

    std::map<std::string, std::vector<std::pair<std::string, WorkingTreeEntry>>> dirs;
    wt.for_each_including_deleted([&](const std::string& path, const WorkingTreeEntry& entry) {
        if ((entry.kind == EntryKind::Blob || entry.kind == EntryKind::Symlink) &&
            entry.hash == ZERO_HASH) {
            return;
        }

        auto slash = path.rfind('/');
        std::string parent, name;
        if (slash == 0) {
            parent = "/";
            name = path.substr(1);
        } else if (slash != std::string::npos) {
            parent = path.substr(0, slash);
            name = path.substr(slash + 1);
        } else {
            parent = "/";
            name = path;
        }
        if (name.empty()) return;
        if (referenced_leaf_hashes &&
            (entry.kind == EntryKind::Blob || entry.kind == EntryKind::Symlink) &&
            entry.hash != ZERO_HASH) {
            referenced_leaf_hashes->push_back(entry.hash);
        }
        dirs[parent].push_back({name, entry});
    });

    std::map<std::string, Hash> dir_hashes;

    std::vector<std::string> dir_paths;
    for (auto& [d, _] : dirs) dir_paths.push_back(d);
    wt.for_each([&](const std::string& path, const WorkingTreeEntry& entry) {
        if (entry.kind == EntryKind::Tree) {
            if (dirs.find(path) == dirs.end()) {
                dirs[path] = {};
                dir_paths.push_back(path);
            }
        }
    });
    std::sort(dir_paths.begin(), dir_paths.end(),
              [](auto& a, auto& b) { return a.size() > b.size(); });

    for (auto& dir : dir_paths) {
        auto& children = dirs[dir];
        for (auto& [name, entry] : children) {
            if (entry.kind == EntryKind::Tree) {
                std::string child_path = (dir == "/") ? ("/" + name) : (dir + "/" + name);
                auto it = dir_hashes.find(child_path);
                if (it != dir_hashes.end()) {
                    entry.hash = it->second;
                }
            }
        }
        auto body = serialize_tree_entries(children);
        Hash h = store.write_tree(body);
        if (h == ZERO_HASH) {
            if (error) {
                std::ostringstream os;
                os << "failed to write tree object for " << dir
                   << " (" << children.size() << " entries)";
                std::string store_error = store.last_error();
                if (!store_error.empty()) os << ": " << store_error;
                *error = os.str();
            }
            return ZERO_HASH;
        }
        written_hashes.push_back(h);
        dir_hashes[dir] = h;
    }

    auto it = dir_hashes.find("/");
    if (it != dir_hashes.end()) return it->second;
    auto body = serialize_tree_entries({});
    Hash h = store.write_tree(body);
    if (h == ZERO_HASH) {
        if (error) {
            std::string store_error = store.last_error();
            *error = store_error.empty()
                ? "failed to write empty root tree object"
                : "failed to write empty root tree object: " + store_error;
        }
        return ZERO_HASH;
    }
    written_hashes.push_back(h);
    return h;
}

bool rebuild_working_tree(
    const Hash& root_tree,
    ObjectStore& store,
    WorkingTree& wt,
    std::string* error) {

    WorkingTree::EntryMap scratch;

    struct DirWork {
        Hash hash;
        std::string prefix;
    };
    std::vector<DirWork> stack;
    stack.push_back({root_tree, "/"});

    while (!stack.empty()) {
        auto [hash, prefix] = stack.back();
        stack.pop_back();

        std::vector<uint8_t> body;
        if (!store.read_tree(hash, body)) {
            if (error) {
                const std::string hex = hash_to_hex(hash);
                std::error_code status_error;
                fs::file_status status = fs::symlink_status(
                    store.object_path(hash), status_error);
                if ((!status_error && status.type() == fs::file_type::not_found) ||
                    status_error == std::errc::no_such_file_or_directory) {
                    *error = "tree object missing: " + hex;
                } else if (status_error) {
                    *error = "failed to inspect tree object: " + hex + ": " +
                             status_error.message();
                } else {
                    // The path still exists, so a wrong tag, short read, or
                    // other read failure is not evidence of GC compaction.
                    *error = "tree object unreadable: " + hex;
                }
            }
            return false;
        }

        std::vector<std::tuple<std::string, uint32_t, EntryKind, Hash>> entries;
        if (!deserialize_tree_entries(body, entries)) {
            if (error) *error = "corrupt tree object: " + hash_to_hex(hash);
            return false;
        }

        for (auto& [name, mode, kind, h] : entries) {
            std::string path = (prefix == "/") ? ("/" + name) : (prefix + "/" + name);
            scratch[path] = {kind, h, mode};
            if (kind == EntryKind::Tree) {
                stack.push_back({h, path});
            }
        }
    }

    wt.set_base(std::move(scratch));
    return true;
}

} // namespace cas
