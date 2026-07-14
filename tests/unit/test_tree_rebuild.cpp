// rebuild_working_tree scratch-map semantics + bootstrap fold-at-completion
// (2026-07-13 mem-and-gc design, Task 2).
#include "bootstrap.h"
#include "inode_map.h"
#include "object_store.h"
#include "tree_serialize.h"
#include "working_tree.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace cas;
namespace fs = std::filesystem;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::error_code ec;
    fs::path parent = fs::temp_directory_path(ec);
    REQUIRE(!ec);

    static std::mt19937_64 rng(std::random_device{}());
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path candidate = parent /
            (std::string("agentvfs-rebuild-") + suffix + "-" +
             std::to_string(rng()));
        ec.clear();
        if (fs::create_directory(candidate, ec)) return candidate.string();
        REQUIRE(!ec || ec == std::errc::file_exists);
    }
    REQUIRE(false);
    return {};
}

static void wait_for_bootstrap(Bootstrap& bs) {
    for (int i = 0; i < 500 && bs.pending(); i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    REQUIRE(!bs.pending());
}

// 1. Success path: rebuild publishes an authoritative base with empty delta
//    and the same visible content that was serialized.
static void test_rebuild_success_sets_base() {
    std::string store_dir = make_tmp_dir("store");
    ObjectStore store(store_dir);
    REQUIRE(store.init_layout());

    WorkingTree src;
    Hash bh = store.write_blob(std::vector<uint8_t>{'h','i'});
    REQUIRE(!(bh == ZERO_HASH));
    src.insert("/d", {EntryKind::Tree, ZERO_HASH, 0040755});
    src.insert("/d/a.txt", {EntryKind::Blob, bh, 0100644});
    src.remove("/whiteout.txt");                       // pre-authority tombstone
    std::vector<Hash> written;
    Hash root = serialize_working_tree(src, store, written);
    REQUIRE(!(root == ZERO_HASH));

    WorkingTree dst;
    std::string err;
    REQUIRE(rebuild_working_tree(root, store, dst, &err));
    REQUIRE(dst.base_authoritative());
    REQUIRE(dst.delta_entry_count() == 0);
    REQUIRE(dst.lookup("/d/a.txt").has_value());
    auto raw = dst.lookup_raw("/whiteout.txt");        // whiteouts survive rebuild
    REQUIRE(raw.has_value() && raw->kind == EntryKind::Deleted);
    fs::remove_all(store_dir);
}

// 2. Failure path: missing root tree leaves the destination tree UNTOUCHED
//    and reports "tree object missing".
static void test_rebuild_failure_leaves_wt_untouched() {
    std::string store_dir = make_tmp_dir("store2");
    ObjectStore store(store_dir);
    REQUIRE(store.init_layout());

    WorkingTree wt;
    wt.insert("/pre-existing.txt", {EntryKind::Blob, ZERO_HASH, 0100644});
    size_t before = wt.size();

    Hash bogus{};
    bogus[0] = 0xde; bogus[1] = 0xad;
    std::string err;
    REQUIRE(!rebuild_working_tree(bogus, store, wt, &err));
    REQUIRE(err.rfind("tree object missing", 0) == 0);
    REQUIRE(wt.size() == before);                       // untouched
    REQUIRE(wt.lookup("/pre-existing.txt").has_value());
    REQUIRE(!wt.base_authoritative());
    fs::remove_all(store_dir);
}

// 3. Bootstrap walk completion folds the ingested tree into an
//    authoritative shared base (empty delta), preserving content.
static void test_bootstrap_folds_at_completion() {
    std::string src_dir = make_tmp_dir("src");
    std::string store_dir = make_tmp_dir("store3");
    { std::ofstream(src_dir + "/a.txt") << "alpha"; }
    fs::create_directory(src_dir + "/sub");
    { std::ofstream(src_dir + "/sub/b.txt") << "beta"; }

    ObjectStore store(store_dir);
    REQUIRE(store.init_layout());
    WorkingTree wt;
    InodeMap im;
    std::mutex checkpoint_mu;
    Bootstrap bs(src_dir, store, wt, im, checkpoint_mu);
    bs.start_background();
    wait_for_bootstrap(bs);
    bs.stop_background();

    REQUIRE(wt.base_authoritative());
    REQUIRE(wt.delta_entry_count() == 0);
    REQUIRE(wt.base_entry_count() >= 3);               // a.txt, sub, sub/b.txt
    REQUIRE(wt.lookup("/a.txt").has_value());
    REQUIRE(wt.lookup("/sub/b.txt").has_value());
    fs::remove_all(src_dir);
    fs::remove_all(store_dir);
}

// 4. A partial walk whose blob ingest fails must not publish authority. Once
//    the store is repaired, lazy ingest is source-backed and its deletion
//    cannot be undone by ensure_path re-importing the physical source file.
static void test_partial_walk_failure_stays_safe() {
    std::string src_dir = make_tmp_dir("partial-src");
    std::string store_dir = make_tmp_dir("partial-store");
    fs::create_directory(src_dir + "/sub");
    { std::ofstream(src_dir + "/sub/lazy.txt") << "source"; }

    ObjectStore store(store_dir);              // deliberately not initialized
    WorkingTree wt;
    wt.set_base(WorkingTree::EntryMap{});       // model a restart base
    InodeMap im;
    std::mutex checkpoint_mu;
    Bootstrap bs(src_dir, store, wt, im, checkpoint_mu);
    bs.start_background();
    wait_for_bootstrap(bs);
    bs.stop_background();

    REQUIRE(!wt.base_authoritative());
    REQUIRE(wt.lookup("/sub").has_value());     // the walk made partial progress
    REQUIRE(!wt.lookup("/sub/lazy.txt").has_value());

    REQUIRE(store.init_layout());
    REQUIRE(bs.ensure_path("/sub/lazy.txt"));
    wt.remove("/sub/lazy.txt");
    auto raw = wt.lookup_raw("/sub/lazy.txt");
    REQUIRE(raw.has_value() && raw->kind == EntryKind::Deleted);
    REQUIRE(!bs.ensure_path("/sub/lazy.txt"));

    fs::remove_all(src_dir);
    fs::remove_all(store_dir);
}

// 5. Files added to the physical source after a completed walk are also
//    source-backed when lazily discovered, for both unlink and rename.
static void test_post_authority_source_additions_keep_whiteouts() {
    std::string src_dir = make_tmp_dir("late-src");
    std::string store_dir = make_tmp_dir("late-store");
    ObjectStore store(store_dir);
    REQUIRE(store.init_layout());
    WorkingTree wt;
    InodeMap im;
    std::mutex checkpoint_mu;
    Bootstrap bs(src_dir, store, wt, im, checkpoint_mu);
    bs.start_background();
    wait_for_bootstrap(bs);
    bs.stop_background();
    REQUIRE(wt.base_authoritative());

    { std::ofstream(src_dir + "/late.txt") << "late"; }
    REQUIRE(bs.ensure_path("/late.txt"));
    wt.remove("/late.txt");
    auto removed = wt.lookup_raw("/late.txt");
    REQUIRE(removed.has_value() && removed->kind == EntryKind::Deleted);
    REQUIRE(!bs.ensure_path("/late.txt"));

    { std::ofstream(src_dir + "/rename.txt") << "rename"; }
    REQUIRE(bs.ensure_path("/rename.txt"));
    wt.rename_entry("/rename.txt", "/renamed.txt");
    auto renamed_from = wt.lookup_raw("/rename.txt");
    REQUIRE(renamed_from.has_value() && renamed_from->kind == EntryKind::Deleted);
    REQUIRE(wt.lookup("/renamed.txt").has_value());
    REQUIRE(!bs.ensure_path("/rename.txt"));

    fs::remove_all(src_dir);
    fs::remove_all(store_dir);
}

// 6. A transient lazy-ingest failure must not globally disable tombstone
//    hygiene after a successful authoritative fold. The failed path can be
//    retried once the store recovers.
static void test_lazy_ingest_failure_preserves_authority() {
    std::string src_dir = make_tmp_dir("lazy-failure-src");
    std::string store_dir = make_tmp_dir("lazy-failure-store");
    ObjectStore store(store_dir);
    REQUIRE(store.init_layout());
    WorkingTree wt;
    InodeMap im;
    std::mutex checkpoint_mu;
    Bootstrap bs(src_dir, store, wt, im, checkpoint_mu);
    bs.start_background();
    wait_for_bootstrap(bs);
    bs.stop_background();
    REQUIRE(wt.base_authoritative());

    { std::ofstream(src_dir + "/retry.txt") << "unique retry payload"; }
    fs::remove_all(store.tmp_dir());
    { std::ofstream(store.tmp_dir()) << "not a directory"; }
    REQUIRE(!bs.ensure_path("/retry.txt"));
    REQUIRE(wt.base_authoritative());

    wt.insert("/created.txt", {EntryKind::Blob, ZERO_HASH, 0100644});
    wt.remove("/created.txt");
    REQUIRE(!wt.lookup_raw("/created.txt").has_value());

    REQUIRE(fs::remove(store.tmp_dir()));
    REQUIRE(fs::create_directory(store.tmp_dir()));
    REQUIRE(bs.ensure_path("/retry.txt"));

    fs::remove_all(src_dir);
    fs::remove_all(store_dir);
}

// 7. Lazy publication takes the main checkpoint lock across source lookup,
//    CAS write, and WT insertion, so it cannot overwrite a whiteout installed
//    by an operation that acquired that lock first.
static void test_lazy_ingest_preserves_concurrent_whiteout() {
    std::string src_dir = make_tmp_dir("lazy-race-src");
    std::string store_dir = make_tmp_dir("lazy-race-store");
    { std::ofstream(src_dir + "/race.txt") << "source"; }
    ObjectStore store(store_dir);
    REQUIRE(store.init_layout());
    WorkingTree wt;
    wt.set_base(WorkingTree::EntryMap{});
    InodeMap im;
    std::mutex checkpoint_mu;
    Bootstrap bs(src_dir, store, wt, im, checkpoint_mu);

    std::unique_lock<std::mutex> held(checkpoint_mu);
    auto lazy = std::async(std::launch::async, [&] {
        return bs.ensure_path("/race.txt");
    });
    REQUIRE(lazy.wait_for(std::chrono::milliseconds(50)) ==
            std::future_status::timeout);
    wt.insert_source("/race.txt", {EntryKind::Blob, ZERO_HASH, 0100644});
    wt.remove("/race.txt");
    held.unlock();

    REQUIRE(lazy.wait_for(std::chrono::seconds(2)) ==
            std::future_status::ready);
    REQUIRE(!lazy.get());
    auto raw = wt.lookup_raw("/race.txt");
    REQUIRE(raw.has_value() && raw->kind == EntryKind::Deleted);

    fs::remove_all(src_dir);
    fs::remove_all(store_dir);
}

int main() {
    test_rebuild_success_sets_base();
    test_rebuild_failure_leaves_wt_untouched();
    test_bootstrap_folds_at_completion();
    test_partial_walk_failure_stays_safe();
    test_post_authority_source_additions_keep_whiteouts();
    test_lazy_ingest_failure_preserves_authority();
    test_lazy_ingest_preserves_concurrent_whiteout();
    std::printf("test_tree_rebuild: PASS\n");
    return 0;
}
