#include "branch_merge.h"
#include "commit.h"
#include "hash.h"
#include "object_store.h"
#include "tree_serialize.h"
#include "working_tree.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static std::string make_tmp_dir(const char* suffix) {
    std::string templ = std::string("/tmp/agentvfs-branch-merge-commit-") + suffix + "-XXXXXX";
    std::vector<char> buf(templ.begin(), templ.end());
    buf.push_back('\0');
    char* path = mkdtemp(buf.data());
    REQUIRE(path != nullptr);
    return std::string(path);
}

static void remove_dir_recursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;
    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) continue;
        std::string child = path + "/" + ent->d_name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) remove_dir_recursive(child);
        else std::remove(child.c_str());
    }
    closedir(dir);
    rmdir(path.c_str());
}

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

static void put_blob(WorkingTree& wt, const std::string& path, uint8_t tag) {
    wt.insert(path, {EntryKind::Blob, tagged_hash(tag), 0100644});
}

static void put_tree(WorkingTree& wt, const std::string& path) {
    wt.insert(path, {EntryKind::Tree, ZERO_HASH, 040755});
}

static Hash write_test_commit(
    ObjectStore& store,
    WorkingTree& wt,
    const std::vector<Hash>& parents,
    const std::string& label) {
    std::string error;
    Hash h = write_commit_with_parents(store, wt, parents, 7, 3, label, error);
    REQUIRE(h != ZERO_HASH);
    REQUIRE(error.empty());
    return h;
}

static CommitData read_commit(ObjectStore& store, const Hash& h) {
    std::vector<uint8_t> body;
    REQUIRE(store.read_commit(h, body));
    CommitData cd;
    REQUIRE(deserialize_commit(body, cd));
    return cd;
}

static bool has_hash(const std::vector<Hash>& hashes, const Hash& needle) {
    for (const Hash& h : hashes)
        if (h == needle) return true;
    return false;
}

static void test_serialize_working_tree_reports_referenced_leaf_hashes() {
    std::string root = make_tmp_dir("serialize-leaves");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    Hash blob = store.write_blob(reinterpret_cast<const uint8_t*>("body"), 4);
    REQUIRE(blob != ZERO_HASH);
    WorkingTree wt;
    wt.insert("/file.txt", {EntryKind::Blob, blob, 0100644});

    std::vector<Hash> written;
    std::vector<Hash> leaves;
    Hash tree = serialize_working_tree(wt, store, written, &leaves);

    REQUIRE(tree != ZERO_HASH);
    REQUIRE(leaves.size() == 1);
    REQUIRE(leaves[0] == blob);
    REQUIRE(!has_hash(written, blob));
    REQUIRE(has_hash(written, tree));

    remove_dir_recursive(root);
    std::printf("  PASS test_serialize_working_tree_reports_referenced_leaf_hashes\n");
}

static void test_common_ancestor_across_two_branches() {
    std::string root = make_tmp_dir("ancestor");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree base_wt;
    put_blob(base_wt, "/base.txt", 0x10);
    Hash base = write_test_commit(store, base_wt, {}, "base");

    WorkingTree source_wt = base_wt.clone();
    put_blob(source_wt, "/source.txt", 0x11);
    Hash source = write_test_commit(store, source_wt, {base}, "source");

    WorkingTree target_wt = base_wt.clone();
    put_blob(target_wt, "/target.txt", 0x12);
    Hash target = write_test_commit(store, target_wt, {base}, "target");

    Hash common = ZERO_HASH;
    std::string error;
    REQUIRE(find_common_ancestor(store, source, target, common, error));
    REQUIRE(error.empty());
    REQUIRE(common == base);

    remove_dir_recursive(root);
    std::printf("  PASS test_common_ancestor_across_two_branches\n");
}

static void test_merge_commit_has_two_parents_and_loads_tree() {
    std::string root = make_tmp_dir("merge");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree base_wt;
    Hash base = write_test_commit(store, base_wt, {}, "base");

    WorkingTree source_wt = base_wt.clone();
    put_blob(source_wt, "/source.txt", 0x21);
    Hash source = write_test_commit(store, source_wt, {base}, "source");

    WorkingTree target_wt = base_wt.clone();
    put_blob(target_wt, "/target.txt", 0x22);
    Hash target = write_test_commit(store, target_wt, {base}, "target");

    WorkingTree merged;
    put_blob(merged, "/source.txt", 0x21);
    put_blob(merged, "/target.txt", 0x22);
    Hash merge = write_test_commit(store, merged, {target, source}, "merge source into target");

    CommitData cd = read_commit(store, merge);
    REQUIRE(cd.parents.size() == 2);
    REQUIRE(cd.parents[0] == target);
    REQUIRE(cd.parents[1] == source);
    REQUIRE(cd.label == "merge source into target");
    REQUIRE(cd.policy_version == 3);

    WorkingTree loaded;
    CommitData loaded_cd;
    std::string error;
    REQUIRE(load_commit_tree(store, merge, loaded, loaded_cd, error));
    REQUIRE(error.empty());
    REQUIRE(loaded.lookup("/source.txt").has_value());
    REQUIRE(loaded.lookup("/target.txt").has_value());

    remove_dir_recursive(root);
    std::printf("  PASS test_merge_commit_has_two_parents_and_loads_tree\n");
}

static void test_reloaded_nested_clean_merge_ignores_directory_tree_hashes() {
    std::string root = make_tmp_dir("nested-clean");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree base_wt;
    put_tree(base_wt, "/dir");
    Hash base = write_test_commit(store, base_wt, {}, "base");

    WorkingTree source_wt = base_wt.clone();
    put_blob(source_wt, "/dir/source.txt", 0x25);
    Hash source = write_test_commit(store, source_wt, {base}, "source");

    WorkingTree target_wt = base_wt.clone();
    put_blob(target_wt, "/dir/target.txt", 0x26);
    Hash target = write_test_commit(store, target_wt, {base}, "target");

    WorkingTree reloaded_base;
    WorkingTree reloaded_source;
    WorkingTree reloaded_target;
    CommitData base_cd;
    CommitData source_cd;
    CommitData target_cd;
    std::string error;
    REQUIRE(load_commit_tree(store, base, reloaded_base, base_cd, error));
    REQUIRE(load_commit_tree(store, source, reloaded_source, source_cd, error));
    REQUIRE(load_commit_tree(store, target, reloaded_target, target_cd, error));

    auto base_dir = reloaded_base.lookup("/dir");
    auto source_dir = reloaded_source.lookup("/dir");
    auto target_dir = reloaded_target.lookup("/dir");
    REQUIRE(base_dir.has_value());
    REQUIRE(source_dir.has_value());
    REQUIRE(target_dir.has_value());
    REQUIRE(base_dir->kind == EntryKind::Tree);
    REQUIRE(source_dir->kind == EntryKind::Tree);
    REQUIRE(target_dir->kind == EntryKind::Tree);
    REQUIRE(base_dir->hash != source_dir->hash);
    REQUIRE(base_dir->hash != target_dir->hash);
    REQUIRE(source_dir->hash != target_dir->hash);

    MergeResult merge = merge_trees(reloaded_base, reloaded_source, reloaded_target);
    REQUIRE(merge.ok);
    REQUIRE(merge.error.empty());
    REQUIRE(merge.conflicts.empty());
    REQUIRE(merge.merged.lookup("/dir").has_value());
    REQUIRE(merge.merged.lookup("/dir/source.txt").has_value());
    REQUIRE(merge.merged.lookup("/dir/target.txt").has_value());

    remove_dir_recursive(root);
    std::printf("  PASS test_reloaded_nested_clean_merge_ignores_directory_tree_hashes\n");
}

static void test_no_common_ancestor_returns_false() {
    std::string root = make_tmp_dir("none");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree left_wt;
    WorkingTree right_wt;
    Hash left = write_test_commit(store, left_wt, {}, "left");
    Hash right = write_test_commit(store, right_wt, {}, "right");

    Hash common = ZERO_HASH;
    std::string error;
    REQUIRE(!find_common_ancestor(store, left, right, common, error));
    REQUIRE(common == ZERO_HASH);
    REQUIRE(error == "no common ancestor");

    remove_dir_recursive(root);
    std::printf("  PASS test_no_common_ancestor_returns_false\n");
}

static void test_common_ancestor_prefers_descendant_merge_base() {
    std::string root = make_tmp_dir("best-base");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree base_wt;
    put_blob(base_wt, "/base.txt", 0x30);
    Hash base = write_test_commit(store, base_wt, {}, "base");

    WorkingTree middle_wt = base_wt.clone();
    put_blob(middle_wt, "/middle.txt", 0x31);
    Hash middle = write_test_commit(store, middle_wt, {base}, "middle");

    WorkingTree descendant_wt = middle_wt.clone();
    put_blob(descendant_wt, "/descendant.txt", 0x32);
    Hash descendant = write_test_commit(store, descendant_wt, {middle}, "descendant");

    WorkingTree first_wt = descendant_wt.clone();
    put_blob(first_wt, "/first.txt", 0x33);
    Hash first = write_test_commit(store, first_wt, {descendant}, "first");

    WorkingTree second_wt = descendant_wt.clone();
    put_blob(second_wt, "/second.txt", 0x34);
    Hash second = write_test_commit(store, second_wt, {base, descendant}, "second");

    Hash common = ZERO_HASH;
    std::string error;
    REQUIRE(find_common_ancestor(store, first, second, common, error));
    REQUIRE(error.empty());
    REQUIRE(common == descendant);

    remove_dir_recursive(root);
    std::printf("  PASS test_common_ancestor_prefers_descendant_merge_base\n");
}

static void test_load_commit_tree_failure_is_atomic() {
    std::string root = make_tmp_dir("atomic");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree original_wt;
    put_blob(original_wt, "/keep.txt", 0x40);

    CommitData original_cd;
    original_cd.tree_hash = tagged_hash(0x41);
    original_cd.parents = {tagged_hash(0x42)};
    original_cd.session_id = 99;
    original_cd.timestamp_ns = 123;
    original_cd.label = "original output";
    original_cd.policy_version = 5;

    CommitData bad_cd;
    bad_cd.tree_hash = tagged_hash(0xf0);
    bad_cd.session_id = 7;
    bad_cd.timestamp_ns = 456;
    bad_cd.label = "bad missing tree";
    bad_cd.policy_version = 3;
    Hash bad_commit = store.write_commit(serialize_commit(bad_cd));
    REQUIRE(bad_commit != ZERO_HASH);

    std::string error = "stale";
    REQUIRE(!load_commit_tree(store, bad_commit, original_wt, original_cd, error));
    REQUIRE(error == "failed to rebuild tree " + hash_to_hex(bad_cd.tree_hash));
    REQUIRE(original_wt.lookup("/keep.txt").has_value());
    REQUIRE(original_cd.label == "original output");
    REQUIRE(original_cd.tree_hash == tagged_hash(0x41));
    REQUIRE(original_cd.parents.size() == 1);
    REQUIRE(original_cd.parents[0] == tagged_hash(0x42));

    remove_dir_recursive(root);
    std::printf("  PASS test_load_commit_tree_failure_is_atomic\n");
}

static void test_zero_hash_inputs_return_no_common_ancestor() {
    std::string root = make_tmp_dir("zero");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree wt;
    Hash commit = write_test_commit(store, wt, {}, "commit");

    Hash common = tagged_hash(0x50);
    std::string error = "stale";
    REQUIRE(!find_common_ancestor(store, ZERO_HASH, commit, common, error));
    REQUIRE(common == ZERO_HASH);
    REQUIRE(error == "no common ancestor");

    common = tagged_hash(0x51);
    error = "stale";
    REQUIRE(!find_common_ancestor(store, commit, ZERO_HASH, common, error));
    REQUIRE(common == ZERO_HASH);
    REQUIRE(error == "no common ancestor");

    remove_dir_recursive(root);
    std::printf("  PASS test_zero_hash_inputs_return_no_common_ancestor\n");
}

static void test_successes_clear_stale_error() {
    std::string root = make_tmp_dir("stale");
    ObjectStore store(root);
    REQUIRE(store.init_layout());

    WorkingTree wt;
    put_blob(wt, "/file.txt", 0x60);

    std::string error = "stale";
    Hash base = write_commit_with_parents(store, wt, {}, 7, 3, "base", error);
    REQUIRE(base != ZERO_HASH);
    REQUIRE(error.empty());

    WorkingTree loaded;
    CommitData loaded_cd;
    error = "stale";
    REQUIRE(load_commit_tree(store, base, loaded, loaded_cd, error));
    REQUIRE(error.empty());
    REQUIRE(loaded.lookup("/file.txt").has_value());

    WorkingTree child_wt = wt.clone();
    put_blob(child_wt, "/child.txt", 0x61);
    Hash child = write_test_commit(store, child_wt, {base}, "child");

    Hash common = ZERO_HASH;
    error = "stale";
    REQUIRE(find_common_ancestor(store, base, child, common, error));
    REQUIRE(error.empty());
    REQUIRE(common == base);

    remove_dir_recursive(root);
    std::printf("  PASS test_successes_clear_stale_error\n");
}

int main() {
    std::printf("test_branch_merge_commit:\n");
    test_serialize_working_tree_reports_referenced_leaf_hashes();
    test_common_ancestor_across_two_branches();
    test_merge_commit_has_two_parents_and_loads_tree();
    test_reloaded_nested_clean_merge_ignores_directory_tree_hashes();
    test_no_common_ancestor_returns_false();
    test_load_commit_tree_failure_is_atomic();
    test_common_ancestor_prefers_descendant_merge_base();
    test_zero_hash_inputs_return_no_common_ancestor();
    test_successes_clear_stale_error();
    std::printf("PASS test_branch_merge_commit\n");
    return 0;
}
