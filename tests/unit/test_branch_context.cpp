#include "branch_context.h"
#include "working_tree.h"
#include "hash.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <map>
#include <memory>
#include <thread>
#include <vector>

using namespace cas;

static void test_clone_isolation() {
    WorkingTree original;
    Hash h1{}; h1[0] = 0xAA;
    original.insert("/a.txt", {EntryKind::Blob, h1, 0100644});
    original.insert("/b.txt", {EntryKind::Blob, h1, 0100644});

    BranchContext br1(1, "branch-1", original.clone());
    BranchContext br2(2, "branch-2", original.clone());

    // Mutate branch-1 only
    Hash h2{}; h2[0] = 0xBB;
    br1.wt.insert("/a.txt", {EntryKind::Blob, h2, 0100644});
    br1.wt.insert("/c.txt", {EntryKind::Blob, h2, 0100644});

    // branch-2 should be unaffected
    auto e = br2.wt.lookup("/a.txt");
    assert(e.has_value());
    assert(e->hash[0] == 0xAA);
    assert(!br2.wt.lookup("/c.txt").has_value());

    // original should be unaffected
    auto o = original.lookup("/a.txt");
    assert(o.has_value());
    assert(o->hash[0] == 0xAA);

    // branch-1 should have the mutation
    auto m = br1.wt.lookup("/a.txt");
    assert(m.has_value());
    assert(m->hash[0] == 0xBB);
    assert(br1.wt.lookup("/c.txt").has_value());

    std::printf("  PASS test_clone_isolation\n");
}

static void test_branch_metadata() {
    BranchContext br(42, "my-branch");
    assert(br.branch_id == 42);
    assert(br.name == "my-branch");
    assert(br.wt.size() == 0);
    std::printf("  PASS test_branch_metadata\n");
}

static void test_independent_checkpoints() {
    // Verify each branch has its own mutex (compiles and doesn't deadlock)
    BranchContext br1(1, "b1");
    BranchContext br2(2, "b2");
    {
        std::lock_guard<std::mutex> lk1(br1.checkpoint_mu);
        std::lock_guard<std::mutex> lk2(br2.checkpoint_mu);
        // Both locked simultaneously — no deadlock
    }
    std::printf("  PASS test_independent_checkpoints\n");
}

static void test_move_ctor_preserves_contents() {
    WorkingTree src;
    Hash h{}; h[0] = 0x42;
    src.insert("/x", {EntryKind::Blob, h, 0100644});
    src.insert("/y", {EntryKind::Blob, h, 0100644});
    assert(src.size() == 2);

    WorkingTree moved(std::move(src));
    // moved takes over the entries; src is left in a valid-but-empty state
    assert(moved.size() == 2);
    auto x = moved.lookup("/x");
    assert(x.has_value() && x->hash[0] == 0x42);
    // src should be empty (not a requirement of std, but of our impl)
    assert(src.size() == 0);
    std::printf("  PASS test_move_ctor_preserves_contents\n");
}

static void test_move_assign_self() {
    WorkingTree wt;
    Hash h{}; h[0] = 0x77;
    wt.insert("/a", {EntryKind::Blob, h, 0100644});
    WorkingTree& ref = wt;
    wt = std::move(ref);  // self-assignment
    assert(wt.size() == 1);
    auto a = wt.lookup("/a");
    assert(a.has_value() && a->hash[0] == 0x77);
    std::printf("  PASS test_move_assign_self\n");
}

static void test_move_assign_replaces() {
    WorkingTree a;
    Hash ha{}; ha[0] = 0xAA;
    a.insert("/old", {EntryKind::Blob, ha, 0100644});

    WorkingTree b;
    Hash hb{}; hb[0] = 0xBB;
    b.insert("/new1", {EntryKind::Blob, hb, 0100644});
    b.insert("/new2", {EntryKind::Blob, hb, 0100644});

    a = std::move(b);
    assert(a.size() == 2);
    assert(!a.lookup("/old").has_value());
    auto n = a.lookup("/new1");
    assert(n.has_value() && n->hash[0] == 0xBB);
    std::printf("  PASS test_move_assign_replaces\n");
}

static void test_concurrent_clone_and_mutate() {
    // Two clones of the same source, mutated from separate threads, must
    // remain isolated. Runs on every build; TSan-clean under our mutex
    // guards.
    WorkingTree src;
    Hash h{}; h[0] = 0x01;
    for (int i = 0; i < 100; i++) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "/f%d", i);
        src.insert(buf, {EntryKind::Blob, h, 0100644});
    }

    auto br1 = std::make_shared<BranchContext>(1, "b1", src.clone());
    auto br2 = std::make_shared<BranchContext>(2, "b2", src.clone());

    std::atomic<bool> go{false};
    auto mutator = [&](BranchContext& br, uint8_t tag) {
        while (!go.load(std::memory_order_acquire)) {}
        Hash mh{}; mh[0] = tag;
        for (int i = 0; i < 500; i++) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "/m%d_%d", tag, i);
            br.wt.insert(buf, {EntryKind::Blob, mh, 0100644});
        }
    };
    std::thread t1(mutator, std::ref(*br1), (uint8_t)0xAA);
    std::thread t2(mutator, std::ref(*br2), (uint8_t)0xBB);
    go.store(true, std::memory_order_release);
    t1.join(); t2.join();

    // br1 never sees br2's insertions and vice versa.
    for (int i = 0; i < 500; i++) {
        char buf1[16], buf2[16];
        std::snprintf(buf1, sizeof(buf1), "/m%d_%d", 0xAA, i);
        std::snprintf(buf2, sizeof(buf2), "/m%d_%d", 0xBB, i);
        assert(br1->wt.lookup(buf1).has_value());
        assert(!br1->wt.lookup(buf2).has_value());
        assert(br2->wt.lookup(buf2).has_value());
        assert(!br2->wt.lookup(buf1).has_value());
    }
    std::printf("  PASS test_concurrent_clone_and_mutate\n");
}

static void test_shared_ptr_outlives_map_erase() {
    // Simulate a caller holding a shared_ptr to a branch while another
    // code path "deletes" the map entry. The BranchContext must remain
    // valid for the caller's whole scope; this is the property the
    // Daemon::branches_ shared_ptr rework relies on.
    std::map<uint32_t, std::shared_ptr<BranchContext>> map;
    map[1] = std::make_shared<BranchContext>(1, "br", WorkingTree{});
    auto held = map[1];  // simulate a FUSE op that took a local copy
    Hash h{}; h[0] = 0x55;
    held->wt.insert("/before", {EntryKind::Blob, h, 0100644});

    map.erase(1);  // simulate delete_branch

    // held still valid; we can lock its mutex and mutate its WT
    {
        std::lock_guard<std::mutex> lk(held->checkpoint_mu);
        held->wt.insert("/after_erase", {EntryKind::Blob, h, 0100644});
    }
    assert(held->wt.lookup("/before").has_value());
    assert(held->wt.lookup("/after_erase").has_value());
    std::printf("  PASS test_shared_ptr_outlives_map_erase\n");
}

int main() {
    test_clone_isolation();
    test_branch_metadata();
    test_independent_checkpoints();
    test_move_ctor_preserves_contents();
    test_move_assign_self();
    test_move_assign_replaces();
    test_concurrent_clone_and_mutate();
    test_shared_ptr_outlives_map_erase();
    std::printf("PASS cas_test_branch_context\n");
    return 0;
}
