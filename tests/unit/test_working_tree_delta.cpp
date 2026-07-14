// Parity + hygiene tests for the delta-over-base WorkingTree
// (2026-07-13 mem-and-gc design, Task 1). Style: no framework, REQUIRE
// macro, main() runs every test (same as test_working_tree.cpp).
#include "working_tree.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static WorkingTreeEntry blob(uint8_t seed) {
    Hash h{}; h[0] = seed; h[1] = 0x5a;
    return {EntryKind::Blob, h, 0100644};
}
static WorkingTreeEntry tree_entry() {
    return {EntryKind::Tree, ZERO_HASH, 0040755};
}

// Reference model: mirrors the user-visible AND raw surface of WorkingTree
// under both regimes. `authoritative` + `base_members` reproduce tombstone
// hygiene: under authority, removing a path the base does not (visibly)
// contain ERASES the entry (no whiteout), exactly as
// WorkingTree::remove_locked does. Pre-authority (authoritative=false, the
// default) remove always tombstones — the old flat-map behavior — so test 1
// (which never calls set_base) keeps its bit-identical raw-surface check.
//
// NOTE: renaming a hygienically-erased (non-base, removed) path is a
// deliberate, spec-sanctioned divergence from the old flat-map. The source
// is raw-absent, so rename_entry is a NO-OP here (and in the impl's
// lookup_raw miss path) and the destination SURVIVES. The old flat-map
// propagated the tombstone and clobbered a live destination; that no longer
// holds post-authority. (The brief's rename_entry-parity note covered
// rename_dir but was inaccurate for this erase-then-rename interaction.)
struct RefModel {
    std::map<std::string, WorkingTreeEntry> m;
    bool authoritative = false;
    std::set<std::string> base_members;

    void insert(const std::string& p, const WorkingTreeEntry& e) { m[p] = e; }
    void mark_base(const std::string& p) { base_members.insert(p); }
    void remove(const std::string& p) {
        if (authoritative && !base_members.count(p))
            m.erase(p);                       // hygiene: non-base path leaves no tombstone
        else
            m[p] = {EntryKind::Deleted, ZERO_HASH, 0};
    }
    void rename_entry(const std::string& o, const std::string& n) {
        auto it = m.find(o);
        if (it == m.end()) return;            // source raw-absent: no-op (key hygiene fix)
        WorkingTreeEntry e = it->second;
        remove(o);                            // apply the same hygiene to the source
        m[n] = e;                             // destination takes the captured entry
    }
    bool lookup(const std::string& p, WorkingTreeEntry& out) const {
        auto it = m.find(p);
        if (it == m.end() || it->second.kind == EntryKind::Deleted) return false;
        out = it->second; return true;
    }
    size_t size() const {
        size_t c = 0;
        for (auto& [k, v] : m) if (v.kind != EntryKind::Deleted) c++;
        return c;
    }
    std::vector<std::string> visible() const {
        std::vector<std::string> out;
        for (auto& [k, v] : m) if (v.kind != EntryKind::Deleted) out.push_back(k);
        return out;
    }
};

static std::vector<std::string> visible_of(const WorkingTree& wt) {
    std::vector<std::string> out;
    wt.for_each([&](const std::string& p, const WorkingTreeEntry&) { out.push_back(p); });
    return out;
}

// 1. Randomized parity: user-visible surface identical to the reference
//    model across insert/remove/rename sequences, pre-authority (hygiene
//    off) — behavior must be bit-identical to the old implementation.
static void test_randomized_parity_pre_authority() {
    std::mt19937 rng(42);
    WorkingTree wt;
    RefModel ref;
    std::vector<std::string> paths;
    for (int d = 0; d < 4; d++)
        for (int f = 0; f < 8; f++)
            paths.push_back("/d" + std::to_string(d) + "/f" + std::to_string(f));
    for (int i = 0; i < 5000; i++) {
        const std::string& p = paths[rng() % paths.size()];
        switch (rng() % 3) {
        case 0: { auto e = blob((uint8_t)(rng() & 0xff)); wt.insert(p, e); ref.insert(p, e); break; }
        case 1: wt.remove(p); ref.remove(p); break;
        case 2: { const std::string& q = paths[rng() % paths.size()];
                  if (q != p) { wt.rename_entry(p, q); ref.rename_entry(p, q); } break; }
        }
    }
    REQUIRE(wt.size() == ref.size());
    REQUIRE(visible_of(wt) == ref.visible());
    for (auto& p : paths) {
        WorkingTreeEntry re{};
        bool rhit = ref.lookup(p, re);
        auto we = wt.lookup(p);
        REQUIRE(rhit == we.has_value());
        if (rhit) { REQUIRE(we->hash == re.hash); REQUIRE(we->kind == re.kind); }
    }
    // Pre-authority the RAW surface must also match exactly (tombstones
    // included) — identical raw content proves identical checkpoint
    // serialization input, the spec's parity requirement.
    std::vector<std::pair<std::string, EntryKind>> raw_new, raw_ref;
    wt.for_each_including_deleted([&](const std::string& p, const WorkingTreeEntry& e) {
        raw_new.push_back({p, e.kind});
    });
    for (auto& [k, v] : ref.m) raw_ref.push_back({k, v.kind});
    REQUIRE(raw_new == raw_ref);
}

// 2. Same randomized sequence applied on top of an authoritative base:
//    user-visible surface still matches the reference model seeded with the
//    same base content (hygiene must never change the visible surface).
static void test_randomized_parity_post_authority() {
    std::mt19937 rng(1337);
    WorkingTree wt;
    RefModel ref;
    std::vector<std::string> paths;
    for (int d = 0; d < 4; d++)
        for (int f = 0; f < 8; f++)
            paths.push_back("/d" + std::to_string(d) + "/f" + std::to_string(f));
    WorkingTree::EntryMap base;
    for (size_t i = 0; i < paths.size(); i += 2) {   // half the paths pre-exist
        base[paths[i]] = blob((uint8_t)i);
        ref.insert(paths[i], blob((uint8_t)i));
        ref.mark_base(paths[i]);                     // mirrors wt.set_base
    }
    wt.set_base(std::move(base));
    ref.authoritative = true;                         // hygiene now active
    REQUIRE(wt.base_authoritative());
    for (int i = 0; i < 5000; i++) {
        const std::string& p = paths[rng() % paths.size()];
        switch (rng() % 3) {
        case 0: { auto e = blob((uint8_t)(rng() & 0xff)); wt.insert(p, e); ref.insert(p, e); break; }
        case 1: wt.remove(p); ref.remove(p); break;
        case 2: { const std::string& q = paths[rng() % paths.size()];
                  if (q != p) { wt.rename_entry(p, q); ref.rename_entry(p, q); } break; }
        }
    }
    REQUIRE(wt.size() == ref.size());
    REQUIRE(visible_of(wt) == ref.visible());
}

// 3. Hygiene: created-then-deleted path leaves NO residue once authoritative.
static void test_hygiene_erases_non_base_tombstones() {
    WorkingTree wt;
    wt.set_base(WorkingTree::EntryMap{});          // empty but authoritative
    wt.insert("/new.txt", blob(1));
    wt.remove("/new.txt");
    REQUIRE(!wt.lookup_raw("/new.txt").has_value());   // erased, no tombstone
    REQUIRE(wt.delta_entry_count() == 0);
}

// 4. Hygiene guard: pre-authority, remove always leaves a whiteout
//    (source-dir fallback safety).
static void test_pre_authority_always_tombstones() {
    WorkingTree wt;                                 // no base, not authoritative
    wt.insert("/src-file.txt", blob(2));
    wt.remove("/src-file.txt");
    auto raw = wt.lookup_raw("/src-file.txt");
    REQUIRE(raw.has_value());
    REQUIRE(raw->kind == EntryKind::Deleted);
}

// 5. Base-path delete keeps its whiteout even with hygiene on.
static void test_base_path_delete_keeps_tombstone() {
    WorkingTree wt;
    WorkingTree::EntryMap base; base["/keep.txt"] = blob(3);
    wt.set_base(std::move(base));
    wt.remove("/keep.txt");
    auto raw = wt.lookup_raw("/keep.txt");
    REQUIRE(raw.has_value());
    REQUIRE(raw->kind == EntryKind::Deleted);
    REQUIRE(!wt.lookup("/keep.txt").has_value());
    REQUIRE(wt.delta_tombstone_count() == 1);
}

// 6. clone() shares the base (use_count grows) and copies the delta;
//    mutations after clone are isolated both ways.
static void test_clone_shares_base_copies_delta() {
    WorkingTree wt;
    WorkingTree::EntryMap base;
    for (int i = 0; i < 100; i++) base["/f" + std::to_string(i)] = blob((uint8_t)i);
    wt.set_base(std::move(base));
    wt.insert("/uncommitted.txt", blob(200));       // delta content
    REQUIRE(wt.base_shared_count() == 1);
    WorkingTree child = wt.clone();
    REQUIRE(wt.base_shared_count() == 2);           // base shared, not copied
    REQUIRE(child.base_shared_count() == 2);
    REQUIRE(child.lookup("/uncommitted.txt").has_value());   // delta inherited
    REQUIRE(child.base_authoritative());
    child.insert("/child-only.txt", blob(201));
    wt.remove("/f1");
    REQUIRE(!wt.lookup("/child-only.txt").has_value());
    REQUIRE(child.lookup("/f1").has_value());       // isolation both ways
}

// 7. fold_into_base: delta folds in, tree becomes authoritative, visible
//    content unchanged; empty-base fold is the move fast path.
static void test_fold_into_base() {
    WorkingTree wt;
    wt.insert("/a/x.txt", blob(1));
    wt.insert("/a", tree_entry());
    wt.remove("/gone.txt");                          // pre-authority tombstone
    auto before = visible_of(wt);
    wt.fold_into_base();
    REQUIRE(wt.base_authoritative());
    REQUIRE(wt.delta_entry_count() == 0);
    REQUIRE(visible_of(wt) == before);
    auto raw = wt.lookup_raw("/gone.txt");           // whiteout preserved in base
    REQUIRE(raw.has_value() && raw->kind == EntryKind::Deleted);
    // second fold with non-empty base (merge path)
    wt.insert("/b.txt", blob(9));
    wt.fold_into_base();
    REQUIRE(wt.lookup("/b.txt").has_value());
    REQUIRE(wt.delta_entry_count() == 0);
}

// 8. list_dir merges layers: delta overrides and tombstones filter.
static void test_list_dir_merged() {
    WorkingTree wt;
    WorkingTree::EntryMap base;
    base["/d"] = tree_entry();
    base["/d/base1.txt"] = blob(1);
    base["/d/base2.txt"] = blob(2);
    wt.set_base(std::move(base));
    wt.insert("/d/new.txt", blob(3));
    wt.remove("/d/base2.txt");
    wt.insert("/d/base1.txt", blob(42));             // override
    auto ls = wt.list_dir("/d");
    REQUIRE(ls.size() == 2);
    REQUIRE(ls[0].first == "/d/base1.txt");
    REQUIRE(ls[0].second.hash[0] == 42);
    REQUIRE(ls[1].first == "/d/new.txt");
}

// 9. rename_entry hygiene: renaming a whiteout (Deleted) source to a
//    non-base destination must NOT leave a spurious tombstone there.
//    Mirrors rename_dir's routing of Deleted destinations through
//    remove_locked (which erases non-base paths under authority). Reachable
//    via insert -> remove -> fold_into_base (base now holds the whiteout)
//    -> rename to a fresh non-base path.
static void test_rename_whiteout_to_non_base_no_tombstone() {
    WorkingTree wt;
    wt.insert("/x", blob(1));
    wt.remove("/x");                         // pre-authority tombstone
    wt.fold_into_base();                     // base holds /x as Deleted; authoritative
    REQUIRE(wt.base_authoritative());
    wt.rename_entry("/x", "/y");             // /y is NOT visibly in the base
    // Hygiene: destination raw-absent, no spurious delta tombstone/bloat.
    REQUIRE(!wt.lookup_raw("/y").has_value());
    REQUIRE(wt.delta_tombstone_count() == 0);
    REQUIRE(wt.delta_entry_count() == 0);
    // Visible surface unaffected: both /x and /y invisible, tree empty.
    REQUIRE(!wt.lookup("/x").has_value());
    REQUIRE(!wt.lookup("/y").has_value());
    REQUIRE(wt.size() == 0);
}

// 10. A path imported from the live source remains source-backed even when it
//     was discovered after authority was published. Deleting or renaming it
//     must hide the still-present source path with a whiteout.
static void test_source_origin_keeps_whiteout_post_authority() {
    WorkingTree wt;
    wt.set_base(WorkingTree::EntryMap{});

    wt.insert_source("/lazy.txt", blob(10));
    wt.insert("/lazy.txt", blob(11));          // later overlay write preserves origin
    wt.remove("/lazy.txt");
    auto removed = wt.lookup_raw("/lazy.txt");
    REQUIRE(removed.has_value());
    REQUIRE(removed->kind == EntryKind::Deleted);

    wt.insert_source("/rename.txt", blob(12));
    wt.rename_entry("/rename.txt", "/renamed.txt");
    auto renamed_from = wt.lookup_raw("/rename.txt");
    REQUIRE(renamed_from.has_value());
    REQUIRE(renamed_from->kind == EntryKind::Deleted);
    REQUIRE(wt.lookup("/renamed.txt").has_value());
}

// 11. Source-origin state is branch-local lifecycle state: clones copy it,
//     moves transfer it, and replacing/clearing the tree drops stale markers.
static void test_source_origin_clone_move_and_reset() {
    WorkingTree wt;
    wt.set_base(WorkingTree::EntryMap{});
    wt.insert_source("/source.txt", blob(20));

    WorkingTree child = wt.clone();
    child.remove("/source.txt");
    auto child_raw = child.lookup_raw("/source.txt");
    REQUIRE(child_raw.has_value() && child_raw->kind == EntryKind::Deleted);

    WorkingTree moved(std::move(wt));
    moved.remove("/source.txt");
    auto moved_raw = moved.lookup_raw("/source.txt");
    REQUIRE(moved_raw.has_value() && moved_raw->kind == EntryKind::Deleted);

    WorkingTree assigned_src;
    assigned_src.set_base(WorkingTree::EntryMap{});
    assigned_src.insert_source("/assigned.txt", blob(21));
    WorkingTree assigned;
    assigned = std::move(assigned_src);
    assigned.remove("/assigned.txt");
    auto assigned_raw = assigned.lookup_raw("/assigned.txt");
    REQUIRE(assigned_raw.has_value() && assigned_raw->kind == EntryKind::Deleted);

    assigned.clear();
    assigned.set_base(WorkingTree::EntryMap{});
    assigned.insert("/assigned.txt", blob(22));
    assigned.remove("/assigned.txt");
    REQUIRE(!assigned.lookup_raw("/assigned.txt").has_value());

    WorkingTree replaced;
    replaced.set_base(WorkingTree::EntryMap{});
    replaced.insert_source("/replaced.txt", blob(23));
    replaced.set_base(WorkingTree::EntryMap{});
    replaced.insert("/replaced.txt", blob(24));
    replaced.remove("/replaced.txt");
    REQUIRE(!replaced.lookup_raw("/replaced.txt").has_value());
}

// 12. Memory accounting is sampled coherently through one WorkingTree lock.
static void test_memory_stats_snapshot() {
    WorkingTree wt;
    WorkingTree::EntryMap base;
    base["/base.txt"] = blob(30);
    wt.set_base(std::move(base));
    wt.insert("/delta.txt", blob(31));
    wt.remove("/base.txt");

    auto stats = wt.memory_stats();
    REQUIRE(stats.base_entries == 1);
    REQUIRE(stats.base_shared_by == 1);
    REQUIRE(stats.delta_entries == 2);
    REQUIRE(stats.delta_tombstones == 1);
}

int main() {
    test_randomized_parity_pre_authority();
    test_randomized_parity_post_authority();
    test_hygiene_erases_non_base_tombstones();
    test_pre_authority_always_tombstones();
    test_base_path_delete_keeps_tombstone();
    test_clone_shares_base_copies_delta();
    test_fold_into_base();
    test_list_dir_merged();
    test_rename_whiteout_to_non_base_no_tombstone();
    test_source_origin_keeps_whiteout_post_authority();
    test_source_origin_clone_move_and_reset();
    test_memory_stats_snapshot();
    std::printf("test_working_tree_delta: PASS\n");
    return 0;
}
