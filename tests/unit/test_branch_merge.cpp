#include "branch_merge.h"
#include "hash.h"
#include "working_tree.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

static Hash tagged_hash(uint8_t tag) {
    Hash h{};
    h[0] = tag;
    return h;
}

static void put_blob(WorkingTree& wt, const std::string& path, uint8_t tag, uint32_t mode = 0100644) {
    wt.insert(path, {EntryKind::Blob, tagged_hash(tag), mode});
}

static void put_tree(WorkingTree& wt, const std::string& path, uint8_t tag = 0xD0) {
    wt.insert(path, {EntryKind::Tree, tagged_hash(tag), 0040755});
}

static uint8_t tag_at(const WorkingTree& wt, const std::string& path) {
    auto e = wt.lookup(path);
    REQUIRE(e.has_value());
    return e->hash[0];
}

static bool has_conflict(const MergeResult& r, const std::string& path) {
    return std::find(r.conflicts.begin(), r.conflicts.end(), path) != r.conflicts.end();
}

static void test_non_overlapping_changes_merge_cleanly() {
    WorkingTree base;
    put_blob(base, "/base.txt", 0x10);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    put_blob(source, "/source.txt", 0x11);
    put_blob(target, "/target.txt", 0x12);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(r.ok);
    REQUIRE(r.conflicts.empty());
    REQUIRE(tag_at(r.merged, "/base.txt") == 0x10);
    REQUIRE(tag_at(r.merged, "/source.txt") == 0x11);
    REQUIRE(tag_at(r.merged, "/target.txt") == 0x12);
    std::printf("  PASS test_non_overlapping_changes_merge_cleanly\n");
}

static void test_source_and_target_single_sided_changes() {
    WorkingTree base;
    put_blob(base, "/source-edit.txt", 0x20);
    put_blob(base, "/target-edit.txt", 0x21);
    put_blob(base, "/source-delete.txt", 0x22);
    put_blob(base, "/target-delete.txt", 0x23);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    put_blob(source, "/source-edit.txt", 0x24);
    source.remove("/source-delete.txt");
    put_blob(target, "/target-edit.txt", 0x25);
    target.remove("/target-delete.txt");

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(r.ok);
    REQUIRE(tag_at(r.merged, "/source-edit.txt") == 0x24);
    REQUIRE(tag_at(r.merged, "/target-edit.txt") == 0x25);
    REQUIRE(!r.merged.lookup("/source-delete.txt").has_value());
    REQUIRE(!r.merged.lookup("/target-delete.txt").has_value());
    std::printf("  PASS test_source_and_target_single_sided_changes\n");
}

static void test_identical_same_path_change_merges_cleanly() {
    WorkingTree base;
    put_blob(base, "/same.txt", 0x30);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    put_blob(source, "/same.txt", 0x31, 0100755);
    put_blob(target, "/same.txt", 0x31, 0100755);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(r.ok);
    auto e = r.merged.lookup("/same.txt");
    REQUIRE(e.has_value());
    REQUIRE(e->hash[0] == 0x31);
    REQUIRE(e->mode == 0100755);
    std::printf("  PASS test_identical_same_path_change_merges_cleanly\n");
}

static void test_conflicts_are_sorted() {
    WorkingTree base;
    put_blob(base, "/z.txt", 0x40);
    put_blob(base, "/a.txt", 0x41);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    put_blob(source, "/z.txt", 0x42);
    put_blob(target, "/z.txt", 0x43);
    put_blob(source, "/a.txt", 0x44);
    put_blob(target, "/a.txt", 0x45);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(!r.ok);
    REQUIRE(r.error == "merge conflicts");
    REQUIRE(r.conflicts.size() == 2);
    REQUIRE(r.conflicts[0] == "/a.txt");
    REQUIRE(r.conflicts[1] == "/z.txt");
    REQUIRE(r.merged.size() == 0);
    std::printf("  PASS test_conflicts_are_sorted\n");
}

static void test_delete_vs_edit_conflicts() {
    WorkingTree base;
    put_blob(base, "/victim.txt", 0x50);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    source.remove("/victim.txt");
    put_blob(target, "/victim.txt", 0x51);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(!r.ok);
    REQUIRE(has_conflict(r, "/victim.txt"));
    REQUIRE(r.merged.size() == 0);
    std::printf("  PASS test_delete_vs_edit_conflicts\n");
}

static void test_directory_delete_vs_descendant_add_edit_conflicts() {
    WorkingTree base;
    put_tree(base, "/dir", 0x70);
    put_blob(base, "/dir/existing.txt", 0x71);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    source.remove("/dir");
    put_blob(target, "/dir/new.txt", 0x72);
    put_blob(target, "/dir/existing.txt", 0x73);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(!r.ok);
    REQUIRE(has_conflict(r, "/dir"));
    REQUIRE(r.merged.size() == 0);
    std::printf("  PASS test_directory_delete_vs_descendant_add_edit_conflicts\n");
}

static void test_directory_replacement_vs_descendant_edit_conflicts() {
    WorkingTree base;
    put_tree(base, "/dir", 0x80);
    put_blob(base, "/dir/file.txt", 0x81);

    WorkingTree source = base.clone();
    WorkingTree target = base.clone();
    put_blob(source, "/dir", 0x82);
    put_blob(target, "/dir/file.txt", 0x83);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(!r.ok);
    REQUIRE(has_conflict(r, "/dir"));
    REQUIRE(r.merged.size() == 0);
    std::printf("  PASS test_directory_replacement_vs_descendant_edit_conflicts\n");
}

static void test_file_vs_directory_conflicts() {
    WorkingTree base;
    WorkingTree source;
    WorkingTree target;

    put_blob(source, "/node", 0x60);
    put_tree(target, "/node", 0x61);
    put_blob(target, "/node/child.txt", 0x62);

    MergeResult r = merge_trees(base, source, target);
    REQUIRE(!r.ok);
    REQUIRE(has_conflict(r, "/node"));
    REQUIRE(r.merged.size() == 0);
    std::printf("  PASS test_file_vs_directory_conflicts\n");
}

int main() {
    std::printf("test_branch_merge:\n");
    test_non_overlapping_changes_merge_cleanly();
    test_source_and_target_single_sided_changes();
    test_identical_same_path_change_merges_cleanly();
    test_conflicts_are_sorted();
    test_delete_vs_edit_conflicts();
    test_directory_delete_vs_descendant_add_edit_conflicts();
    test_directory_replacement_vs_descendant_edit_conflicts();
    test_file_vs_directory_conflicts();
    std::printf("PASS test_branch_merge\n");
    return 0;
}
