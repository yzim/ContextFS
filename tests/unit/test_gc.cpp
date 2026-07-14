// GcRunner mark/retention (Task 7) and sweep/verify (Task 8 appends tests).
#include "gc.h"
#include "checkpoint.h"
#include "agent_state.h"
#include "runtime_state.h"
#include "tree_serialize.h"
#include "working_tree.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;
namespace fs = std::filesystem;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-gc-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end()); buf.push_back('\0');
    char* p = mkdtemp(buf.data()); REQUIRE(p != nullptr); return std::string(p);
}

struct GcFixture {
    std::string dir;
    ObjectStore store;
    Refs refs;
    CheckpointManager cm;
    WorkingTree wt;
    std::mutex mu;
    GcFixture() : dir(make_tmp_dir("fix")), store(dir), refs(dir), cm(store, refs) {
        REQUIRE(store.init_layout());
        wt.set_base(WorkingTree::EntryMap{});
    }
    ~GcFixture() { fs::remove_all(dir); }
    Hash put_file(const std::string& path, const std::string& content) {
        Hash h = store.write_blob(
            reinterpret_cast<const uint8_t*>(content.data()), content.size());
        wt.insert(path, {EntryKind::Blob, h, 0100644});
        return h;
    }
    Hash checkpoint(const std::string& label) {
        auto r = cm.checkpoint(label, 1, 1, mu, wt, [] { return true; });
        REQUIRE(r.ok);
        return r.commit_hash;
    }
};

// ---- helpers for building commit graphs the fixture's single-parent
// checkpoint() cannot express (merge commits, off-chain parents) ----

static std::vector<uint8_t> bytes_of(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Serialize a flat working tree of path -> blob, returning the root tree
// hash. Writes tree objects to the store and drains pending so the GC sweep
// sees them as ordinary on-disk objects.
static Hash build_tree(GcFixture& fx,
                       const std::vector<std::pair<std::string, Hash>>& files) {
    WorkingTree lwt;
    lwt.set_base(WorkingTree::EntryMap{});
    for (auto& [p, h] : files) lwt.insert(p, {EntryKind::Blob, h, 0100644});
    std::vector<Hash> written;
    std::string err;
    Hash tree = serialize_working_tree(lwt, fx.store, written, nullptr, &err);
    REQUIRE(!(tree == ZERO_HASH));
    (void)fx.store.drain_pending();
    return tree;
}

// Write a commit object directly with arbitrary parents (supports merges),
// returning its hash. Drains pending.
static Hash write_commit_raw(GcFixture& fx, const Hash& tree,
                             const std::vector<Hash>& parents,
                             const std::string& label) {
    CommitData cd;
    cd.tree_hash = tree;
    cd.parents = parents;
    cd.session_id = 1;
    cd.timestamp_ns = static_cast<uint64_t>(std::chrono::duration_cast<
        std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    cd.label = label;
    cd.policy_version = 1;
    Hash h = fx.store.write_commit(serialize_commit(cd));
    REQUIRE(!(h == ZERO_HASH));
    (void)fx.store.drain_pending();
    return h;
}

// Age every on-disk object past the GC fence so the sweep may collect
// unmarked objects (mirrors the pattern in test_rollback_to_compacted_errors).
static void age_all_objects(GcFixture& fx) {
    struct timespec ts[2];
    ts[0].tv_sec = time(nullptr) - 10; ts[0].tv_nsec = 0; ts[1] = ts[0];
    std::string err;
    REQUIRE(fx.store.for_each_object([&](const Hash& h, uint64_t, int64_t) {
        utimensat(AT_FDCWD, fx.store.object_path(h).c_str(), ts, 0);
    }, err));
}

static Hash publish_state_latest(
    GcFixture& fx, AgentStateRecord& rec,
    const std::vector<Hash>& dependencies = {}) {
    std::string err;
    Hash id = write_agent_state_record(
        fx.store, rec, dependencies, false, err);
    REQUIRE(!(id == ZERO_HASH));
    (void)fx.store.drain_pending();
    std::string latest_dir = fx.dir + "/state/latest/" + rec.agent_id;
    fs::create_directories(latest_dir);
    std::ofstream out(latest_dir + "/" + rec.branch);
    REQUIRE(out.good());
    out << hash_to_hex(id) << "\n";
    REQUIRE(out.good());
    return id;
}

// 1. Orphan detection: a blob never referenced by any commit is unmarked;
//    everything reachable from the branch head is marked.
static void test_mark_orphan_vs_reachable() {
    GcFixture fx;
    Hash live = fx.put_file("/live.txt", "live");
    Hash cp = fx.checkpoint("cp1");
    Hash orphan = fx.store.write_blob(reinterpret_cast<const uint8_t*>("orphan"), 6);
    (void)fx.store.drain_pending();               // orphan is NOT pending anymore

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(marked.count(cp) == 1);
    REQUIRE(marked.count(live) == 1);
    REQUIRE(marked.count(orphan) == 0);
}

// 2. Retention: keep_last=1 keeps the head's data; an older commit's
//    unique blob is unmarked (compacted) but its COMMIT object stays
//    marked (metadata chain preserved). keep_label pins it back.
static void test_retention_compacts_old_data() {
    GcFixture fx;
    Hash old_blob = fx.put_file("/f.txt", "version-one");
    Hash old_cp = fx.checkpoint("old");
    fx.put_file("/f.txt", "version-two");
    Hash new_cp = fx.checkpoint("new");

    GcRunner gc(fx.store, fx.refs);
    GcPolicy pol; pol.keep_last = 1;
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(gc.mark(GcLiveRoots{}, pol, marked, stats, err));
    REQUIRE(marked.count(new_cp) == 1);
    REQUIRE(marked.count(old_cp) == 1);            // metadata retained
    REQUIRE(marked.count(old_blob) == 0);          // data compacted
    REQUIRE(stats.compacted_commits == 1);

    GcPolicy pin = pol; pin.keep_labels.push_back("old");
    GcMarkSet marked2; GcResult stats2;
    REQUIRE(gc.mark(GcLiveRoots{}, pin, marked2, stats2, err));
    REQUIRE(marked2.count(old_blob) == 1);         // pinned back
    REQUIRE(stats2.compacted_commits == 0);
}

// 3. Live roots: an uncheckpointed WT blob and a pending object are marked
//    even though no commit references them.
static void test_live_roots_marked() {
    GcFixture fx;
    fx.checkpoint("base");
    Hash uncommitted = fx.store.write_blob(reinterpret_cast<const uint8_t*>("wip"), 3);
    (void)fx.store.drain_pending();
    Hash pending = fx.store.write_blob(reinterpret_cast<const uint8_t*>("pend"), 4);

    GcLiveRoots live;
    live.wt_hashes.push_back(uncommitted);
    live.pending = fx.store.pending_snapshot();
    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(gc.mark(live, GcPolicy{}, marked, stats, err));
    REQUIRE(marked.count(uncommitted) == 1);
    REQUIRE(marked.count(pending) == 1);
}

// 4. State-record closure: a state latest ref retains its record, payload,
//    parent chain, and fs_commit DATA even when keep_last would compact it.
static void test_state_records_retain_fs_commit() {
    GcFixture fx;
    Hash old_blob = fx.put_file("/g.txt", "state-linked");
    Hash old_cp = fx.checkpoint("linked");
    fx.put_file("/g.txt", "newer");
    fx.checkpoint("newer");

    Hash payload = fx.store.write_blob(reinterpret_cast<const uint8_t*>("payload"), 7);
    AgentStateRecord rec;
    rec.agent_id = "agent-a"; rec.branch = "main";
    rec.fs_commit = old_cp;
    rec.payload_ref = hash_to_hex(payload);
    rec.kind = AgentStateKind::Session;
    std::string err;
    Hash rec_id = write_agent_state_record(fx.store, rec, {payload, old_cp}, true, err);
    REQUIRE(!(rec_id == ZERO_HASH));
    // publish the latest ref exactly like AgentStateService does
    std::string latest_dir = fx.dir + "/state/latest/agent-a";
    fs::create_directories(latest_dir);
    { std::ofstream(latest_dir + "/main") << hash_to_hex(rec_id); }

    GcRunner gc(fx.store, fx.refs);
    GcPolicy pol; pol.keep_last = 1;
    GcMarkSet marked; GcResult stats;
    REQUIRE(gc.mark(GcLiveRoots{}, pol, marked, stats, err));
    REQUIRE(marked.count(rec_id) == 1);
    REQUIRE(marked.count(payload) == 1);
    REQUIRE(marked.count(old_blob) == 1);          // fs_commit data retained
    REQUIRE(stats.roots_state_refs == 1);
    // Task 7 review fix: old_cp is a branch-BFS compaction CANDIDATE under
    // keep_last=1 (not head, not in the keep_last window, label "linked" not
    // pinned), but the state record's fs_commit link retains its DATA. The
    // commit is NOT genuinely compacted, so compacted_commits must be 0 —
    // the candidate is subtracted because its data is in the retained set.
    REQUIRE(stats.compacted_commits == 0);
}

// 5. Fail closed: a state latest ref pointing at a missing record aborts
//    the mark with an error.
static void test_mark_fails_closed_on_missing_record() {
    GcFixture fx;
    fx.checkpoint("base");
    std::string latest_dir = fx.dir + "/state/latest/ghost";
    fs::create_directories(latest_dir);
    Hash bogus{}; bogus[0] = 0xbb;
    { std::ofstream(latest_dir + "/main") << hash_to_hex(bogus); }
    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(!gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(!err.empty());
}

// 6. Sweep removes orphans (old mtime), keeps marked objects, and the age
//    fence protects fresh unmarked objects.
static void test_sweep_and_age_fence() {
    GcFixture fx;
    Hash live = fx.put_file("/keep.txt", "keep");
    fx.checkpoint("cp");
    Hash orphan = fx.store.write_blob(reinterpret_cast<const uint8_t*>("orphX"), 5);
    (void)fx.store.drain_pending();
    // Age the orphan past the fence (mtime -> 10s ago).
    struct timespec ts[2];
    ts[0].tv_sec = time(nullptr) - 10; ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    REQUIRE(utimensat(AT_FDCWD, fx.store.object_path(orphan).c_str(), ts, 0) == 0);
    Hash fresh_orphan = fx.store.write_blob(reinterpret_cast<const uint8_t*>("fresh"), 5);
    (void)fx.store.drain_pending();                // fresh mtime == now

    GcRunner gc(fx.store, fx.refs);
    auto r = gc.run(GcLiveRoots{}, GcPolicy{});
    REQUIRE(r.ok);
    REQUIRE(r.swept_objects == 1);                 // aged orphan only
    REQUIRE(!fx.store.object_exists(orphan));
    REQUIRE(fx.store.object_exists(fresh_orphan)); // fence saved it
    REQUIRE(fx.store.object_exists(live));
}

// 7. dry_run reports but does not delete.
static void test_dry_run_deletes_nothing() {
    GcFixture fx;
    fx.checkpoint("cp");
    Hash orphan = fx.store.write_blob(reinterpret_cast<const uint8_t*>("orphY"), 5);
    (void)fx.store.drain_pending();
    struct timespec ts[2];
    ts[0].tv_sec = time(nullptr) - 10; ts[0].tv_nsec = 0; ts[1] = ts[0];
    REQUIRE(utimensat(AT_FDCWD, fx.store.object_path(orphan).c_str(), ts, 0) == 0);
    GcRunner gc(fx.store, fx.refs);
    GcPolicy pol; pol.dry_run = true;
    auto r = gc.run(GcLiveRoots{}, pol);
    REQUIRE(r.ok);
    REQUIRE(r.swept_objects == 1);
    REQUIRE(fx.store.object_exists(orphan));       // still there
}

// 8. verify: green after a sweep; red when a marked object is deleted
//    out from under the store.
static void test_verify() {
    GcFixture fx;
    Hash live = fx.put_file("/v.txt", "v");
    fx.checkpoint("cp");
    GcRunner gc(fx.store, fx.refs);
    auto ok = gc.verify(GcLiveRoots{}, GcPolicy{});
    REQUIRE(ok.ok);
    REQUIRE(ok.missing_objects == 0);
    REQUIRE(std::remove(fx.store.object_path(live).c_str()) == 0);
    auto bad = gc.verify(GcLiveRoots{}, GcPolicy{});
    REQUIRE(!bad.ok);
    REQUIRE(bad.missing_objects == 1);
}

// 9. Rollback to a compacted checkpoint fails with the exact retention
//    error and leaves the working tree untouched.
static void test_rollback_to_compacted_errors() {
    GcFixture fx;
    fx.put_file("/f.txt", "v1");
    fx.checkpoint("old");
    fx.put_file("/f.txt", "v2");
    fx.checkpoint("new");

    GcRunner gc(fx.store, fx.refs);
    GcPolicy pol; pol.keep_last = 1;
    // Age every object so the fence doesn't protect the compacted data.
    std::string eerr;
    struct timespec ts[2];
    ts[0].tv_sec = time(nullptr) - 10; ts[0].tv_nsec = 0; ts[1] = ts[0];
    REQUIRE(fx.store.for_each_object([&](const Hash& h, uint64_t, int64_t) {
        utimensat(AT_FDCWD, fx.store.object_path(h).c_str(), ts, 0);
    }, eerr));
    auto r = gc.run(GcLiveRoots{}, pol);
    REQUIRE(r.ok);
    REQUIRE(r.swept_objects >= 1);                 // old tree/blob compacted

    size_t before = fx.wt.size();
    auto rb = fx.cm.rollback("old", fx.mu, fx.wt, [] {});
    REQUIRE(!rb.ok);
    REQUIRE(rb.error == "checkpoint compacted by retention policy");
    REQUIRE(fx.wt.size() == before);               // untouched
    auto rb2 = fx.cm.rollback("new", fx.mu, fx.wt, [] {});   // retained target works
    REQUIRE(rb2.ok);
}

// 10. Merge-commit retention: a real two-parent merge commit retains HEAD +
//     first-parent-chain data (the merge tree's blobs) while an off-chain
//     second-parent commit's unique blob is compacted, and the merge commit's
//     metadata stays marked (verify stays green). Exercises the keep_last
//     first-parent window vs the all-parent BFS on a genuine merge graph —
//     previously the mark phase only saw linear histories.
static void test_merge_commit_retention() {
    GcFixture fx;

    Hash keep = fx.store.write_blob(bytes_of("keep"));
    Hash feat_unique = fx.store.write_blob(bytes_of("feat-only"));
    Hash main_unique = fx.store.write_blob(bytes_of("main-only"));
    (void)fx.store.drain_pending();

    // Root commit (no parents): {/keep}.
    Hash t_root = build_tree(fx, {{"/keep", keep}});
    Hash cp_root = write_commit_raw(fx, t_root, {}, "root");
    // feat side (will be the SECOND parent): {/keep, /feat}, parent = root.
    Hash t_feat = build_tree(fx, {{"/keep", keep}, {"/feat", feat_unique}});
    Hash cp_feat = write_commit_raw(fx, t_feat, {cp_root}, "feat");
    // main side (will be the FIRST parent): {/keep, /main}, parent = root.
    Hash t_main = build_tree(fx, {{"/keep", keep}, {"/main", main_unique}});
    Hash cp_main = write_commit_raw(fx, t_main, {cp_root}, "main2");
    // Merge head: {/keep, /main} — drops /feat, so feat_unique is reachable
    // ONLY through the off-chain second parent. Two parents: [cp_main, cp_feat].
    Hash t_merge = build_tree(fx, {{"/keep", keep}, {"/main", main_unique}});
    Hash cp_merge = write_commit_raw(fx, t_merge, {cp_main, cp_feat}, "merge");

    REQUIRE(fx.refs.write_ref("main", cp_merge, fx.store.tmp_dir()));
    age_all_objects(fx);

    GcRunner gc(fx.store, fx.refs);
    GcPolicy pol; pol.keep_last = 1;
    auto r = gc.run(GcLiveRoots{}, pol);
    REQUIRE(r.ok);
    // HEAD + first-parent-chain DATA retained (merge tree's blobs survive).
    REQUIRE(fx.store.object_exists(keep));
    REQUIRE(fx.store.object_exists(main_unique));
    // Merge-commit METADATA retained (commit objects are always marked).
    REQUIRE(fx.store.object_exists(cp_merge));
    // Off-chain second parent's unique blob compacted away.
    REQUIRE(!fx.store.object_exists(feat_unique));
    REQUIRE(r.swept_objects >= 1);

    // verify stays green: every object the mark phase references still exists.
    auto v = gc.verify(GcLiveRoots{}, pol);
    REQUIRE(v.ok);
    REQUIRE(v.missing_objects == 0);
}

// 11. Fail closed (second path): a missing TREE object referenced by a
//     reachable commit's data chain aborts mark with a non-empty error and
//     no sweep runs. Previously only the missing state-RECORD fail-closed
//     path (test 5) was covered; this guards mark_tree_recursive.
static void test_mark_fails_closed_on_missing_tree() {
    GcFixture fx;
    fx.put_file("/a.txt", "aaa");
    Hash cp = fx.checkpoint("cp");              // main ref -> cp
    (void)fx.store.drain_pending();

    // Resolve the commit's tree, then unlink the tree object out from under
    // the reachable commit. mark must detect this and fail closed.
    std::vector<uint8_t> body;
    REQUIRE(fx.store.read_commit(cp, body));
    CommitData cd;
    REQUIRE(deserialize_commit(body, cd));
    REQUIRE(std::remove(fx.store.object_path(cd.tree_hash).c_str()) == 0);

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(!gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(!err.empty());
}

// A broken refs directory must abort mark. Treating an enumeration error as
// an empty branch-root set would make a subsequent run sweep live history.
static void test_mark_fails_closed_on_refs_enumeration_error() {
    GcFixture fx;
    fx.put_file("/live.txt", "live");
    fx.checkpoint("cp");
    fs::rename(fx.dir + "/refs", fx.dir + "/refs.saved");
    { std::ofstream out(fx.dir + "/refs"); out << "not a directory\n"; }

    std::vector<std::string> names{"partial"};
    std::string list_error;
    REQUIRE(!fx.refs.list_refs(names, list_error));
    REQUIRE(names.empty());
    REQUIRE(!list_error.empty());

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(!gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(err.find("refs") != std::string::npos);
}

static void test_mark_fails_closed_on_malformed_ref_suffix() {
    GcFixture fx;
    fx.put_file("/live.txt", "live");
    fx.checkpoint("cp");
    {
        std::ofstream out(fx.dir + "/refs/main", std::ios::app);
        REQUIRE(out.good());
        out << "trailing";
    }

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(!gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(err.find("unreadable ref: main") != std::string::npos);
}

// state/latest is optional when absent, but if it exists and cannot be
// enumerated the marker must fail closed instead of silently losing roots.
static void test_mark_fails_closed_on_state_enumeration_error() {
    GcFixture fx;
    fx.checkpoint("cp");
    fs::create_directories(fx.dir + "/state");
    { std::ofstream out(fx.dir + "/state/latest"); out << "not a directory\n"; }

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(!gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(err.find("state/latest") != std::string::npos);
}

// A state-only fs_commit is a commit-graph root, not just one retained tree.
// With compaction disabled, every parent commit and its data must survive.
static void test_state_fs_commit_traverses_parent_chain_without_compaction() {
    GcFixture fx;
    Hash parent_blob = fx.store.write_blob(bytes_of("parent-data"));
    Hash child_blob = fx.store.write_blob(bytes_of("child-data"));
    (void)fx.store.drain_pending();
    Hash parent_tree = build_tree(fx, {{"/parent", parent_blob}});
    Hash parent = write_commit_raw(fx, parent_tree, {}, "parent");
    Hash child_tree = build_tree(fx, {{"/child", child_blob}});
    Hash child = write_commit_raw(fx, child_tree, {parent}, "child");

    AgentStateRecord rec;
    rec.agent_id = "state-root";
    rec.branch = "main";
    rec.fs_commit = child;
    publish_state_latest(fx, rec);

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(marked.count(child) == 1);
    REQUIRE(marked.count(child_blob) == 1);
    REQUIRE(marked.count(parent) == 1);
    REQUIRE(marked.count(parent_blob) == 1);
}

// Under compaction, the exact state-linked snapshot stays retained while its
// parent metadata remains traversable and older data follows keep_last.
static void test_state_fs_commit_compacts_parent_data_only() {
    GcFixture fx;
    Hash grandparent_blob = fx.store.write_blob(bytes_of("state-grandparent"));
    Hash parent_blob = fx.store.write_blob(bytes_of("state-parent"));
    Hash child_blob = fx.store.write_blob(bytes_of("state-child"));
    (void)fx.store.drain_pending();
    Hash grandparent_tree = build_tree(fx, {{"/grandparent", grandparent_blob}});
    Hash grandparent = write_commit_raw(fx, grandparent_tree, {}, "grandparent");
    Hash parent_tree = build_tree(fx, {{"/parent", parent_blob}});
    Hash parent = write_commit_raw(fx, parent_tree, {grandparent}, "parent");
    Hash child_tree = build_tree(fx, {{"/child", child_blob}});
    Hash child = write_commit_raw(fx, child_tree, {parent}, "child");

    AgentStateRecord rec;
    rec.agent_id = "state-compact";
    rec.branch = "main";
    rec.fs_commit = child;
    publish_state_latest(fx, rec);

    GcPolicy policy;
    // keep_last is a branch-head window, not a window rooted at every state
    // snapshot. N > 1 catches accidental over-retention of state ancestors.
    policy.keep_last = 3;
    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(gc.mark(GcLiveRoots{}, policy, marked, stats, err));
    REQUIRE(marked.count(child) == 1);
    REQUIRE(marked.count(child_blob) == 1);
    REQUIRE(marked.count(parent) == 1);
    REQUIRE(marked.count(parent_blob) == 0);
    REQUIRE(marked.count(grandparent) == 1);
    REQUIRE(marked.count(grandparent_blob) == 0);
    REQUIRE(stats.compacted_commits == 2);
}

// Real union records use opaque command/resource values such as argv:/ and
// inline:. Only strict 64-hex values are CAS dependencies. The union's
// fs_commit still roots the complete parent graph when compaction is off.
static void test_union_opaque_refs_and_fs_commit_parent_chain() {
    GcFixture fx;
    Hash parent_blob = fx.store.write_blob(bytes_of("union-parent"));
    Hash child_blob = fx.store.write_blob(bytes_of("union-child"));
    Hash command_blob = fx.store.write_blob(bytes_of("command-object"));
    (void)fx.store.drain_pending();
    Hash parent_tree = build_tree(fx, {{"/parent", parent_blob}});
    Hash parent = write_commit_raw(fx, parent_tree, {}, "parent");
    Hash child_tree = build_tree(fx, {{"/child", child_blob}});
    Hash child = write_commit_raw(fx, child_tree, {parent}, "child");

    UnionRuntimeState us;
    us.fs_commit = child;
    us.command_ref = hash_to_hex(command_blob);
    us.resource_manifest_ref = "inline:cooperative-process-group";
    std::string err;
    Hash union_id = write_union_runtime_state(fx.store, us, err);
    REQUIRE(!(union_id == ZERO_HASH));
    (void)fx.store.drain_pending();

    GcLiveRoots live;
    live.runtime_union_states.push_back(union_id);
    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats;
    REQUIRE(gc.mark(live, GcPolicy{}, marked, stats, err));
    REQUIRE(marked.count(union_id) == 1);
    REQUIRE(marked.count(command_blob) == 1);
    REQUIRE(marked.count(child_blob) == 1);
    REQUIRE(marked.count(parent) == 1);
    REQUIRE(marked.count(parent_blob) == 1);

    GcPolicy compact;
    compact.keep_last = 3;
    GcMarkSet compact_marked; GcResult compact_stats;
    REQUIRE(gc.mark(live, compact, compact_marked, compact_stats, err));
    REQUIRE(compact_marked.count(command_blob) == 1);
    REQUIRE(compact_marked.count(child_blob) == 1);
    REQUIRE(compact_marked.count(parent) == 1);
    REQUIRE(compact_marked.count(parent_blob) == 0);
    REQUIRE(compact_stats.compacted_commits == 1);
}

// Existing-but-unreadable/corrupt tree data is not evidence that retention
// compacted the checkpoint. Only a genuinely absent tree gets that message.
static void test_rollback_corrupt_tree_is_not_reported_as_compacted() {
    GcFixture fx;
    fx.put_file("/f.txt", "data");
    Hash cp = fx.checkpoint("corrupt");
    std::vector<uint8_t> body;
    REQUIRE(fx.store.read_commit(cp, body));
    CommitData cd;
    REQUIRE(deserialize_commit(body, cd));
    std::string path = fx.store.object_path(cd.tree_hash);
    REQUIRE(chmod(path.c_str(), 0644) == 0);
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC);
    REQUIRE(fd >= 0);
    REQUIRE(write(fd, "bad", 3) == 3);
    REQUIRE(close(fd) == 0);

    auto rb = fx.cm.rollback("corrupt", fx.mu, fx.wt, [] {});
    REQUIRE(!rb.ok);
    REQUIRE(rb.error != "checkpoint compacted by retention policy");
    REQUIRE(rb.error.find("failed to rebuild working tree") != std::string::npos);
}

static void test_state_explicit_dependencies_are_marked() {
    GcFixture fx;
    Hash dependency = fx.store.write_blob(bytes_of("explicit-dependency"));
    (void)fx.store.drain_pending();

    AgentStateRecord rec;
    rec.agent_id = "dependency-root";
    rec.branch = "main";
    Hash state_id = publish_state_latest(fx, rec, {dependency});

    GcRunner gc(fx.store, fx.refs);
    GcMarkSet marked; GcResult stats; std::string err;
    REQUIRE(gc.mark(GcLiveRoots{}, GcPolicy{}, marked, stats, err));
    REQUIRE(marked.count(state_id) == 1);
    REQUIRE(marked.count(dependency) == 1);
}

// Removal failures are surfaced, and real-sweep counters include only files
// that the synchronized ObjectStore primitive actually removed.
static void test_sweep_reports_removal_errors_without_false_counts() {
    GcFixture fx;
    Hash bad{};
    bad[0] = 0xde;
    std::string shard = fx.dir + "/objects/" + hash_to_hex(bad).substr(0, 2);
    REQUIRE(mkdir(shard.c_str(), 0755) == 0);
    std::string path = fx.store.object_path(bad);
    REQUIRE(mkdir(path.c_str(), 0755) == 0);
    { std::ofstream child(path + "/child"); child << "non-empty"; }
    struct timespec old[2]{};
    old[0].tv_sec = time(nullptr) - 10;
    old[1] = old[0];
    REQUIRE(utimensat(AT_FDCWD, path.c_str(), old, 0) == 0);

    GcRunner gc(fx.store, fx.refs);
    GcResult result = gc.run(GcLiveRoots{}, GcPolicy{});
    REQUIRE(!result.ok);
    REQUIRE(result.sweep_errors == 1);
    REQUIRE(result.swept_objects == 0);
    REQUIRE(result.swept_bytes == 0);
    REQUIRE(!result.error.empty());
    REQUIRE(fx.store.object_exists(bad));
}

int main() {
    test_mark_orphan_vs_reachable();
    test_retention_compacts_old_data();
    test_live_roots_marked();
    test_state_records_retain_fs_commit();
    test_mark_fails_closed_on_missing_record();
    test_sweep_and_age_fence();
    test_dry_run_deletes_nothing();
    test_verify();
    test_rollback_to_compacted_errors();
    test_merge_commit_retention();
    test_mark_fails_closed_on_missing_tree();
    test_mark_fails_closed_on_refs_enumeration_error();
    test_mark_fails_closed_on_malformed_ref_suffix();
    test_mark_fails_closed_on_state_enumeration_error();
    test_state_fs_commit_traverses_parent_chain_without_compaction();
    test_state_fs_commit_compacts_parent_data_only();
    test_union_opaque_refs_and_fs_commit_parent_chain();
    test_rollback_corrupt_tree_is_not_reported_as_compacted();
    test_state_explicit_dependencies_are_marked();
    test_sweep_reports_removal_errors_without_false_counts();
    std::printf("test_gc: PASS\n");
    return 0;
}
