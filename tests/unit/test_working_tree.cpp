#include "working_tree.h"
#include <cassert>
#include <cstdio>

using namespace cas;

static Hash make_hash(uint8_t val) {
    Hash h;
    h.fill(val);
    return h;
}

static void test_insert_lookup() {
    WorkingTree wt;
    wt.insert("/a.txt", {EntryKind::Blob, make_hash(1), 0100644});
    auto e = wt.lookup("/a.txt");
    assert(e.has_value());
    assert(e->kind == EntryKind::Blob);
    assert(e->hash == make_hash(1));
    assert(!wt.lookup("/b.txt").has_value());
    std::printf("  PASS test_insert_lookup\n");
}

static void test_remove() {
    WorkingTree wt;
    wt.insert("/a.txt", {EntryKind::Blob, make_hash(1), 0100644});
    wt.remove("/a.txt");
    assert(!wt.lookup("/a.txt").has_value());
    std::printf("  PASS test_remove\n");
}

static void test_list_dir() {
    WorkingTree wt;
    wt.insert("/src", {EntryKind::Tree, ZERO_HASH, 040755});
    wt.insert("/src/a.cpp", {EntryKind::Blob, make_hash(1), 0100644});
    wt.insert("/src/b.cpp", {EntryKind::Blob, make_hash(2), 0100644});
    wt.insert("/src/sub/c.cpp", {EntryKind::Blob, make_hash(3), 0100644});
    auto children = wt.list_dir("/src");
    assert(children.size() == 2); // a.cpp and b.cpp, not sub/c.cpp
    std::printf("  PASS test_list_dir\n");
}

static void test_rename_dir() {
    WorkingTree wt;
    wt.insert("/old", {EntryKind::Tree, ZERO_HASH, 040755});
    wt.insert("/old/a.txt", {EntryKind::Blob, make_hash(1), 0100644});
    wt.insert("/old/sub/b.txt", {EntryKind::Blob, make_hash(2), 0100644});
    wt.rename_dir("/old", "/new");
    assert(!wt.lookup("/old/a.txt").has_value());
    assert(wt.lookup("/new/a.txt").has_value());
    assert(wt.lookup("/new/sub/b.txt").has_value());
    std::printf("  PASS test_rename_dir\n");
}

int main() {
    std::printf("test_working_tree:\n");
    test_insert_lookup();
    test_remove();
    test_list_dir();
    test_rename_dir();
    std::printf("All working tree tests passed.\n");
    return 0;
}
