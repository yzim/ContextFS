#include "working_tree.h"
#include <algorithm>

namespace cas {

std::optional<WorkingTreeEntry> WorkingTree::lookup_raw_locked(const std::string& path) const {
    // Delta fast path (spec rider 1): skip the probe when the overlay is
    // empty so the common post-freeze lookup pays one map find, as before.
    if (!delta_.empty()) {
        auto it = delta_.find(path);
        if (it != delta_.end()) return it->second;
    }
    if (base_) {
        auto it = base_->find(path);
        if (it != base_->end()) return it->second;
    }
    return std::nullopt;
}

std::optional<WorkingTreeEntry> WorkingTree::lookup(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto e = lookup_raw_locked(path);
    if (!e.has_value() || e->kind == EntryKind::Deleted) return std::nullopt;
    return e;
}

std::optional<WorkingTreeEntry> WorkingTree::lookup_raw(const std::string& path) const {
    std::lock_guard<std::mutex> lk(mu_);
    return lookup_raw_locked(path);
}

void WorkingTree::insert(const std::string& path, const WorkingTreeEntry& entry) {
    std::lock_guard<std::mutex> lk(mu_);
    delta_[path] = entry;
}

void WorkingTree::insert_source(const std::string& path,
                                const WorkingTreeEntry& entry) {
    std::lock_guard<std::mutex> lk(mu_);
    delta_[path] = entry;
    // Before authority, remove() already whiteouts every path. Retain an
    // explicit marker only when it is needed to override post-authority
    // tombstone hygiene.
    if (base_authoritative_) source_origin_paths_.insert(path);
}

void WorkingTree::remove_locked(const std::string& path) {
    // Tombstone hygiene: once the base is authoritative, a path absent from
    // both the base and live source needs no whiteout. Source-origin markers
    // cover paths lazily discovered after the base was published.
    if (base_authoritative_) {
        bool in_base = false;
        if (base_) {
            auto bit = base_->find(path);
            in_base = (bit != base_->end() && bit->second.kind != EntryKind::Deleted);
        }
        if (!in_base && source_origin_paths_.count(path) == 0) {
            delta_.erase(path);
            return;
        }
    }
    delta_[path] = {EntryKind::Deleted, ZERO_HASH, 0};
}

void WorkingTree::remove(const std::string& path) {
    std::lock_guard<std::mutex> lk(mu_);
    remove_locked(path);
}

void WorkingTree::rename_entry(const std::string& old_path, const std::string& new_path) {
    std::lock_guard<std::mutex> lk(mu_);
    auto e = lookup_raw_locked(old_path);
    if (!e.has_value()) return;
    WorkingTreeEntry entry = *e;
    remove_locked(old_path);
    if (entry.kind == EntryKind::Deleted) {
        // Moving a whiteout: route the destination through remove_locked so a
        // non-base path is hygienically erased (no spurious tombstone) while a
        // base path still receives its whiteout. Mirrors rename_dir.
        remove_locked(new_path);
    } else {
        delta_[new_path] = entry;
    }
}

void WorkingTree::rename_dir(const std::string& old_prefix, const std::string& new_prefix) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string scan = old_prefix;
    if (!scan.empty() && scan.back() != '/') scan += '/';

    // Collect the merged raw view under the prefix (delta wins), matching
    // the old implementation's to_move semantics (Deleted entries move too).
    std::map<std::string, WorkingTreeEntry> to_move;
    if (base_) {
        for (auto it = base_->lower_bound(scan); it != base_->end(); ++it) {
            if (it->first.compare(0, scan.size(), scan) != 0) break;
            to_move[it->first] = it->second;
        }
    }
    for (auto it = delta_.lower_bound(scan); it != delta_.end(); ++it) {
        if (it->first.compare(0, scan.size(), scan) != 0) break;
        to_move[it->first] = it->second;
    }
    auto dir_e = lookup_raw_locked(old_prefix);
    if (dir_e.has_value()) to_move[old_prefix] = *dir_e;

    for (auto& [path, entry] : to_move) {
        remove_locked(path);
        std::string new_path = (path == old_prefix)
            ? new_prefix
            : new_prefix + path.substr(old_prefix.size());
        if (entry.kind == EntryKind::Deleted) {
            // Moving a whiteout: it only matters if the destination path
            // exists in an authoritative base; remove_locked applies the
            // same hygiene decision for us.
            remove_locked(new_path);
        } else {
            delta_[new_path] = entry;
        }
    }
}

void WorkingTree::merged_for_each_locked(
    const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const {
    auto bit = base_ ? base_->begin() : EntryMap::const_iterator{};
    auto bend = base_ ? base_->end() : EntryMap::const_iterator{};
    auto dit = delta_.begin();
    auto dend = delta_.end();
    while ((base_ && bit != bend) || dit != dend) {
        if (!base_ || bit == bend) { fn(dit->first, dit->second); ++dit; }
        else if (dit == dend)      { fn(bit->first, bit->second); ++bit; }
        else if (dit->first < bit->first)  { fn(dit->first, dit->second); ++dit; }
        else if (bit->first < dit->first)  { fn(bit->first, bit->second); ++bit; }
        else { fn(dit->first, dit->second); ++dit; ++bit; }   // delta wins
    }
}

std::vector<std::pair<std::string, WorkingTreeEntry>> WorkingTree::list_dir(const std::string& dir_path) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::pair<std::string, WorkingTreeEntry>> result;
    std::string prefix = dir_path;
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';

    auto emit = [&](const std::string& path, const WorkingTreeEntry& e) {
        if (e.kind == EntryKind::Deleted) return;
        auto rest = path.substr(prefix.size());
        if (rest.find('/') != std::string::npos) return;   // direct children only
        result.push_back({path, e});
    };

    // Two-pointer merge restricted to the prefix range of each layer.
    auto bit = base_ ? base_->lower_bound(prefix) : EntryMap::const_iterator{};
    auto bend = base_ ? base_->end() : EntryMap::const_iterator{};
    auto in_prefix = [&](EntryMap::const_iterator it, EntryMap::const_iterator end) {
        return it != end && it->first.compare(0, prefix.size(), prefix) == 0;
    };
    auto dit = delta_.lower_bound(prefix);
    auto dend = delta_.end();
    while (in_prefix(dit, dend) || (base_ && in_prefix(bit, bend))) {
        bool d_ok = in_prefix(dit, dend);
        bool b_ok = base_ && in_prefix(bit, bend);
        if (d_ok && (!b_ok || dit->first < bit->first)) { emit(dit->first, dit->second); ++dit; }
        else if (b_ok && (!d_ok || bit->first < dit->first)) { emit(bit->first, bit->second); ++bit; }
        else { emit(dit->first, dit->second); ++dit; ++bit; }   // equal key: delta wins
    }
    return result;
}

void WorkingTree::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    base_.reset();
    delta_.clear();
    source_origin_paths_.clear();
    base_authoritative_ = false;
}

void WorkingTree::for_each(const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    merged_for_each_locked([&](const std::string& p, const WorkingTreeEntry& e) {
        if (e.kind != EntryKind::Deleted) fn(p, e);
    });
}

void WorkingTree::for_each_including_deleted(
    const std::function<void(const std::string&, const WorkingTreeEntry&)>& fn) const {
    std::lock_guard<std::mutex> lk(mu_);
    merged_for_each_locked(fn);
}

size_t WorkingTree::size() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t count = 0;
    merged_for_each_locked([&](const std::string&, const WorkingTreeEntry& e) {
        if (e.kind != EntryKind::Deleted) count++;
    });
    return count;
}

WorkingTree::WorkingTree(WorkingTree&& other) noexcept {
    std::lock_guard<std::mutex> lk(other.mu_);
    base_ = std::move(other.base_);
    delta_ = std::move(other.delta_);
    source_origin_paths_ = std::move(other.source_origin_paths_);
    base_authoritative_ = other.base_authoritative_;
}

WorkingTree& WorkingTree::operator=(WorkingTree&& other) noexcept {
    if (this == &other) return *this;
    std::lock(mu_, other.mu_);
    std::lock_guard<std::mutex> lk1(mu_, std::adopt_lock);
    std::lock_guard<std::mutex> lk2(other.mu_, std::adopt_lock);
    base_ = std::move(other.base_);
    delta_ = std::move(other.delta_);
    source_origin_paths_ = std::move(other.source_origin_paths_);
    base_authoritative_ = other.base_authoritative_;
    return *this;
}

WorkingTree WorkingTree::clone() const {
    std::lock_guard<std::mutex> lk(mu_);
    WorkingTree copy;
    copy.base_ = base_;                    // shared, not copied
    copy.delta_ = delta_;
    copy.source_origin_paths_ = source_origin_paths_;
    copy.base_authoritative_ = base_authoritative_;
    return copy;
}

void WorkingTree::set_base(EntryMap&& entries) {
    std::lock_guard<std::mutex> lk(mu_);
    base_ = std::make_shared<const EntryMap>(std::move(entries));
    delta_.clear();
    source_origin_paths_.clear();
    base_authoritative_ = true;
}

void WorkingTree::fold_into_base() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!base_ || base_->empty()) {
        base_ = std::make_shared<const EntryMap>(std::move(delta_));
    } else {
        EntryMap merged(*base_);
        for (auto& [k, v] : delta_) merged[k] = v;
        base_ = std::make_shared<const EntryMap>(std::move(merged));
    }
    delta_ = EntryMap{};
    source_origin_paths_.clear();
    base_authoritative_ = true;
}

void WorkingTree::begin_source_walk() {
    std::lock_guard<std::mutex> lk(mu_);
    base_authoritative_ = false;
}

bool WorkingTree::base_authoritative() const {
    std::lock_guard<std::mutex> lk(mu_);
    return base_authoritative_;
}

WorkingTreeMemoryStats WorkingTree::memory_stats() const {
    std::lock_guard<std::mutex> lk(mu_);
    WorkingTreeMemoryStats stats;
    stats.base_entries = base_ ? base_->size() : 0;
    stats.base_shared_by = base_ ? static_cast<int64_t>(base_.use_count()) : 0;
    stats.delta_entries = delta_.size();
    for (const auto& [_, entry] : delta_) {
        if (entry.kind == EntryKind::Deleted) ++stats.delta_tombstones;
    }
    return stats;
}

size_t WorkingTree::base_entry_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return base_ ? base_->size() : 0;
}

long WorkingTree::base_shared_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return base_ ? static_cast<long>(base_.use_count()) : 0;
}

size_t WorkingTree::delta_entry_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return delta_.size();
}

size_t WorkingTree::delta_tombstone_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t count = 0;
    for (const auto& [_, entry] : delta_) {
        if (entry.kind == EntryKind::Deleted) ++count;
    }
    return count;
}

} // namespace cas
