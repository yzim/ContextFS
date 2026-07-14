#include "daemon.h"
#include "commit.h"
#include "telemetry_registry.h"
#include "tree_serialize.h"
#include "branch_name.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <utility>

namespace cas {

static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string default_merge_label(
    const std::string& source_name,
    const std::string& target_name,
    const std::string& requested_label) {
    if (!requested_label.empty()) return requested_label;
    return "merge " + source_name + " into " + target_name;
}

// VmRSS (kB) from /proc/self/status; 0 when unavailable (non-Linux or the
// field/file absent). Powers MemoryStats.rss_kb for stats.memory.
static uint64_t read_rss_kb() {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            unsigned long long kb = 0;
            if (std::sscanf(line.c_str(), "VmRSS: %llu", &kb) == 1)
                return static_cast<uint64_t>(kb);
            return 0;
        }
    }
    return 0;
}

Daemon::Daemon(std::string source_root, std::string mount_point, std::string store_root)
    : source_root_(std::move(source_root))
    , mount_point_(std::move(mount_point))
    , store_root_(std::move(store_root))
    , store_(store_root_)
    , refs_(store_root_)
    , cm_(store_, refs_)
    , agent_state_(store_)
    , runtime_supervisor_(&runtime_process_controller_) {
    session_id_ = now_ns();
    main_branch_ = std::make_shared<BranchContext>(0, "main");
    branches_[0] = main_branch_;
    branch_name_ids_["main"] = 0;
}

Daemon::~Daemon() {
    // Bootstrap owns a background thread that references main_branch_->wt.
    // Join it explicitly while every BranchContext is still alive; member
    // destruction order alone would destroy branches before bootstrap_.
    if (bootstrap_) {
        bootstrap_->stop_background();
        bootstrap_.reset();
    }
    // Defensive: even if main.cpp forgot to call shutdown_telemetry(), this
    // ensures the registry is stopped before its destructor runs (so any
    // FUSE/probe threads have quiesced) and before the rest of Daemon is
    // torn down (working tree, store, etc.) — backends may reference those
    // through callbacks that capture `daemon`.
    shutdown_telemetry();
}

void Daemon::install_registry(std::unique_ptr<TelemetryRegistry> registry) {
    // We expect this to be called once at startup, before FUSE begins
    // serving. If a registry is already installed, stop the old one first
    // to keep teardown sane.
    if (registry_) {
        registry_->stop_all();
    }
    registry_ = std::move(registry);
}

void Daemon::shutdown_telemetry() {
    if (!registry_) return;
    registry_->stop_all();
    registry_.reset();
    // The installer holds a reference into a backend the registry just
    // destroyed; null it so any later accessor sees a defined null state
    // instead of a dangling pointer.
    policy_installer_ = nullptr;
}

bool Daemon::initialize() {
    if (!store_.init_layout()) return false;
    store_.cleanup_tmp();

    Hash current;
    bool fresh_mount = !refs_.read_main(current);
    if (fresh_mount) {
        std::vector<Hash> written;
        Hash empty_tree = serialize_working_tree(main_branch_->wt, store_, written);
        if (empty_tree == ZERO_HASH) return false;
        CommitData cd;
        cd.tree_hash = empty_tree;
        cd.session_id = session_id_;
        cd.timestamp_ns = now_ns();
        cd.label = "initial";
        cd.policy_version = policy_version_.load();
        Hash commit_hash = store_.write_commit(serialize_commit(cd));
        if (commit_hash == ZERO_HASH) return false;
        std::vector<Hash> to_fsync = store_.drain_pending();
        if (!store_.fsync_objects(to_fsync)) {
            store_.restore_pending(to_fsync);
            return false;
        }
        if (!store_.fsync_shard_dirs(to_fsync)) {
            store_.restore_pending(to_fsync);
            return false;
        }
        if (!refs_.write_main(commit_hash, store_.tmp_dir())) return false;
    }

    std::string error;
    if (!fresh_mount) {
        // Restart: restore main from its ref as an authoritative base. On a fresh
        // mount, main_branch_->wt is already the empty non-authoritative tree that
        // lazy bootstrap fills and the background walk later folds authoritative
        // (mem-and-gc design). Rebuilding from the placeholder "initial" empty
        // commit would wrongly mark main authoritative with an empty base, which
        // activates tombstone hygiene before any real snapshot exists — erasing
        // real deletions of lazy-ingested content (broke branch merge reingest).
        if (!rebuild_main_from_ref(error)) {
            std::fprintf(stderr, "agentvfs: failed to restore main branch: %s\n", error.c_str());
            return false;
        }
    }

    load_persisted_branches();
    return true;
}

bool Daemon::rebuild_branch_from_ref(const std::string& name,
                                     uint32_t branch_id,
                                     std::shared_ptr<BranchContext>& out,
                                     std::string& error) {
    Hash commit_hash;
    if (!refs_.read_ref(name, commit_hash)) {
        error = "failed to read refs/" + name;
        return false;
    }

    std::vector<uint8_t> body;
    if (!store_.read_commit(commit_hash, body)) {
        error = "failed to read commit " + hash_to_hex(commit_hash);
        return false;
    }

    CommitData cd;
    if (!deserialize_commit(body, cd)) {
        error = "failed to parse commit " + hash_to_hex(commit_hash);
        return false;
    }

    WorkingTree wt;
    if (!rebuild_working_tree(cd.tree_hash, store_, wt)) {
        error = "failed to rebuild tree " + hash_to_hex(cd.tree_hash);
        return false;
    }

    out = std::make_shared<BranchContext>(branch_id, name, std::move(wt));
    return true;
}

bool Daemon::rebuild_main_from_ref(std::string& error) {
    std::shared_ptr<BranchContext> rebuilt;
    if (!rebuild_branch_from_ref("main", 0, rebuilt, error)) return false;
    // Move only the WorkingTree into the permanent main_branch_ shared_ptr;
    // the temporary rebuilt shell (and its moved-from wt) is discarded.
    main_branch_->wt = std::move(rebuilt->wt);
    return true;
}

void Daemon::load_persisted_branches() {
    std::vector<std::string> names = refs_.list_refs();

    for (const std::string& name : names) {
        if (name == "main") continue;
        if (!is_valid_branch_name(name)) {
            std::fprintf(stderr,
                         "agentvfs: warning: skipping persisted branch '%s': invalid branch name\n",
                         name.c_str());
            continue;
        }

        std::shared_ptr<BranchContext> branch;
        std::string error;
        uint32_t id = next_branch_id_.fetch_add(1);
        if (!rebuild_branch_from_ref(name, id, branch, error)) {
            std::fprintf(stderr,
                         "agentvfs: warning: skipping persisted branch '%s': %s\n",
                         name.c_str(), error.c_str());
            continue;
        }

        std::unique_lock<std::shared_mutex> lk(branches_mu_);
        if (branch_name_ids_.find(name) != branch_name_ids_.end()) {
            std::fprintf(stderr,
                         "agentvfs: warning: skipping persisted branch '%s': duplicate branch name\n",
                         name.c_str());
            continue;
        }
        branch_name_ids_[name] = id;
        branches_[id] = std::move(branch);
    }
}

uint64_t Daemon::allocate_fh(std::unique_ptr<FhState> state) {
    uint64_t fh = next_fh_id_.fetch_add(1);
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    fh_table_[fh] = std::shared_ptr<FhState>(std::move(state));
    return fh;
}

std::shared_ptr<FhState> Daemon::get_fh(uint64_t fh) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    auto it = fh_table_.find(fh);
    return (it == fh_table_.end()) ? nullptr : it->second;
}

// Must only run for FUSE RELEASE: dropping the FhState closes its retained
// blob_view fd, which in-flight fd-backed reads may still splice from after
// their callback returns (see cas_read_buf in the Linux adapter). The kernel
// guarantees RELEASE is not dispatched while a READ is in flight; daemon-side
// eviction of fh_table_ entries would break that guarantee.
void Daemon::release_fh(uint64_t fh) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    fh_table_.erase(fh);
}

int64_t Daemon::dirty_fh_size_for_path(const std::string& path, uint32_t branch_id) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    for (auto& [_, state] : fh_table_) {
        if (!state || state->stale || !state->write_buf) continue;
        if (state->path != path) continue;
        if (state->branch_id != branch_id) continue;
        if (!state->write_buf->is_dirty()) continue;
        uint64_t base = state->base_cache_loaded ? state->base_cache.size() : state->base_size;
        return (int64_t)state->write_buf->effective_size(base);
    }
    return -1;
}

void Daemon::ensure_base_cache(FhState& s) {
    if (s.base_cache_loaded) return;
    if (s.base_blob != ZERO_HASH) store_.read_blob(s.base_blob, s.base_cache);
    s.base_cache_loaded = true;
}

bool Daemon::flush_fhs_for_branch(uint32_t branch_id) {
    auto br = branch(branch_id);
    if (!br) return false;
    return flush_fhs_for_branch(branch_id, br->wt);
}

bool Daemon::flush_fhs_for_branch(uint32_t branch_id, WorkingTree& wt) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    for (auto& [_, state] : fh_table_) {
        if (state->branch_id != branch_id) continue;
        if (state->stale) continue;
        if (!state->write_buf || !state->write_buf->is_dirty()) continue;
        if (!state->base_cache_loaded) {
            if (state->base_blob != ZERO_HASH) store_.read_blob(state->base_blob, state->base_cache);
            state->base_cache_loaded = true;
        }
        auto data = state->write_buf->materialize(state->base_cache);
        Hash new_blob = store_.write_blob(data);
        if (new_blob == ZERO_HASH) return false;
        auto existing = wt.lookup(state->path);
        uint32_t mode = existing ? existing->mode : 0100644;
        wt.insert(state->path, {EntryKind::Blob, new_blob, mode});
        state->write_buf->clear();
        state->base_blob = new_blob;
        state->base_size = data.size();
        state->base_cache = std::move(data);
        state->base_cache_loaded = true;
    }
    return true;
}

void Daemon::invalidate_fhs_for_branch(uint32_t branch_id) {
    std::lock_guard<std::mutex> lk(fh_table_mu_);
    for (auto& [_, state] : fh_table_)
        if (state->branch_id == branch_id) state->stale = true;
}

std::shared_ptr<BranchContext> Daemon::branch(uint32_t id) {
    std::shared_lock<std::shared_mutex> lk(branches_mu_);
    auto it = branches_.find(id);
    return (it != branches_.end()) ? it->second : nullptr;
}

std::shared_ptr<BranchContext> Daemon::branch_by_name(const std::string& name) {
    std::shared_lock<std::shared_mutex> lk(branches_mu_);
    auto nit = branch_name_ids_.find(name);
    if (nit == branch_name_ids_.end()) return nullptr;
    auto it = branches_.find(nit->second);
    return (it != branches_.end()) ? it->second : nullptr;
}

std::shared_ptr<BranchContext> Daemon::branch_by_name_locked(
    const std::string& name,
    std::unique_lock<std::mutex>& checkpoint_lock) {

    checkpoint_lock = std::unique_lock<std::mutex>();
    std::shared_lock<std::shared_mutex> branches_lk(branches_mu_);
    auto nit = branch_name_ids_.find(name);
    if (nit == branch_name_ids_.end()) {
        return nullptr;
    }
    auto it = branches_.find(nit->second);
    if (it == branches_.end()) {
        return nullptr;
    }
    auto br = it->second;
    if (!br) {
        checkpoint_lock = std::unique_lock<std::mutex>();
        return nullptr;
    }
    // Keep the shared branch-map lock until checkpoint_mu is acquired.
    // This preserves the delete lock order: branches_mu_ -> checkpoint_mu.
    checkpoint_lock = std::unique_lock<std::mutex>(br->checkpoint_mu);
    return br;
}

AgentStateAppendResult Daemon::append_agent_state(
    const AgentStateAppendRequest& request) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    return agent_state_.append(request);
}

RollbackResult Daemon::rollback_branch_to_commit(const std::string& branch_name,
                                                 const Hash& target) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    // Same lock acquisition as the former inline `rollback` command body:
    // branch_by_name_locked takes branches_mu_ -> checkpoint_mu in the
    // documented order and hands back the held lock via checkpoint_lk.
    std::unique_lock<std::mutex> checkpoint_lk;
    auto br = branch_by_name_locked(branch_name, checkpoint_lk);
    if (!br) return {false, ZERO_HASH, "unknown branch"};

    // rollback_locked resolves the target string itself; passing the hex form
    // of the already-resolved Hash round-trips (resolve_target finds the
    // object in the store and returns it). FH invalidation runs inside
    // rollback_locked via the callback, matching the original command.
    uint32_t bid = br->branch_id;
    return cm_.rollback_locked(
        hash_to_hex(target), br->wt,
        [this, bid] { invalidate_fhs_for_branch(bid); },
        branch_name);
}

std::shared_ptr<BranchContext> Daemon::branch_for_pid(Pid pid) {
    uint32_t id = router_.resolve(pid);
    auto br = branch(id);
    return br ? br : main_branch_;
}

CheckpointResult Daemon::checkpoint_branch(
    const std::shared_ptr<BranchContext>& br,
    const std::string& label) {

    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);

    if (!br) return {false, ZERO_HASH, "unknown branch"};

    std::unique_lock<std::mutex> checkpoint_lk;
    {
        std::shared_lock<std::shared_mutex> branches_lk(branches_mu_);
        auto it = branches_.find(br->branch_id);
        if (it == branches_.end() || it->second != br) {
            return {false, ZERO_HASH, "unknown branch"};
        }
        // Hold branches_mu_ until checkpoint_mu is acquired so delete_branch()
        // cannot remove refs/<branch> between publication validation and the
        // checkpoint ref write.
        checkpoint_lk = std::unique_lock<std::mutex>(br->checkpoint_mu);
    }

    uint32_t bid = br->branch_id;
    return cm_.checkpoint_locked(
        label,
        session_id_,
        policy_version_.load(),
        br->wt,
        [this, bid, br] { return flush_fhs_for_branch(bid, br->wt); },
        br->name);
}

CheckpointResult Daemon::checkpoint_branch_by_name(
    const std::string& branch_name,
    const std::string& label) {

    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);

    std::unique_lock<std::mutex> checkpoint_lk;
    auto br = branch_by_name_locked(branch_name, checkpoint_lk);
    if (!br) return {false, ZERO_HASH, "unknown branch"};

    uint32_t bid = br->branch_id;
    return cm_.checkpoint_locked(
        label,
        session_id_,
        policy_version_.load(),
        br->wt,
        [this, bid, br] { return flush_fhs_for_branch(bid, br->wt); },
        br->name);
}

uint32_t Daemon::create_branch(const std::string& name, const std::string& from) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    std::shared_ptr<BranchContext> src_snap;
    {
        std::unique_lock<std::shared_mutex> lk(branches_mu_);
        if (branch_name_ids_.find(name) != branch_name_ids_.end()) return UINT32_MAX;
        if (branch_create_reservations_.find(name) != branch_create_reservations_.end())
            return UINT32_MAX;

        auto nit = branch_name_ids_.find(from);
        if (nit == branch_name_ids_.end()) return UINT32_MAX;
        auto it = branches_.find(nit->second);
        if (it == branches_.end()) return UINT32_MAX;

        src_snap = it->second;
        branch_create_reservations_.insert(name);
        branch_source_reservations_[from]++;
    }

    auto clear_reservations = [this, &name, &from] {
        std::unique_lock<std::shared_mutex> lk(branches_mu_);
        branch_create_reservations_.erase(name);
        auto it = branch_source_reservations_.find(from);
        if (it == branch_source_reservations_.end()) return;
        if (it->second <= 1) branch_source_reservations_.erase(it);
        else it->second--;
    };

    uint32_t id = next_branch_id_.fetch_add(1);
    Hash src_commit = ZERO_HASH;
    std::shared_ptr<BranchContext> br;
    bool source_ref_ok = false;
    {
        std::lock_guard<std::mutex> src_lk(src_snap->checkpoint_mu);
        source_ref_ok = refs_.read_ref(from, src_commit);
        if (source_ref_ok) {
            br = std::make_shared<BranchContext>(id, name, src_snap->wt.clone());
        }
    }
    // Never acquire branches_mu_ while holding checkpoint_mu. GC and branch
    // deletion use the documented branches_mu_ -> checkpoint_mu order.
    if (!source_ref_ok) {
        clear_reservations();
        return UINT32_MAX;
    }

    if (!refs_.write_ref(name, src_commit, store_.tmp_dir())) {
        clear_reservations();
        return UINT32_MAX;
    }

    {
        std::unique_lock<std::shared_mutex> lk(branches_mu_);
        branch_create_reservations_.erase(name);
        auto sit = branch_source_reservations_.find(from);
        if (sit != branch_source_reservations_.end()) {
            if (sit->second <= 1) branch_source_reservations_.erase(sit);
            else sit->second--;
        }
        if (branch_name_ids_.find(name) != branch_name_ids_.end()) {
            std::fprintf(stderr,
                         "agentvfs: create_branch invariant violated: "
                         "reserved branch '%s' was already published\n",
                         name.c_str());
            return UINT32_MAX;
        }
        branch_name_ids_[name] = id;
        branches_[id] = std::move(br);
    }
    return id;
}

BranchMergeResult Daemon::merge_branch(
    const std::string& source_name,
    const std::string& target_name,
    const std::string& label) {

    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);

    BranchMergeResult result;
    std::shared_ptr<BranchContext> source;
    std::shared_ptr<BranchContext> target;
    std::unique_lock<std::mutex> first_checkpoint_lk;
    std::unique_lock<std::mutex> second_checkpoint_lk;

    {
        std::unique_lock<std::shared_mutex> branches_lk(branches_mu_);
        for (auto& [_, br] : branches_) {
            if (br->name == source_name) source = br;
            if (br->name == target_name) target = br;
        }
        if (!source || !target) {
            result.error = "unknown branch";
            return result;
        }
        if (source->branch_id == target->branch_id) {
            result.error = "cannot merge branch into itself";
            return result;
        }

        BranchContext* first = source.get();
        BranchContext* second = target.get();
        if (second->branch_id < first->branch_id) std::swap(first, second);

        first_checkpoint_lk = std::unique_lock<std::mutex>(first->checkpoint_mu);
        second_checkpoint_lk = std::unique_lock<std::mutex>(second->checkpoint_mu);
    }

    auto source_cp = cm_.checkpoint_locked(
        "pre-merge " + source_name + " into " + target_name + " source",
        session_id_,
        policy_version_.load(),
        source->wt,
        [this, source] {
            return flush_fhs_for_branch(source->branch_id, source->wt);
        },
        source_name);
    if (!source_cp.ok) {
        result.error = source_cp.error;
        return result;
    }

    auto target_cp = cm_.checkpoint_locked(
        "pre-merge " + source_name + " into " + target_name + " target",
        session_id_,
        policy_version_.load(),
        target->wt,
        [this, target] {
            return flush_fhs_for_branch(target->branch_id, target->wt);
        },
        target_name);
    if (!target_cp.ok) {
        result.error = target_cp.error;
        return result;
    }

    std::string error;
    Hash base_commit = ZERO_HASH;
    if (!find_common_ancestor(store_, source_cp.commit_hash, target_cp.commit_hash,
                              base_commit, error)) {
        result.error = error;
        return result;
    }

    WorkingTree base_wt;
    WorkingTree source_wt;
    WorkingTree target_wt;
    CommitData base_cd;
    CommitData source_cd;
    CommitData target_cd;
    if (!load_commit_tree(store_, base_commit, base_wt, base_cd, error)) {
        result.error = error;
        return result;
    }
    if (!load_commit_tree(store_, source_cp.commit_hash, source_wt, source_cd, error)) {
        result.error = error;
        return result;
    }
    if (!load_commit_tree(store_, target_cp.commit_hash, target_wt, target_cd, error)) {
        result.error = error;
        return result;
    }

    MergeResult merge = merge_trees(base_wt, source_wt, target_wt);
    if (!merge.ok) {
        result.error = merge.error;
        result.conflicts = std::move(merge.conflicts);
        return result;
    }

    Hash merge_commit = write_commit_with_parents(
        store_,
        merge.merged,
        {target_cp.commit_hash, source_cp.commit_hash},
        session_id_,
        policy_version_.load(),
        default_merge_label(source_name, target_name, label),
        error);
    if (merge_commit == ZERO_HASH) {
        result.error = error;
        return result;
    }

    if (!refs_.write_ref(target_name, merge_commit, store_.tmp_dir())) {
        result.error = "failed to advance refs/" + target_name;
        return result;
    }

    target->wt = std::move(merge.merged);
    invalidate_fhs_for_branch(target->branch_id);

    result.ok = true;
    result.commit_hash = merge_commit;
    return result;
}

bool Daemon::delete_branch(const std::string& name) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    if (name == "main") return false;

    // Extract the branch from the map under branches_mu_. After erase, any
    // other thread holding a shared_ptr keeps the BranchContext alive —
    // the underlying memory stays valid until the last ref drops.
    std::shared_ptr<BranchContext> br;
    {
        std::unique_lock<std::shared_mutex> lk(branches_mu_);
        auto nit = branch_name_ids_.find(name);
        if (nit == branch_name_ids_.end()) return false;
        auto it = branches_.find(nit->second);
        if (it == branches_.end()) return false;
        br = it->second;
        if (!br) return false;
        if (branch_source_reservations_.find(name) != branch_source_reservations_.end())
            return false;
        if (router_.has_cgroup_for_branch(br->branch_id)) return false;

        // Take the branch's checkpoint_mu *after* branches_mu_: this
        // matches the lock order used by FUSE ops (which never hold
        // branches_mu_) so there is no inversion risk.
        std::lock_guard<std::mutex> br_lk(br->checkpoint_mu);
        if (!refs_.remove_ref(name)) return false;
        br->retired = true;
        invalidate_fhs_for_branch(br->branch_id);
        branch_name_ids_.erase(name);
        branches_.erase(br->branch_id);
    }
    // br's destructor runs here or later, once every shared_ptr holder is done.
    return true;
}

std::vector<std::shared_ptr<BranchContext>> Daemon::list_branches() {
    std::shared_lock<std::shared_mutex> lk(branches_mu_);
    std::vector<std::shared_ptr<BranchContext>> result;
    for (auto& [_, br] : branches_) result.push_back(br);
    return result;
}

MemoryStats Daemon::collect_memory_stats() {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    MemoryStats out;

    // Match the mutation/checkpoint lock order used throughout the daemon.
    // Holding every branch checkpoint lock also makes the WriteBuffer sample
    // below race-free: all platform adapters mutate their buffers under the
    // owning branch's checkpoint_mu.
    std::shared_lock<std::shared_mutex> branches_lk(branches_mu_);
    std::vector<std::unique_lock<std::mutex>> checkpoint_locks;
    checkpoint_locks.reserve(branches_.size());
    for (auto& [_, br] : branches_)
        checkpoint_locks.emplace_back(br->checkpoint_mu);

    for (auto& [_, br] : branches_) {
        WorkingTreeMemoryStats wt = br->wt.memory_stats();
        BranchMemoryStats bs;
        bs.name = br->name;
        bs.base_entries = wt.base_entries;
        bs.base_shared_by = wt.base_shared_by;
        bs.delta_entries = wt.delta_entries;
        bs.delta_tombstones = wt.delta_tombstones;
        out.branches.push_back(std::move(bs));
    }
    {
        std::lock_guard<std::mutex> lk(fh_table_mu_);
        for (auto& [fh, st] : fh_table_) {
            (void)fh;
            if (st && st->write_buf) {
                out.write_buffer_count++;
                out.write_buffer_dirty_bytes += st->write_buf->dirty_bytes();
            }
        }
    }
    out.rss_kb = read_rss_kb();
    return out;
}

// Collects every live GC root reachable from in-memory daemon state. Called
// by run_gc/verify_gc UNDER branches_mu_ (unique) + every branch
// checkpoint_mu, so the WorkingTree snapshots, fh table, pending set and
// supervisor list it reads are mutually consistent with publishing. Locks
// are taken by the caller; this function only reads.
static GcLiveRoots collect_live_roots_locked(
    const std::map<uint32_t, std::shared_ptr<BranchContext>>& branches,
    std::unordered_map<uint64_t, std::shared_ptr<FhState>>& fh_table,
    std::mutex& fh_mu, ObjectStore& store, RuntimeSupervisor& sup) {
    GcLiveRoots live;
    // Every WT entry across every branch (both base and delta layers,
    // including Deleted whiteouts — a Deleted entry's hash is ZERO_HASH and
    // is filtered below). branches is a std::map keyed by branch_id, so this
    // iteration is in ascending id order, matching the lock order.
    for (auto& [id, br] : branches) {
        (void)id;
        live.expected_branch_refs.push_back(br->name);
        br->wt.for_each_including_deleted(
            [&](const std::string&, const WorkingTreeEntry& e) {
                if ((e.kind == EntryKind::Blob || e.kind == EntryKind::Symlink) &&
                    !(e.hash == ZERO_HASH))
                    live.wt_hashes.push_back(e.hash);
            });
    }
    {
        std::lock_guard<std::mutex> lk(fh_mu);
        for (auto& [fh, st] : fh_table) {
            (void)fh;
            if (!st) continue;
            if (!(st->base_blob == ZERO_HASH))
                live.fh_blob_hashes.push_back(st->base_blob);
            if (!(st->pinned_commit == ZERO_HASH))
                live.fh_pinned_commits.push_back(st->pinned_commit);
        }
    }
    live.pending = store.pending_snapshot();
    // Live cooperative-runtime union states: RuntimeSupervisor::list() returns
    // one RuntimeStatus per runtime; each carries its templates, and the live
    // union_state_id lives on the TemplateStatus (not the RuntimeStatus).
    for (auto& rt : sup.list()) {
        for (auto& tpl : rt.templates) {
            Hash h;
            if (tpl.union_state_id.size() == 64 &&
                hex_to_hash(tpl.union_state_id.c_str(), h))
                live.runtime_union_states.push_back(h);
        }
    }
    return live;
}

GcResult Daemon::run_gc(const GcPolicy& policy) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    GcLiveRoots live;
    {
        // Existing lock order: branches_mu_ -> checkpoint_mu. These locks make
        // the live WT/FH snapshot coherent, but are deliberately released
        // before mark+sweep so FUSE mutations can continue behind the age fence.
        std::unique_lock<std::shared_mutex> blk(branches_mu_);
        std::vector<std::unique_lock<std::mutex>> checkpoint_locks;
        for (auto& [id, br] : branches_) {
            (void)id;
            checkpoint_locks.emplace_back(br->checkpoint_mu);
        }
        live = collect_live_roots_locked(
            branches_, fh_table_, fh_table_mu_, store_, runtime_supervisor_);
    }
    GcRunner gc(store_, refs_);
    return gc.run(live, policy);
}

GcResult Daemon::verify_gc(const GcPolicy& policy) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    GcLiveRoots live;
    {
        std::unique_lock<std::shared_mutex> blk(branches_mu_);
        std::vector<std::unique_lock<std::mutex>> checkpoint_locks;
        for (auto& [id, br] : branches_) {
            (void)id;
            checkpoint_locks.emplace_back(br->checkpoint_mu);
        }
        live = collect_live_roots_locked(
            branches_, fh_table_, fh_table_mu_, store_, runtime_supervisor_);
    }
    GcRunner gc(store_, refs_);
    return gc.verify(live, policy);
}

bool Daemon::is_hidden(const std::string& path) {
    return path == "/.agentvfs-store" || path.rfind("/.agentvfs-store/", 0) == 0;
}

// ---------------------------------------------------------------------------
// Daemon-coupled runtime snapshot/restore.
//
// These orchestrations run on the requester's thread (a control-socket
// handler thread) and block on the supervisor's coordination primitives
// while the cooperative runtime's own handler threads (runtime.boundary /
// runtime.template.ready / runtime.generation.ready) rendezvous through the
// same supervisor. The supervisor owns no CheckpointManager knowledge; this
// is the single place where the two meet.
// ---------------------------------------------------------------------------

RuntimeSnapshotResult Daemon::snapshot_runtime(const RuntimeSnapshotRequest& request) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    RuntimeSnapshotResult result;
    result.runtime_id = request.runtime_id;
    std::string error;

    // 1. Validate runtime, cooperativity, and branch resolvability.
    RuntimeStatus status;
    if (!runtime_supervisor_.status(request.runtime_id, status, error)) {
        result.error = error;  // "unknown runtime"
        return result;
    }
    if (!status.cooperative) {
        result.error = "runtime is not cooperative";
        return result;
    }
    if (!branch_by_name(status.branch)) {
        result.error = "unknown branch";
        return result;
    }

    // 2. Open the snapshot operation in the supervisor.
    std::string op_id;
    if (!runtime_supervisor_.begin_snapshot(request, op_id, error)) {
        result.error = error;
        return result;
    }

    // 3. Block until the cooperative runtime observes the boundary.
    std::string boundary_id;
    if (!runtime_supervisor_.wait_for_boundary(
            request.runtime_id, request.timeout_ms, boundary_id, error)) {
        // Clear the pending snapshot so this runtime is not permanently
        // snapshot-broken (begin_snapshot refuses while has_pending_snapshot),
        // and release any already-observed boundary handler with an error.
        std::string cancel_err;
        runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
        result.error = error;  // "snapshot timeout"
        return result;
    }

    // 4. Checkpoint the branch BEFORE releasing the boundary. The boundary
    //    handler stays blocked in wait_boundary_action across this checkpoint
    //    so the runtime cannot race new writes into the tree.
    Hash fs_commit = ZERO_HASH;
    std::string cp_label = "runtime:" + request.runtime_id + ":" +
                           request.boundary_kind;
    {
        std::unique_lock<std::mutex> checkpoint_lk;
        auto br = branch_by_name_locked(status.branch, checkpoint_lk);
        if (!br) {
            RuntimeBoundaryAction ignored;
            runtime_supervisor_.release_boundary_with_error(
                boundary_id, "unknown branch", ignored, error);
            result.error = "unknown branch";
            return result;
        }
        uint32_t bid = br->branch_id;
        auto cp = cm_.checkpoint_locked(
            cp_label, session_id_, policy_version_.load(), br->wt,
            [this, bid, br] { return flush_fhs_for_branch(bid, br->wt); },
            br->name);
        if (!cp.ok) {
            RuntimeBoundaryAction ignored;
            runtime_supervisor_.release_boundary_with_error(
                boundary_id, cp.error, ignored, error);
            result.error = cp.error;
            return result;
        }
        fs_commit = cp.commit_hash;
    }

    // 5. Release the boundary with action:"snapshot" (allocates the template
    //    id and unblocks the runtime.boundary handler so it can fork).
    RuntimeBoundaryAction action;
    if (!runtime_supervisor_.release_boundary_for_snapshot(boundary_id, action, error)) {
        // Defensive: if release failed the boundary handler is still parked and
        // has_pending_snapshot is still set. cancel_snapshot releases both.
        std::string cancel_err;
        runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
        result.error = error;
        return result;
    }
    const std::string template_id = action.template_id;

    // 6. Block until the parked template reports ready.
    if (!runtime_supervisor_.wait_for_template_ready(
            request.runtime_id, request.timeout_ms, error)) {
        // Template never reported ready: drop it so the registry does not
        // retain a half-prepared template forever.
        std::string drop_err;
        runtime_supervisor_.drop_template(template_id, drop_err);
        // release_boundary_for_snapshot no longer clears the pending-snapshot
        // state (snapshots stay serialized through union-state durability), so
        // clear it here on failure or the runtime would be permanently
        // snapshot-broken (begin_snapshot refuses while has_pending_snapshot).
        std::string cancel_err;
        runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
        result.error = error;  // "snapshot timeout"
        return result;
    }

    // 7-8. Build and write the union runtime state.
    TemplateStatus tmpl_st;
    std::string ts_err;
    runtime_supervisor_.template_status(template_id, tmpl_st, ts_err);

    UnionRuntimeState us;
    us.branch = status.branch;
    us.fs_commit = fs_commit;
    us.runtime_id = request.runtime_id;
    us.runtime_generation = status.generation;
    us.template_id = template_id;
    us.template_kind = "live_fork";
    us.boundary_kind = request.boundary_kind;
    us.agent_state_id = request.agent_state_id;
    us.command_ref = status.command_ref;
    us.resource_manifest_ref = "inline:cooperative-process-group";
    us.warnings.push_back({
        "process_group_descendants",
        "Only the cooperative root runtime memory is restored; descendant processes are managed as a process group for termination/status only.",
        false,
    });
    us.warnings.push_back({
        "external_resources",
        "External resources such as sockets, devices, network connections, and external service state are not inspected or restored by this first slice.",
        false,
    });
    us.timestamp_ns = now_ns();
    Hash union_hash = write_union_runtime_state(store_, us, error);
    if (union_hash == ZERO_HASH) {
        // Release the parked runtime.template.ready handler (blocked in
        // wait_template_published) with the error. Use fail_template_publish,
        // NOT drop_template: drop_template would erase the node under the
        // waiter (heap-use-after-free) and never surface an error.
        std::string fail_err;
        std::string msg = error.empty() ? "union state write failed" : error;
        runtime_supervisor_.fail_template_publish(template_id, msg, fail_err);
        // Clear pending-snapshot state (see wait_for_template_ready path).
        std::string cancel_err;
        runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
        result.error = msg;
        return result;
    }
    {
        std::string durable_error;
        if (!store_.fsync_pending({union_hash}, durable_error)) {
            std::string fail_err;
            std::string msg = durable_error.empty()
                ? "failed to fsync union runtime state"
                : durable_error;
            runtime_supervisor_.fail_template_publish(template_id, msg, fail_err);
            std::string cancel_err;
            runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
            result.error = msg;
            return result;
        }
    }

    // 9. Attach the union state to the template; this unblocks the
    //    runtime.template.ready handler.
    if (!runtime_supervisor_.attach_union_state(
            template_id, us.union_state_id, fs_commit, error)) {
        std::string fail_err;
        std::string msg = error.empty() ? "attach union state failed" : error;
        runtime_supervisor_.fail_template_publish(template_id, msg, fail_err);
        // Clear pending-snapshot state (see wait_for_template_ready path).
        std::string cancel_err;
        runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
        result.error = msg;
        return result;
    }

    // 10. Union state is durable: the snapshot is complete. NOW clear the
    //     pending-snapshot state so a subsequent begin_snapshot (a second
    //     concurrent snapshot request, or the next snapshot in a loop) is
    //     accepted. Clearing earlier (e.g. in release_boundary_for_snapshot)
    //     would let a second snapshot retarget the first's pending_template_id
    //     mid wait_for_template_ready.
    {
        std::string cancel_err;
        runtime_supervisor_.cancel_snapshot(request.runtime_id, cancel_err);
    }

    // 11. Restore eligibility is computed (not stored): live-restorable while
    //     the template process is alive at snapshot completion.
    result.ok = true;
    result.union_state_id = us.union_state_id;
    result.fs_commit = fs_commit;
    result.template_id = template_id;
    result.generation = status.generation;
    result.restore_eligibility = tmpl_st.alive
        ? RestoreEligibility::LiveRuntimeRestorable
        : RestoreEligibility::FsOnly;
    return result;
}

RuntimeRestoreResult Daemon::restore_runtime(const std::string& union_state_id_hex,
                                             uint64_t timeout_ms) {
    std::lock_guard<std::recursive_mutex> publish_lk(gc_publish_mu_);
    RuntimeRestoreResult result;
    std::string error;

    // 1. Parse the union-state id.
    Hash id;
    if (!hex_to_hash_strict(union_state_id_hex, id)) {
        result.error = "invalid union_state_id";
        return result;
    }

    // 2. Read the union state.
    UnionRuntimeState us;
    if (!read_union_runtime_state(store_, id, us, error)) {
        result.error = error;
        return result;
    }
    result.runtime_id = us.runtime_id;
    result.template_id = us.template_id;
    result.fs_commit = us.fs_commit;
    result.target_generation = us.runtime_generation + 1;

    // 3. Verify the template is still alive.
    TemplateStatus tmpl_st;
    if (!runtime_supervisor_.template_status(us.template_id, tmpl_st, error)) {
        result.error = "unknown template";
        return result;
    }
    if (!tmpl_st.alive) {
        result.error = "template is not live";
        return result;
    }

    // 4. Resolve and validate the branch before freezing the active generation.
    auto br = branch_by_name(us.branch);
    if (!br) {
        result.error = "unknown branch";
        return result;
    }
    uint32_t bid = br->branch_id;

    // 5. Record and validate the recovery commit (current branch tip before
    //    rollback). This must be non-zero/readable before we freeze anything.
    Hash recovery_commit = cm_.current_commit(us.branch);
    if (recovery_commit == ZERO_HASH) {
        result.error = "recovery commit not found";
        return result;
    }
    std::vector<uint8_t> recovery_body;
    if (!store_.read_commit(recovery_commit, recovery_body)) {
        result.error = "failed to read recovery commit";
        return result;
    }
    std::vector<uint8_t> target_body;
    if (!store_.read_commit(us.fs_commit, target_body)) {
        result.error = "failed to read target commit";
        return result;
    }
    auto recover_branch_tip = [&]() {
        std::unique_lock<std::mutex> checkpoint_lk;
        auto br_locked = branch_by_name_locked(us.branch, checkpoint_lk);
        if (!br_locked) return false;
        auto rb = cm_.rollback_locked(
            hash_to_hex(recovery_commit), br_locked->wt,
            [this, bid] { invalidate_fhs_for_branch(bid); },
            us.branch);
        return rb.ok;
    };

    // 6. Start the restore intent; this freezes the active process group and
    //    leaves the parked template alive. The template is not released yet.
    RuntimeRestoreIntent intent;
    intent.runtime_id = us.runtime_id;
    intent.template_id = us.template_id;
    intent.target_generation = us.runtime_generation + 1;
    if (!runtime_supervisor_.begin_restore(intent, error)) {
        result.error = error;
        return result;
    }

    // 7. Roll the branch back to fs_commit while the template still polls wait.
    {
        std::unique_lock<std::mutex> checkpoint_lk;
        auto br_locked = branch_by_name_locked(us.branch, checkpoint_lk);
        if (!br_locked) {
            std::string abort_err;
            runtime_supervisor_.abort_restore(us.runtime_id, abort_err);
            result.error = "unknown branch";
            return result;
        }
        auto rb = cm_.rollback_locked(
            hash_to_hex(us.fs_commit), br_locked->wt,
            [this, bid] { invalidate_fhs_for_branch(bid); },
            us.branch);
        if (!rb.ok) {
            std::string abort_err;
            runtime_supervisor_.abort_restore(us.runtime_id, abort_err);
            result.error = rb.error;
            return result;
        }
    }
    // 7. Invalidate branch file handles unconditionally (rollback_locked's
    //    invalidate_fn also covers this, but be defensive).
    invalidate_fhs_for_branch(bid);

    // 8. Filesystem rollback is complete: release the parked template's
    //    runtime.template.poll with action:"restore".
    if (!runtime_supervisor_.publish_restore(us.runtime_id, error)) {
        bool recovery_ok = recover_branch_tip();
        std::string abort_err;
        runtime_supervisor_.abort_restore(us.runtime_id, abort_err);
        if (!recovery_ok) {
            result.ok = false;
            result.partial = "recovery_failed_runtime_resumed";
            result.error = error;
            return result;
        }
        result.error = error;
        return result;
    }

    // 9. Block until the restored generation reports ready.
    if (!runtime_supervisor_.wait_for_generation_ready(
            us.runtime_id, timeout_ms, error)) {
        // Timeout recovery: roll back to the recovery commit and resume the
        // frozen active pgid.
        bool recovery_ok = recover_branch_tip();
        std::string abort_err;
        runtime_supervisor_.abort_restore(us.runtime_id, abort_err);
        if (!recovery_ok) {
            // fs was rolled back to the snapshot commit (fs_commit), recovery
            // back to the pre-restore commit failed, and abort_restore RESUMED
            // the old active process group -- it is running, not stopped.
            result.ok = false;
            result.partial = "recovery_failed_runtime_resumed";
            return result;
        }
        result.error = error;  // "restore timeout"
        return result;
    }

    // 9b. Retire the previously-active process group that generation_ready()
    //     stashed. The restored grandchild has already been acknowledged and is
    //     running, so old-generation cleanup can use forced termination without
    //     delaying the restored runtime's ready acknowledgement.
    {
        std::string retire_err;
        if (!runtime_supervisor_.retire_pending_generation(us.runtime_id, retire_err)) {
            // Unknown runtime would mean the runtime vanished mid-restore; the
            // filesystem is already restored, so report partial rather than
            // leak a frozen prior group.
            std::string abort_err;
            runtime_supervisor_.abort_restore(us.runtime_id, abort_err);
            result.ok = false;
            result.partial = "retire_unknown_prior_frozen";
            result.error = retire_err;
            return result;
        }
        // retire_pending_generation returns true (runtime known, retire record
        // consumed) even when the terminate itself failed -- it sets retire_err
        // and logs to stderr, but the restore genuinely succeeded (fs already
        // rolled back, new generation running). Surface the diagnostic here so
        // the out-param is always observed, never silently set-and-dropped; do
        // NOT flip ok to false -- only old-generation cleanup had an issue.
        if (!retire_err.empty()) {
            std::fprintf(stderr,
                         "agentvfs: restore_runtime retire warning for runtime %s: %s\n",
                         us.runtime_id.c_str(), retire_err.c_str());
        }
    }

    // 10. Success. Recompute restore eligibility from the template's state.
    result.ok = true;
    TemplateStatus tmpl_after;
    std::string after_err;
    if (runtime_supervisor_.template_status(us.template_id, tmpl_after, after_err) &&
        tmpl_after.alive) {
        result.restore_eligibility = RestoreEligibility::LiveRuntimeRestorable;
    } else {
        result.restore_eligibility = RestoreEligibility::FsOnly;
    }
    return result;
}

} // namespace cas
