#include "branch_router.h"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace cas;

#define REQUIRE(expr) do { if (!(expr)) { \
    std::fprintf(stderr, "REQUIRE failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
    std::abort(); } } while (0)

// Fake cgroup universe: three cgroups under the v2 root plus a child of cg1.
// read_cgroup returns the /proc-relative path ("/cg1"); resolve() prepends
// "/sys/fs/cgroup" before asking cgroup_id for the directory id.
// The per-path counters include cgroup_id calls issued by register_cgroup;
// tests snapshot them after registration. read_calls counts membership
// reads — the hot-path cost being optimized.
struct FakeCgroups {
    std::string current = "/cg1";
    uint64_t cg1_id = 101;
    int cg1_calls = 0;
    int cg1_child_calls = 0;
    int read_calls = 0;
};

static BranchRouter::Hooks make_hooks(FakeCgroups* fake) {
    BranchRouter::Hooks hooks;
    hooks.cgroup_id = [fake](const std::string& path) -> uint64_t {
        if (path == "/sys/fs/cgroup/cg1") {
            fake->cg1_calls++;
            return fake->cg1_id;
        }
        if (path == "/sys/fs/cgroup/cg1/child") {
            fake->cg1_child_calls++;
            return 111;
        }
        if (path == "/sys/fs/cgroup/cg2") return 102;
        if (path == "/sys/fs/cgroup/cg3") return 103;
        if (path == "/sys/fs/cgroup") return 1;
        return 0;
    };
    hooks.read_cgroup = [fake](Pid) {
        fake->read_calls++;
        return fake->current;
    };
    return hooks;
}

static void test_resolve_routes_registered_cgroup() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);

    fake.current = "/elsewhere";
    REQUIRE(router.resolve(500) == 0);
}

static void test_resolve_follows_cgroup_migration() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg2", 2));
    REQUIRE(router.resolve(500) == 1);

    // The SAME live process migrates to the other registered cgroup, as
    // `echo $$ > .../cg2/cgroup.procs` does. The next resolve must follow;
    // only already-open handles stay pinned (FhState, not the router).
    fake.current = "/cg2";
    REQUIRE(router.resolve(500) == 2);
}

static void test_resolve_revalidates_recreated_cgroup_path() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);

    // The path is unchanged, but the original cgroup was deleted and a new
    // directory was created at the same path with a different inode.
    fake.cg1_id = 201;
    REQUIRE(router.resolve(500) == 0);
}

static void test_resolve_memoizes_path_walk() {
    FakeCgroups fake;
    fake.current = "/cg1/child";
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    int leaf_checks_before_first = fake.cg1_child_calls;
    int ancestor_walks_before_first = fake.cg1_calls;
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(fake.cg1_child_calls == leaf_checks_before_first + 1);
    REQUIRE(fake.cg1_calls == ancestor_walks_before_first + 1);
    int leaf_checks_after_first = fake.cg1_child_calls;
    int ancestor_walks_after_first = fake.cg1_calls;
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(router.resolve(600) == 1);  // different pid, same cgroup path
    REQUIRE(fake.cg1_child_calls == leaf_checks_after_first + 2);
    REQUIRE(fake.cg1_calls == ancestor_walks_after_first);
}

static void test_register_invalidates_path_cache() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    fake.current = "/cg2";
    REQUIRE(router.resolve(500) == 0);  // cg2 not registered yet; memoized
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg2", 2));
    REQUIRE(router.resolve(500) == 2);  // registration cleared the memo
}

static void test_unregister_routes_zero() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(router.unregister_cgroup("/sys/fs/cgroup/cg1"));
    REQUIRE(router.resolve(500) == 0);
}

static void test_read_failure_routes_zero() {
    FakeCgroups fake;
    fake.current = "";
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 0);
}

static void test_watch_mode_skips_leaf_checks_until_evicted() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    // The daemon disables per-resolve leaf revalidation when the cgroup
    // delete watch is active: external rmdir arrives as evict_cgroup_id().
    router.set_leaf_revalidation(false);
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);
    int leaf_checks_after_first = fake.cg1_calls;
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(router.resolve(600) == 1);
    REQUIRE(fake.cg1_calls == leaf_checks_after_first);  // warm hits: no stats

    // rmdir + mkdir at the same path (new inode). Without the per-resolve
    // stat the router cannot see this on its own; the watch reports it.
    fake.cg1_id = 201;
    router.evict_cgroup_id(101);
    REQUIRE(router.resolve(500) == 0);
}

static void test_evict_erases_registration_not_just_caches() {
    FakeCgroups fake;
    BranchRouter router(make_hooks(&fake));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);

    // The watched cgroup was deleted. If kernfs later recycles the same
    // inode number for an unrelated cgroup at this path, the dead
    // registration must not resurrect: eviction removes the map entry,
    // not merely the caches.
    router.evict_cgroup_id(101);
    REQUIRE(router.resolve(500) == 0);
}

struct FakeFence {
    uint64_t generation = 7;
    bool accept_track = true;
};

static void attach_fake_fence(BranchRouter& router, FakeFence* fence) {
    router.attach_fence(
        [fence] { return fence->generation; },
        [fence](Pid) { return fence->accept_track; });
}

static void test_fence_skips_membership_reads() {
    FakeCgroups fake;
    FakeFence fence;
    BranchRouter router(make_hooks(&fake));
    attach_fake_fence(router, &fence);
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);
    int reads_after_first = fake.read_calls;
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(fake.read_calls == reads_after_first);  // warm hits: no procfs
}

static void test_fence_generation_bump_forces_reread() {
    FakeCgroups fake;
    FakeFence fence;
    BranchRouter router(make_hooks(&fake));
    attach_fake_fence(router, &fence);
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg2", 2));
    REQUIRE(router.resolve(500) == 1);
    int reads_after_first = fake.read_calls;

    fake.current = "/cg2";        // the process migrated...
    fence.generation++;           // ...and the kernel bumped the counter
    REQUIRE(router.resolve(500) == 2);
    REQUIRE(fake.read_calls == reads_after_first + 1);  // exactly one re-read
}

static void test_fence_track_rejected_keeps_reading() {
    FakeCgroups fake;
    FakeFence fence;
    fence.accept_track = false;   // tracked_pids full: per-pid fallback
    BranchRouter router(make_hooks(&fake));
    attach_fake_fence(router, &fence);
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);
    int reads_after_first = fake.read_calls;
    REQUIRE(router.resolve(500) == 1);
    REQUIRE(fake.read_calls == reads_after_first + 1);  // still reads procfs
}

static void test_register_clears_fence_cache() {
    FakeCgroups fake;
    FakeFence fence;
    BranchRouter router(make_hooks(&fake));
    attach_fake_fence(router, &fence);
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg1", 1));
    REQUIRE(router.resolve(500) == 1);

    // Registration changes routing without any kernel-side migration, so
    // the fence generation does NOT change; the router must clear its own
    // fence cache on every registration-set mutation.
    fake.current = "/cg3";
    REQUIRE(router.register_cgroup("/sys/fs/cgroup/cg3", 3));
    REQUIRE(router.resolve(500) == 3);
}

int main() {
    test_resolve_routes_registered_cgroup();
    test_resolve_follows_cgroup_migration();
    test_resolve_revalidates_recreated_cgroup_path();
    test_resolve_memoizes_path_walk();
    test_register_invalidates_path_cache();
    test_unregister_routes_zero();
    test_read_failure_routes_zero();
    test_watch_mode_skips_leaf_checks_until_evicted();
    test_evict_erases_registration_not_just_caches();
    test_fence_skips_membership_reads();
    test_fence_generation_bump_forces_reread();
    test_fence_track_rejected_keeps_reading();
    test_register_clears_fence_cache();
    std::printf("PASS test_branch_router\n");
    return 0;
}
